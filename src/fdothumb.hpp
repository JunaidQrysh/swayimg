// SPDX-License-Identifier: MIT
// Freedesktop.org thumbnail cache reader.
// https://specifications.freedesktop.org/thumbnail-spec/latest/

#pragma once

#include "pixmap.hpp"

#include <filesystem>

namespace FdoThumb {

/**
 * Load a valid thumbnail for the file from the freedesktop thumbnail cache
 * ($XDG_CACHE_HOME/thumbnails/x-large).
 * The thumbnail is accepted only if its Thumb::URI and Thumb::MTime match
 * the origin file.
 * @param path path to the origin image file
 * @return thumbnail pixmap or empty pixmap if not found/invalid
 */
Pixmap load(const std::filesystem::path& path);

} // namespace FdoThumb
