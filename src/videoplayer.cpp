// SPDX-License-Identifier: MIT
// External video player launcher.

#include "videoplayer.hpp"

#include "log.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace VideoPlayer {

/**
 * Check if the image entry is a video file (by file extension).
 * @param entry image entry to check
 * @return true if entry is a video file
 */
bool is_video(const ImageEntryPtr& entry)
{
    static constexpr std::array<std::string_view, 17> extensions = {
        ".mp4", ".m4v", ".mkv", ".webm", ".mov",  ".avi",  ".wmv",
        ".flv", ".mpg", ".mpeg", ".ts",  ".m2ts", ".mts", ".3gp",
        ".ogv", ".vob", ".asf",
    };

    if (!entry || entry->is_special()) {
        return false;
    }

    std::string ext = entry->path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return std::ranges::find(extensions, ext) != extensions.end();
}

/**
 * Open file in an external video player (mpv), detached from swayimg.
 * @param path path to the file to open
 */
void open(const std::filesystem::path& path)
{
    const pid_t pid = fork();
    if (pid == -1) {
        Log::error(errno, "Unable to fork");
        return;
    }
    if (pid == 0) {
        // double fork: the player is reparented to init, no zombies left
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
        execlp("mpv", "mpv", "--", path.c_str(), nullptr);
        _exit(1);
    }
    waitpid(pid, nullptr, 0);
}

} // namespace VideoPlayer
