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
 * @param orig output size of the origin image (Thumb::Image::Width/Height),
 *             zero size if not specified in the thumbnail
 * @return thumbnail pixmap or empty pixmap if not found/invalid
 */
Pixmap load(const std::filesystem::path& path, Size* orig = nullptr);

} // namespace FdoThumb
