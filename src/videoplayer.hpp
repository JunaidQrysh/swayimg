// SPDX-License-Identifier: MIT
// External video player launcher.

#pragma once

#include "image.hpp"

#include <filesystem>

namespace VideoPlayer {

/**
 * Check if the image entry is a video file (by file extension).
 * @param entry image entry to check
 * @return true if entry is a video file
 */
bool is_video(const ImageEntryPtr& entry);

/**
 * Check if the file is a video file (by file extension).
 * @param path file path
 * @return true if file is a video file
 */
bool is_video(const std::filesystem::path& path);

/**
 * Open file in an external video player (mpv), detached from swayimg.
 * @param path path to the file to open
 */
void open(const std::filesystem::path& path);

} // namespace VideoPlayer
