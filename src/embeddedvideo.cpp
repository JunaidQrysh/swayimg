// SPDX-License-Identifier: MIT
// Embedded video playback (libmpv rendering into OpenGL texture).

#include "embeddedvideo.hpp"

#include "application.hpp"
#include "buildconf.hpp"
#include "log.hpp"

#ifdef HAVE_LIBMPV
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <format>
#include <vector>

namespace {

void* get_proc_address(void*, const char* name)
{
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

} // namespace

bool EmbeddedVideo::available()
{
    const Ui* ui = Application::get_ui();
    return ui && ui->is_gpu();
}

EmbeddedVideo::~EmbeddedVideo()
{
    // GL context is current in the main (rendering) thread
    if (render_ctx) {
        mpv_render_context_free(render_ctx);
    }
    if (mpv) {
        mpv_terminate_destroy(mpv);
    }
    if (fbo) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
    }
}

bool EmbeddedVideo::init()
{
    if (mpv) {
        return true;
    }

    // libmpv refuses to work with non-C numeric locale
    std::setlocale(LC_NUMERIC, "C");

    mpv = mpv_create();
    if (!mpv) {
        Log::error("Unable to create mpv instance");
        return false;
    }

    const std::pair<const char*, const char*> options[] = {
        { "vo",                     "libmpv"    },
        { "hwdec",                  "auto-safe" },
        { "keep-open",              "yes"       },
        { "idle",                   "yes"       },
        { "terminal",               "no"        },
        { "osc",                    "yes"       },
        { "input-default-bindings", "no"        },
        { "input-vo-keyboard",      "no"        },
        { "audio-client-name",      "swayimg"   },
        { "msg-level",              "all=warn"  },
    };
    for (const auto& [name, value] : options) {
        mpv_set_option_string(mpv, name, value);
    }

    // cache compiled shaders: avoids freeze on the first rendered frame
    std::filesystem::path cache;
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (xdg_cache && *xdg_cache) {
        cache = std::filesystem::path(xdg_cache) / "swayimg" / "mpv";
    } else if (home && *home) {
        cache = std::filesystem::path(home) / ".cache" / "swayimg" / "mpv";
    }
    if (!cache.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache, ec);
        if (!ec) {
            mpv_set_option_string(mpv, "gpu-shader-cache-dir",
                                  cache.string().c_str());
        }
    }

    if (mpv_initialize(mpv) < 0) {
        Log::error("Unable to initialize mpv");
        mpv_terminate_destroy(mpv);
        mpv = nullptr;
        return false;
    }

    // property values are delivered as events: never block on mpv core
    mpv_observe_property(mpv, 0, "percent-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "hwdec-current", MPV_FORMAT_STRING);

    return true;
}

bool EmbeddedVideo::prepare()
{
    if (!init()) {
        return false;
    }
    if (!render_ctx && !render_failed && !create_render_context()) {
        render_failed = true;
        return false;
    }
    return true;
}

bool EmbeddedVideo::start(const std::filesystem::path& path)
{
    if (!init()) {
        return false;
    }

    process_events(); // drop events of the previous file
    playing = true;
    frame_seen = false;
    started = false;
    percent_pos = 0;

    if (render_ctx) {
        const std::string file = path.string();
        const char* cmd[] = { "loadfile", file.c_str(), nullptr };
        mpv_command_async(mpv, 0, cmd);
    } else {
        // file is loaded when render context is created (on first frame)
        pending = path.string();
    }

    return true;
}

void EmbeddedVideo::stop()
{
    if (mpv && playing) {
        const char* cmd[] = { "stop", nullptr };
        mpv_command_async(mpv, 0, cmd);
    }
    playing = false;
    frame_seen = false;
    started = false;
    pending.clear();
}

unsigned EmbeddedVideo::render(const Size& size)
{
    if (!mpv || render_failed || !size) {
        return 0;
    }

    process_events();

    if (!playing) {
        return 0;
    }

    if (!render_ctx && !create_render_context()) {
        render_failed = true;
        return 0;
    }

    update = false;
    const uint64_t flags = mpv_render_context_update(render_ctx);
    if (started && (flags & MPV_RENDER_UPDATE_FRAME)) {
        frame_seen = true; // first frame of the current file
    }
    if (!frame_seen) {
        return 0; // no video frame decoded yet
    }

    bool redraw = flags & MPV_RENDER_UPDATE_FRAME;

    // (re)allocate render target
    if (!fbo || size.width != texture_size.width ||
        size.height != texture_size.height) {
        if (!fbo) {
            glGenFramebuffers(1, &fbo);
            glGenTextures(1, &texture);
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width, size.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture, 0);
        texture_size = size;
        redraw = true;
    }

    if (redraw) {
        mpv_opengl_fbo target {
            .fbo = static_cast<int>(fbo),
            .w = static_cast<int>(size.width),
            .h = static_cast<int>(size.height),
            .internal_format = 0,
        };
        int flip_y = 1; // bottom-up texture, as other GL targets
        int block = 0;
        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO,             &target  },
            { MPV_RENDER_PARAM_FLIP_Y,                 &flip_y  },
            { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME,  &block   },
            { MPV_RENDER_PARAM_INVALID,                nullptr },
        };
        mpv_render_context_render(render_ctx, params);
    }

    return texture;
}

void EmbeddedVideo::mouse_move(const double rel_x, const double rel_y)
{
    if (mpv && texture_size) {
        const std::string x =
            std::format("{}", static_cast<int>(rel_x * texture_size.width));
        const std::string y =
            std::format("{}", static_cast<int>(rel_y * texture_size.height));
        const char* cmd[] = { "mouse", x.c_str(), y.c_str(), nullptr };
        mpv_command_async(mpv, 0, cmd);
    }
}

void EmbeddedVideo::mouse_button(const char* name, const bool pressed)
{
    if (mpv) {
        const char* cmd[] = { pressed ? "keydown" : "keyup", name, nullptr };
        mpv_command_async(mpv, 0, cmd);
    }
}

void EmbeddedVideo::toggle_pause()
{
    if (mpv) {
        const char* cmd[] = { "cycle", "pause", nullptr };
        mpv_command_async(mpv, 0, cmd);
        Application::redraw();
    }
}

void EmbeddedVideo::seek(const double seconds)
{
    if (mpv) {
        const std::string value = std::format("{}", seconds);
        const char* cmd[] = { "seek", value.c_str(), "relative", nullptr };
        mpv_command_async(mpv, 0, cmd);
    }
}

void EmbeddedVideo::change_volume(const double delta)
{
    if (mpv) {
        const std::string value = std::format("{}", delta);
        const char* cmd[] = { "add", "volume", value.c_str(), nullptr };
        mpv_command_async(mpv, 0, cmd);
    }
}

double EmbeddedVideo::progress()
{
    process_events();
    return percent_pos / 100.0;
}

bool EmbeddedVideo::paused()
{
    process_events();
    return is_paused;
}

void EmbeddedVideo::process_events()
{
    while (mpv) {
        const mpv_event* event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && playing) {
            started = true;
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            const auto* prop = static_cast<mpv_event_property*>(event->data);
            if (prop->format == MPV_FORMAT_DOUBLE &&
                std::strcmp(prop->name, "percent-pos") == 0) {
                percent_pos = *static_cast<double*>(prop->data);
            } else if (prop->format == MPV_FORMAT_FLAG &&
                       std::strcmp(prop->name, "pause") == 0) {
                is_paused = *static_cast<int*>(prop->data);
            } else if (prop->format == MPV_FORMAT_STRING &&
                       std::strcmp(prop->name, "hwdec-current") == 0) {
                Log::verbose("Video hardware decoding: {}",
                             *static_cast<char**>(prop->data));
            }
        }
    }
}

bool EmbeddedVideo::create_render_context()
{
    mpv_opengl_init_params gl_init {
        .get_proc_address = &get_proc_address,
        .get_proc_address_ctx = nullptr,
    };

    std::vector<mpv_render_param> params = {
        { MPV_RENDER_PARAM_API_TYPE,
         const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
    };
    const Ui* ui = Application::get_ui();
    void* display = ui ? ui->native_display() : nullptr;
    if (display) {
        // required for hardware decoding (VA-API interop)
        params.push_back({ MPV_RENDER_PARAM_WL_DISPLAY, display });
    }
    params.push_back({ MPV_RENDER_PARAM_INVALID, nullptr });

    if (mpv_render_context_create(&render_ctx, mpv, params.data()) < 0) {
        Log::error("Unable to create mpv render context");
        render_ctx = nullptr;
        return false;
    }
    mpv_render_context_set_update_callback(render_ctx, &on_update, this);

    // now video output is ready, load file
    if (!pending.empty()) {
        const char* cmd[] = { "loadfile", pending.c_str(), nullptr };
        mpv_command_async(mpv, 0, cmd);
        pending.clear();
    }

    return true;
}

void EmbeddedVideo::on_update(void* ctx)
{
    // called from mpv thread: just schedule redraw
    static_cast<EmbeddedVideo*>(ctx)->update = true;
    Application::redraw();
}

#else // HAVE_LIBMPV

bool EmbeddedVideo::available()
{
    return false;
}

EmbeddedVideo::~EmbeddedVideo() = default;

bool EmbeddedVideo::start(const std::filesystem::path&)
{
    return false;
}

unsigned EmbeddedVideo::render(const Size&)
{
    return 0;
}

void EmbeddedVideo::stop() {}
void EmbeddedVideo::mouse_move(const double, const double) {}
void EmbeddedVideo::mouse_button(const char*, const bool) {}
bool EmbeddedVideo::init()
{
    return false;
}
bool EmbeddedVideo::prepare()
{
    return false;
}
void EmbeddedVideo::toggle_pause() {}
void EmbeddedVideo::seek(const double) {}
void EmbeddedVideo::change_volume(const double) {}

double EmbeddedVideo::progress()
{
    return 0;
}

bool EmbeddedVideo::paused()
{
    return false;
}

void EmbeddedVideo::process_events() {}

bool EmbeddedVideo::create_render_context()
{
    return false;
}

void EmbeddedVideo::on_update(void*) {}

#endif // HAVE_LIBMPV
