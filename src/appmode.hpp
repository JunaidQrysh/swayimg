// SPDX-License-Identifier: MIT
// Generic application mode interface.
// Copyright (C) 2025 Artem Senichev <artemsen@gmail.com>

#pragma once

#include "image.hpp"
#include "input.hpp"
#include "text.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <vector>

class AppMode {
public:
    /** Application mode types. */
    enum Type : uint8_t {
        Viewer,
        Slideshow,
        Gallery,
    };

    /** Image list event types. */
    enum class ImageListEvent : uint8_t {
        Create,
        Modify,
        Remove,
    };

    /** Input event handler. */
    using InputCallback = std::function<void()>;

    /** Scroll event handler. */
    using ScrollCallback = std::function<void(keymod_t, double, double)>;

    /** Unassigned kay input event handler. */
    using UnassignedKeyCallback =
        std::function<void(const InputKeyboard& input)>;

    /** Constructor. */
    AppMode();

    /**
     * Initialize mode instance.
     */
    virtual void initialize() = 0;

    /**
     * Activate mode.
     * @param entry image entry to show/select
     * @param wnd window size
     */
    virtual void activate(const ImageEntryPtr& entry, const Size& wnd) = 0;

    /**
     * Deactivate mode.
     */
    virtual void deactivate() = 0;

    /**
     * Get currently showed/selected image entry.
     * @return current image entry
     */
    virtual ImageEntryPtr get_current() = 0;

    /**
     * Set currently showed/selected image entry.
     * @param entry desired image entry
     * @return true if image was opened/selected
     */
    virtual bool set_current(const ImageEntryPtr& entry) = 0;

    /**
     * Window resize handler.
     * @param wnd window size
     */
    virtual void window_resize(const Size& /*wnd*/) {}

    /**
     * Window resize handler.
     * @param wnd window surface pixmap
     */
    virtual void window_redraw(Canvas& wnd) = 0;

    /**
     * Check if mode is ready to draw a frame. Used to postpone the first
     * frame until the essential content is loaded.
     * @return false to skip redraw (the previous frame stays on the screen)
     */
    virtual bool ready_to_draw() { return true; }

    /**
     * Advance animations, called before each window redraw.
     * @param dt time since previous frame in seconds
     * @return true if animation is in progress and next frame is required
     */
    virtual bool animate(const double /*dt*/) { return false; }

    /**
     * Handle key press event.
     * @param input input event description
     */
    void handle_keyboard(const InputKeyboard& input);

    /**
     * Handle mouse/touchpad scroll.
     * @param kmods keyboard modifiers state
     * @param delta_h,delta_v horizontal and vertical scroll delta
     * @param finger true if scroll source is a finger on touchpad
     * @param stop true if scrolling stopped (finger lifted)
     * @param time event timestamp in milliseconds
     */
    void handle_scroll(const keymod_t kmods, const double delta_h,
                       const double delta_v, const bool finger,
                       const bool stop, const uint32_t time);

    /**
     * Handle mouse click.
     * @param input input event description
     * @param pos mouse pointer coordinates
     */
    void handle_mclick(const InputMouse& input, const Point& pos);

    /**
     * Handle mouse move.
     * @param input input event description
     * @param pos mouse pointer coordinates
     * @param delta diff between this and last call
     */
    virtual void handle_mmove(const InputMouse& input, const Point& pos,
                              const Point& delta) = 0;

    /**
     * Handle pinch gesture.
     * @param scale_delta scale diff
     */
    virtual void handle_pinch(const double scale_delta, const double dx,
                              const double dy) = 0;

    /**
     * Handle end of pinch gesture.
     */
    virtual void handle_pinch_end() {}

    /**
     * Handle raw mouse button event before key bindings.
     * @param button changed button
     * @param pressed true if pressed, false if released
     * @param pos mouse pointer coordinates
     * @return true if event was consumed
     */
    virtual bool handle_mbutton(const InputMouse::mouse_btn_t /*button*/,
                                const bool /*pressed*/, const Point& /*pos*/)
    {
        return false;
    }

    /**
     * Handle signal.
     * @param input input event description
     * @return false if event not supported
     */
    virtual bool handle_signal(const InputSignal& input);

    /**
     * Handle image list changes.
     * @param event event type
     * @param entries array of added/removed/updated image entries
     */
    virtual void handle_imagelist(const ImageListEvent event,
                                  const std::vector<ImageEntryPtr>& entries);

    /**
     * Check if current mode is active.
     * @return true if current mode is active
     */
    [[nodiscard]] bool is_active() const;

    /**
     * Set or toggle mark state for current image.
     * @param state state to set, `std::nullopt` to toggle
     */
    void mark_current(const std::optional<bool>& state);

    /**
     * Set mark icon color.
     * @param color mark icon color
     */
    void set_mark_color(const argb_t& color);

    /**
     * Set the pinch gesture factor.
     * @param factor pinch gesture factor
     */
    void set_pinch_factor(const double factor);

    /**
     * Set text layer scheme.
     * @param pos block position
     * @param scheme scheme description
     */
    void set_text_scheme(const Text::Position pos, const Text::Scheme& scheme);

    /**
     * Remove all bindings: keyboard/mouse/signals.
     */
    void bind_reset();

    /**
     * Bind key press event.
     * @param input event description
     * @param handler callback
     */
    void bind_input(const InputKeyboard& input, const InputCallback& handler);

    /**
     * Bind mouse click event.
     * @param input event description
     * @param handler callback
     */
    void bind_input(const InputMouse& input, const InputCallback& handler);

    /**
     * Bind signal event.
     * @param input event description
     * @param handler callback
     */
    void bind_input(const InputSignal& input, const InputCallback& handler);

public:
    std::function<void()> on_image_change;   ///< Image switch callback
    UnassignedKeyCallback on_unassigned_key; ///< Unassigned key handler
    ScrollCallback on_scroll;                ///< Scroll handler

protected:
    /**
     * Switch to another image.
     */
    virtual void switch_current();

    /**
     * Handle end of touchpad scroll (finger lifted).
     */
    virtual void handle_scroll_stop() {}

    /**
     * Draw placeholder for empty image list.
     * @param wnd window surface pixmap
     * @param bkg window background color
     */
    static void draw_empty(Canvas& wnd, const argb_t bkg);

protected:
    argb_t mark_color;                       ///< Mark icon color
    bool scroll_finger = false; ///< Current scroll comes from touchpad
    uint32_t scroll_time = 0;   ///< Current scroll event timestamp (ms)
    double pinch_factor;                     ///< Pinch gesture factor
    std::array<Text::Scheme, 4> text_scheme; ///< Text layer scheme

private:
    std::map<InputKeyboard, InputCallback> kbindings; ///< Keyboard bindings
    std::map<InputMouse, InputCallback> mbindings;    ///< Mouse bindings
    std::map<InputSignal, InputCallback> sbindings;   ///< Signal bindings
};
