// SPDX-License-Identifier: MIT
// Folder selection dialog (XDG desktop portal, zenity fallback).

#include "folderpicker.hpp"

#include "application.hpp"
#include "buildconf.hpp"
#include "log.hpp"

#ifdef HAVE_SDBUS
#include <systemd/sd-bus.h>
#endif

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <random>
#include <thread>

namespace {

/** Dialog is open. */
std::atomic<bool> busy = false;

/** Decode file:// URI to path. */
std::optional<std::filesystem::path> uri_to_path(const std::string& uri)
{
    constexpr std::string_view prefix = "file://";
    if (!uri.starts_with(prefix)) {
        return std::nullopt;
    }
    std::string path;
    for (size_t i = prefix.size(); i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const std::string hex = uri.substr(i + 1, 2);
            char* end = nullptr;
            const long chr = std::strtol(hex.c_str(), &end, 16);
            if (end == hex.c_str() + 2) {
                path += static_cast<char>(chr);
                i += 2;
                continue;
            }
        }
        path += uri[i];
    }
    return path;
}

#ifdef HAVE_SDBUS

/** Portal request state. */
struct PortalRequest {
    bool done = false;
    std::vector<std::filesystem::path> paths;
};

/** Handler of org.freedesktop.portal.Request::Response signal. */
int on_response(sd_bus_message* msg, void* data, sd_bus_error*)
{
    PortalRequest& req = *static_cast<PortalRequest*>(data);
    req.done = true;

    uint32_t response = 2;
    if (sd_bus_message_read(msg, "u", &response) < 0 || response != 0) {
        return 0; // canceled or failed
    }

    if (sd_bus_message_enter_container(msg, 'a', "{sv}") < 0) {
        return 0;
    }
    while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
        const char* key = nullptr;
        if (sd_bus_message_read(msg, "s", &key) < 0) {
            break;
        }
        if (key && strcmp(key, "uris") == 0 &&
            sd_bus_message_enter_container(msg, 'v', "as") > 0) {
            char** uris = nullptr;
            if (sd_bus_message_read_strv(msg, &uris) >= 0 && uris) {
                for (char** it = uris; *it; ++it) {
                    const auto path = uri_to_path(*it);
                    if (path) {
                        req.paths.push_back(*path);
                    }
                    free(*it);
                }
                free(static_cast<void*>(uris));
            }
            sd_bus_message_exit_container(msg);
        } else {
            sd_bus_message_skip(msg, "v");
        }
        sd_bus_message_exit_container(msg);
    }
    return 0;
}

/**
 * Select folders using XDG desktop portal (FileChooser.OpenFile).
 * @param title dialog title
 * @return selected folders, nullopt if portal is not available
 */
std::optional<std::vector<std::filesystem::path>>
portal_pick(const std::string& title)
{
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) {
        return std::nullopt;
    }

    std::optional<std::vector<std::filesystem::path>> result;
    sd_bus_slot* slot = nullptr;
    sd_bus_message* call = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    PortalRequest req;

    // expected request object path: subscribe before the call (no race)
    const char* unique = nullptr;
    sd_bus_get_unique_name(bus, &unique);
    std::string sender = unique ? unique + 1 : "";
    std::ranges::replace(sender, '.', '_');
    static std::mt19937 rng(std::random_device {}());
    const std::string token = std::format("swayimg{}", rng());
    std::string handle = std::format(
        "/org/freedesktop/portal/desktop/request/{}/{}", sender, token);

    const auto subscribe = [&](const std::string& path) {
        sd_bus_slot_unref(slot);
        slot = nullptr;
        return sd_bus_match_signal(bus, &slot, "org.freedesktop.portal.Desktop",
                                   path.c_str(),
                                   "org.freedesktop.portal.Request",
                                   "Response", on_response, &req) >= 0;
    };

    if (!subscribe(handle) ||
        sd_bus_message_new_method_call(
            bus, &call, "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.FileChooser", "OpenFile") < 0 ||
        sd_bus_message_append(call, "ss", "", title.c_str()) < 0 ||
        sd_bus_message_open_container(call, 'a', "{sv}") < 0 ||
        sd_bus_message_append(call, "{sv}", "handle_token", "s",
                              token.c_str()) < 0 ||
        sd_bus_message_append(call, "{sv}", "directory", "b", 1) < 0 ||
        sd_bus_message_append(call, "{sv}", "multiple", "b", 1) < 0 ||
        sd_bus_message_append(call, "{sv}", "modal", "b", 1) < 0 ||
        sd_bus_message_append(call, "{sv}", "accept_label", "s", "Add") < 0 ||
        sd_bus_message_close_container(call) < 0) {
        goto done;
    }

    if (sd_bus_call(bus, call, 0, &error, &reply) < 0) {
        Log::verbose("Portal file chooser unavailable: {}",
                     error.message ? error.message : "unknown error");
        goto done;
    }
    {
        // old portals return a different handle
        const char* path = nullptr;
        if (sd_bus_message_read(reply, "o", &path) >= 0 && path &&
            handle != path) {
            handle = path;
            subscribe(handle);
        }
    }

    // wait for the dialog to close
    while (!req.done) {
        const int rc = sd_bus_process(bus, nullptr);
        if (rc < 0) {
            break;
        }
        if (rc == 0 && sd_bus_wait(bus, UINT64_MAX) < 0) {
            break;
        }
    }
    if (req.done) {
        result = std::move(req.paths);
    }

done:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(call);
    sd_bus_slot_unref(slot);
    sd_bus_flush_close_unref(bus);
    return result;
}
#endif // HAVE_SDBUS

/**
 * Select folders using zenity.
 * @param title dialog title
 * @return selected folders, nullopt if zenity is not available
 */
std::optional<std::vector<std::filesystem::path>>
zenity_pick(const std::string& title)
{
    if (access("/usr/bin/zenity", X_OK) != 0 &&
        access("/usr/local/bin/zenity", X_OK) != 0) {
        return std::nullopt;
    }

    std::string quoted;
    for (const char c : title) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    const std::string cmd = std::format(
        "zenity --file-selection --directory --multiple "
        "--separator='\n' --title='{}' 2>/dev/null",
        quoted);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> paths;
    std::string line;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        line += buf;
    }
    pclose(pipe);

    size_t start = 0;
    while (start < line.size()) {
        size_t end = line.find('\n', start);
        if (end == std::string::npos) {
            end = line.size();
        }
        if (end > start) {
            paths.emplace_back(line.substr(start, end - start));
        }
        start = end + 1;
    }
    return paths;
}

} // namespace

bool FolderPicker::pick(const std::string& title, const Handler& handler)
{
    if (busy.exchange(true)) {
        return false;
    }

    std::thread([title, handler] {
        std::optional<std::vector<std::filesystem::path>> paths;
#ifdef HAVE_SDBUS
        paths = portal_pick(title);
#endif
        if (!paths) {
            paths = zenity_pick(title);
        }
        if (!paths) {
            Log::error("No folder chooser available: install "
                       "xdg-desktop-portal or zenity");
            paths.emplace();
        }

        busy = false;
        Application::self().add_event(AppEvent::Callback {
            [handler, result = std::move(*paths)] {
                handler(result);
            } });
    }).detach();

    return true;
}
