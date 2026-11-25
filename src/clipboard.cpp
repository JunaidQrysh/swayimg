// SPDX-License-Identifier: MIT
// Clipboard access (via wl-copy).

#include "clipboard.hpp"

#include "application.hpp"
#include "log.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

bool Clipboard::copy(const std::string& text)
{
    // native Wayland selection, works with any clipboard manager
    Ui* ui = Application::get_ui();
    if (ui && ui->set_clipboard(text)) {
        return true;
    }

    // fallback (e.g. DRM backend): external tool
    if (access("/usr/bin/wl-copy", X_OK) != 0 &&
        access("/usr/local/bin/wl-copy", X_OK) != 0) {
        Log::error("Unable to copy: wl-copy not found (install wl-clipboard)");
        return false;
    }

    const pid_t pid = fork();
    if (pid == -1) {
        Log::error(errno, "Unable to fork");
        return false;
    }
    if (pid == 0) {
        // double fork: wl-copy keeps serving the clipboard, no zombies left
        setsid();
        if (fork() != 0) {
            _exit(0);
        }
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd != -1) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execlp("wl-copy", "wl-copy", "--", text.c_str(), nullptr);
        _exit(1);
    }
    waitpid(pid, nullptr, 0);
    return true;
}
