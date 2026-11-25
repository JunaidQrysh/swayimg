// SPDX-License-Identifier: MIT
// Hardware (VAAPI) decoding of still images.

#pragma once

#include "imageformat.hpp"
#include "pixmap.hpp"

namespace HwDecode {

/**
 * Initialize hardware in advance (slow, call from a background thread).
 */
void warmup();

/**
 * Decode HEIF image stored as a grid of HEVC tiles (phone cameras).
 * @param data raw HEIF data
 * @param pm output pixmap
 * @return false if hardware decoding is not possible
 */
bool heif_grid(const ImageFormat::Data& data, Pixmap& pm);

/**
 * Decode baseline JPEG image.
 * @param data raw JPEG data
 * @param pm output pixmap
 * @param orientation EXIF orientation to apply (0 or 1: none)
 * @return false if hardware decoding is not possible
 */
bool jpeg(const ImageFormat::Data& data, Pixmap& pm, const int orientation);

} // namespace HwDecode
