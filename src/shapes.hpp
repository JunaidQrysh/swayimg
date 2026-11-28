// SPDX-License-Identifier: MIT
// Antialiased shape masks for UI overlays.

#pragma once

#include "pixmap.hpp"

namespace Shapes {

/** Line icons. */
enum class Icon : uint8_t {
    Filter, ///< Three lines of decreasing length
    Check,  ///< Check mark
    All,    ///< Two stacked frames with check mark
    Photo,  ///< Picture frame with mountains and sun
    Video,  ///< Video camera
    Sort,   ///< Up and down arrows
    Clock,  ///< Clock face
    Alpha,  ///< Letter A with down arrow
    Stack,  ///< Stacked frames
};

/**
 * Get rounded rectangle mask.
 * @param size mask size
 * @param radius corner radius
 * @return grayscale mask
 */
const Pixmap& rounded_rect(const Size& size, const size_t radius);

/**
 * Get icon mask.
 * @param icon icon type
 * @param size icon size (square)
 * @return grayscale mask
 */
const Pixmap& icon(const Icon icon, const size_t size);

/**
 * Get rounded rectangle mask with fully rounded ends (pill).
 * Masks are cached by size; the reference stays valid until the cache is
 * trimmed, so use it immediately.
 * @param size mask size
 * @return grayscale mask
 */
const Pixmap& pill(const Size& size);

/**
 * Get isosceles triangle mask (arrow head).
 * @param size mask size
 * @param up true for triangle pointing up, false for down
 * @return grayscale mask
 */
const Pixmap& triangle(const Size& size, const bool up);

} // namespace Shapes
