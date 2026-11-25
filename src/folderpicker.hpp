// SPDX-License-Identifier: MIT
// Folder selection dialog (XDG desktop portal, zenity fallback).

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace FolderPicker {

/** Selection result handler (empty list if canceled). */
using Handler = std::function<void(const std::vector<std::filesystem::path>&)>;

/**
 * Show folder selection dialog without blocking the application.
 * The handler is called from the main thread when the dialog is closed.
 * @param title dialog title
 * @param handler result handler
 * @return false if another dialog is already open
 */
bool pick(const std::string& title, const Handler& handler);

} // namespace FolderPicker
