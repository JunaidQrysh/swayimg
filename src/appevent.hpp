// SPDX-License-Identifier: MIT
// Application event.
// Copyright (C) 2026 Artem Senichev <artemsen@gmail.com>

#pragma once

#include "geometry.hpp"
#include "input.hpp"

#include <filesystem>
#include <functional>
#include <variant>
#include <vector>

/** Application event. */
namespace AppEvent {

/** Window close event. */
struct WindowClose {};

/** Window redraw event. */
struct WindowRedraw {};

/** Window resize event. */
struct WindowResize {
    Size size; ///< New size of the window
};

/** Window rescale event. */
struct WindowRescale {
    double scale; ///< New scale factor of the window
};

/** Key press event. */
struct KeyPress {
    InputKeyboard key; ///< Key description
};

/** Mouse/touchpad scroll event. */
struct Scroll {
    keymod_t kmods; ///< Keyboard modifiers
    double delta_h; ///< Horizontal scrolling delta
    double delta_v; ///< Vertival scrolling delta
    bool finger;    ///< Scroll source is a finger on touchpad
    bool stop;      ///< Scrolling stopped (finger lifted)
    uint32_t time;  ///< Event timestamp in milliseconds
};

/** Mouse clock event. */
struct MouseClick {
    InputMouse mouse;                ///< Mouse key state
    Point pointer;                   ///< Mouse pointer coordinates
    InputMouse::mouse_btn_t button;  ///< Changed button
    bool pressed;                    ///< Button pressed (false: released)
};

/** Mouse move event. */
struct MouseMove {
    InputMouse mouse; ///< Mouse key state
    Point pointer;    ///< Mouse pointer coordinates within the window
    Point delta;      ///< Delta between this and last call
};

/** Gesture: pinch event. */
struct GesturePinch {
    double scale_delta; ///< Scale diff
    double dx, dy;      ///< Movement of the gesture center (pixels)
    bool end;           ///< Gesture finished
};

/** Signal occur event. */
struct Signal {
    InputSignal signal; ///< Signal description
};

/** File drop event (drag-and-drop receive). */
struct DragAndDrop {
    std::vector<std::filesystem::path> paths; ///< Paths to the files
};

/** File create event. */
struct FileCreate {
    std::filesystem::path path; ///< Path to the file
};

/** File change event. */
struct FileModify {
    std::filesystem::path path; ///< Path to the file
};

/** File remove event. */
struct FileRemove {
    std::filesystem::path path; ///< Path to the file
};

// clang-format off
using Holder = std::variant<WindowClose,
                            WindowRedraw,
                            WindowResize,
                            WindowRescale,
                            KeyPress,
                            Scroll,
                            MouseClick,
                            MouseMove,
                            GesturePinch,
                            Signal,
                            DragAndDrop,
                            FileCreate,
                            FileModify,
                            FileRemove>;
// clang-format on

// event handler
using Handler = std::function<void(const Holder&)>;
};
