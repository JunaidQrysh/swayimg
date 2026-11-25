// SPDX-License-Identifier: MIT
// Clipboard access (via wl-copy).

#pragma once

#include <string>

namespace Clipboard {

/**
 * Copy text to the clipboard.
 * @param text text to copy
 * @return false if clipboard tool can't be started
 */
bool copy(const std::string& text);

} // namespace Clipboard
