// SPDX-License-Identifier: MIT
// Thumbnail layout for gallery mode.
// Copyright (C) 2025 Artem Senichev <artemsen@gmail.com>

#pragma once

#include "image.hpp"

/**
 * Thumbnail layout.
 * All images form a virtual grid; the window shows a part of it defined by
 * the vertical scroll offset (in pixels). The scroll offset can be animated
 * (smooth scrolling and kinetic scrolling).
 */
class Layout {
public:
    /** Selection direction. */
    enum Direction : uint8_t {
        First,
        Last,
        Up,
        Down,
        Left,
        Right,
        PgUp,
        PgDown,
    };

    /** Thumbnail instance with coordinates. */
    struct Thumbnail {
        ImageEntryPtr img; ///< Image entry
        size_t col, row;   ///< Column and row (relative to first visible row)
        Point pos;         ///< Window coordinates
    };

    /** Constructor. */
    Layout();

    /**
     * Update layout: recalculate thumbnails scheme.
     */
    void update();

    /**
     * Set output window size (resize layout).
     * @param wnd size of output window
     */
    void set_window_size(const Size& size);

    /**
     * Set thumbnail size.
     * @param size new thumbnail size in pixels
     */
    void set_thumb_size(const size_t size);

    /**
     * Get thumbnail size.
     * @return thumbnail size in pixels
     */
    [[nodiscard]] size_t get_thumb_size() const { return thumb_size; }

    /**
     * Set padding size.
     * @param padding new padding size in pixels
     */
    void set_padding(const size_t padding);

    /**
     * Get number of columns.
     * @return number of thumbnail columns
     */
    [[nodiscard]] size_t get_columns() const { return columns; }

    /**
     * Get number of fully visible rows.
     * @return number of thumbnail rows
     */
    [[nodiscard]] size_t get_rows() const { return rows; }

    /**
     * Select image and scroll to center it.
     * @param image image entry to select
     */
    void select(const ImageEntryPtr& image);

    /**
     * Select image keeping the current scroll position if the image is
     * visible (e.g. when returning from the viewer).
     * @param image image entry to select
     */
    void select_keep_scroll(const ImageEntryPtr& image);

    /**
     * Move selection.
     * @param dir movement direction
     * @return true if selection was changed
     */
    bool select(const Direction dir);

    /**
     * Select thumbnail at specified window coordinates.
     * @param pos window coordinates
     * @return true if selection was changed
     */
    bool select(const Point& pos);

    /**
     * Get thumbnail at specified window coordinates.
     * @param pos window coordinates
     * @return pointer to thumbnail or nullptr if no thumbnail at position
     */
    [[nodiscard]] const Thumbnail* at(const Point& pos) const;

    /**
     * Get currently selected image entry.
     * @return selected image entry
     */
    [[nodiscard]] ImageEntryPtr get_selected() const;

    /**
     * Check if image is visible.
     * @param entry image entry to check
     * @return true if image is visible
     */
    [[nodiscard]] bool is_visible(const ImageEntryPtr& entry) const;

    /**
     * Get layout scheme: currently visible thumbnails.
     * @return array of visible thumbnails
     */
    [[nodiscard]] const std::vector<Thumbnail>& get_scheme() const;

    /**
     * Enable/disable animated scrolling.
     * @param enable true to animate scrolling
     */
    void set_smooth(const bool enable);

    /**
     * Scroll by specified distance.
     * @param distance distance in pixels (positive to scroll down)
     * @param immediate true to scroll without animation
     */
    void scroll_by(const double distance, const bool immediate);

    /**
     * Start kinetic scrolling.
     * @param velocity initial velocity in pixels per second
     */
    void fling(const double velocity);

    /**
     * Stop any scroll animation at the current position.
     */
    void stop_scroll();

    /**
     * Advance scroll animation.
     * @param dt time since previous frame in seconds
     * @return true if animation is still in progress
     */
    bool animate(const double dt);

    /**
     * Get current kinetic scroll velocity.
     * @return velocity in pixels per second, 0 if not flinging
     */
    [[nodiscard]] double get_velocity() const { return velocity; }

    /**
     * Check if scroll animation is in progress.
     * @return true if scrolling is animated now
     */
    [[nodiscard]] bool is_animating() const;

    /**
     * Move selection to the nearest fully visible thumbnail if the currently
     * selected one is out of view (used after free scrolling).
     * @return true if selection was changed
     */
    bool keep_selection_visible();

private:
    /** Recalculate grid metrics, clamp scroll position and rebuild scheme. */
    void recalc();

    /** Rebuild the scheme of visible thumbnails for current scroll. */
    void rebuild();

    /** Get max scroll offset. */
    [[nodiscard]] double max_scroll() const;

    /** Get top coordinate of the row in grid space. */
    [[nodiscard]] double row_top(const size_t row) const;

    /** Set scroll target (and position for non-smooth mode) with clamping. */
    void scroll_to(const double offset);

    /** Scroll minimally to make the selected thumbnail fully visible. */
    void ensure_visible();

    /** Scroll to place the selected thumbnail in the window center. */
    void center_selection();

    /** Get row of the selected image in the grid. */
    [[nodiscard]] size_t selected_row() const;

private:
    size_t thumb_size;             ///< Size of thumbnail in pixels
    size_t thumb_padding;          ///< Padding between thumbnails in pixels
    std::vector<Thumbnail> scheme; ///< Layout scheme of visible thumbnails

    Size window;          ///< Layout size (output window)
    size_t columns = 1;   ///< Number of columns
    size_t rows = 1;      ///< Number of fully visible rows
    size_t total = 0;     ///< Total number of images
    size_t total_rows = 0; ///< Total number of rows
    ssize_t offset_x = 0; ///< Horizontal offset to center the grid
    ssize_t offset_y = 0; ///< Vertical offset to center short grids

    bool smooth = false;   ///< Animate scrolling
    double scroll = 0;     ///< Current scroll offset in pixels
    double target = 0;     ///< Target scroll offset in pixels
    double velocity = 0;   ///< Kinetic scroll velocity, pixels per second

    ImageEntryPtr sel_entry = nullptr; ///< Currently selected entry
};
