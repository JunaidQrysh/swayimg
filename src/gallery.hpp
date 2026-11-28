// SPDX-License-Identifier: MIT
// Gallery mode.
// Copyright (C) 2024 Artem Senichev <artemsen@gmail.com>

#pragma once

#include "appmode.hpp"
#include "fdevent.hpp"
#include "fling.hpp"
#include "image.hpp"
#include "layout.hpp"
#include "threadpool.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>

class Gallery : public AppMode {
public:
    /** Thumbnail aspect ratio. */
    enum class Aspect : uint8_t {
        Fit,  ///< Fit image into a square thumbnail
        Fill, ///< Fill square thumbnail with the image
        Keep, ///< Adjust thumbnail size to the aspect ratio of the image
    };

    /**
     * Get global instance of the gallery.
     * @return gallery instance
     */
    static Gallery& self();

    /** Constructor. */
    Gallery();

    /**
     * Select another image file.
     * @param dir next entry direction
     * @return true if new image was selected
     */
    bool select(const Layout::Direction dir);

    /**
     * Select another image at specified position.
     * @param pt thumbnail coordinates within the window
     * @return true if new image was selected
     */
    bool select(const Point& pt);

    /**
     * Reload thumbnails.
     */
    void reload();

    /**
     * Set thumbnail aspect ratio.
     * @param ratio aspect ratio
     */
    void set_thumb_aspect(const Aspect ratio);

    /**
     * Set thumbnail size.
     * @param size new thumbnail size in pixels
     */
    [[nodiscard]] size_t get_thumb_size() const
    {
        return layout.get_thumb_size();
    }

    /**
     * Set thumbnail size.
     * @param size new thumbnail size in pixels
     */
    void set_thumb_size(const size_t size);

    /**
     * Set padding size.
     * @param size new padding size in pixels
     */
    void set_padding_size(const size_t size);

    /**
     * Set border size for selected thumbnail.
     * @param size border size in pixels
     */
    void set_border_size(const size_t size);

    /**
     * Set selected thumbnail border color.
     * @param color selected thumbnail border color
     */
    void set_border_color(const argb_t& color);

    /**
     * Set scale for selected thumbnail.
     * @param scale scale factor for selected thumbnail
     */
    void set_selected_scale(const double scale);

    /**
     * Set background color for selected thumbnail.
     * @param color selected thumbnail background color
     */
    void set_selected_color(const argb_t& color);

    /**
     * Set background color for unselected thumbnails.
     * @param color thumbnail background color
     */
    void set_background_color(const argb_t& color);

    /**
     * Set window color.
     * @param color window color
     */
    void set_window_color(const argb_t& color);

    /**
     * Enable/disable mouse hover selection.
     * @param enable flag to set
     */
    void enable_hover(const bool enable);

    /**
     * Set touchpad scroll speed.
     * @param speed multiplier of finger movement
     */
    void set_scroll_speed(const double speed);

    /**
     * Set number of thumbnails stored in memory cache.
     * @param size max number of thumbnails stored in memory cache
     */
    void set_cache_size(const size_t size);

    /**
     * Enable preloading invisible thumbnails.
     * @param enable flag to set
     */
    void enable_preload(const bool enable);

    /**
     * Enable/disable persistent storage.
     * @param enable flag to set
     */
    void enable_pstore(const bool enable);

    /**
     * Set path for persistent storage.
     * @param path directory for persistent storage
     */
    void set_pstore_path(const std::filesystem::path& path);

    // app mode interface implementation
    void initialize() override;
    void activate(const ImageEntryPtr& entry, const Size& wnd) override;
    void deactivate() override;
    ImageEntryPtr get_current() override;
    bool set_current(const ImageEntryPtr& entry) override;
    void window_resize(const Size& wnd) override;
    void window_redraw(Canvas& wnd) override;
    void handle_mmove(const InputMouse& input, const Point& pos,
                      const Point& delta) override;
    void handle_pinch(const double scale_delta, const double dx,
                      const double dy) override;
    bool ready_to_draw() override;
    bool animate(const double dt) override;
    bool handle_mbutton(const InputMouse::mouse_btn_t button,
                        const bool pressed, const Point& pos) override;

    /**
     * Check if there is a thumbnail at specified position.
     * @param pos window coordinates
     * @return true if a thumbnail is under the position
     */
    [[nodiscard]] bool thumb_at(const Point& pos) const
    {
        return layout.at(pos) != nullptr;
    }

    /**
     * Scroll the gallery (mouse wheel or touchpad).
     * @param delta scroll delta (wheel steps, fractional for touchpad)
     */
    void scroll(const double delta);

    /**
     * Check if the current scroll comes from a touchpad.
     * @return true for finger scrolling
     */
    [[nodiscard]] bool finger_scroll() const { return scroll_finger; }

    /**
     * Get tile of the image thumbnail on the window.
     * @param entry image entry
     * @param wnd_size window size
     * @return tile rectangle, empty if image is not visible
     */
    [[nodiscard]] Rectangle get_tile_rect(const ImageEntryPtr& entry,
                                          const Size& wnd_size) const;

    /**
     * Check if thumbnails cover the tile (cropped to square).
     * @return true if thumbnail covers the tile, false if fit
     */
    [[nodiscard]] bool thumb_cover() const { return aspect == Aspect::Fill; }

    /**
     * Set content insets covered by overlays (header, floating controls).
     * @param top,bottom inset sizes in pixels
     */
    void set_margins(const size_t top, const size_t bottom);

    /**
     * Drop all cached thumbnails (the image list was replaced).
     * Must be called while the gallery is not active.
     */
    void drop_cache();

    /**
     * Get window background color.
     * @return window color
     */
    [[nodiscard]] argb_t get_window_color() const { return clr_window; }

    /**
     * Group thumbnails into sections with headers.
     * @param fn section splitter, nullptr to disable sections
     * @param height header height in pixels
     */
    void set_sections(Layout::Splitter fn, const size_t height);

    /** Section header drawing callback: first entry, header area. */
    using SectionOverlay =
        std::function<void(Canvas&, const ImageEntryPtr&, const Rectangle&)>;
    SectionOverlay section_overlay; ///< Drawn for each visible header

    /** Tile overlay drawing callback (e.g. album captions). */
    using TileOverlay = std::function<void(Canvas&, const ImageEntryPtr&,
                                           const Rectangle&, double)>;
    TileOverlay tile_overlay; ///< Drawn on top of each thumbnail

    /** Called from a loader thread when an image can't be decoded. */
    std::function<void(const std::filesystem::path&)> on_load_failed;

protected:
    void switch_current() override;
    void handle_scroll_stop() override;
    void handle_imagelist(const ImageListEvent event,
                          const std::vector<ImageEntryPtr>& entries) override;
    void invalidate(const std::vector<ImageEntryPtr>& entries) override;

private:
    /**
     * Draw thumbnail.
     * @param tlay thumbnail layout description
     * @param wnd target window
     */
    void draw(const Layout::Thumbnail& tlay, Canvas& wnd);

    /**
     * Get tile rectangle of the thumbnail (including selection scale).
     * @param tlay thumbnail layout description
     * @param wnd_size window size
     * @return tile rectangle
     */
    [[nodiscard]] Rectangle tile_rect(const Layout::Thumbnail& tlay,
                                      const Size& wnd_size) const;

    /**
     * Draw scroll bar.
     * @param wnd target window
     */
    void draw_scrollbar(Canvas& wnd);

    /**
     * Check if point is inside scroll bar hit area.
     * @param pos window coordinates
     */
    [[nodiscard]] bool on_scrollbar(const Point& pos) const;

    /**
     * Scroll to follow the dragged scroll bar thumb.
     * @param y pointer vertical position
     */
    void drag_scrollbar(const ssize_t y);

    /**
     * Refresh view: clear and load thumbnails.
     */
    void refresh();

    /**
     * Update visible range and reload thumbnails if visible area changed.
     */
    void on_scrolled();

    /**
     * Update range of visible images (used by loader threads).
     */
    void update_visible();

    /**
     * Update selection after free scrolling: select thumbnail under the
     * pointer (hover selection) or keep the selection visible.
     * @param use_pointer true to take the pointer position into account
     */
    void follow_scroll(const bool use_pointer);

    /**
     * Switch current image after selection followed scrolling (no animation).
     */
    void follow_selection();

    /**
     * Rebuild loading queue.
     */
    void requeue_loading();

    /**
     * Remove invisible thumbnails from the cache.
     */
    void clear_invisible();

    /**
     * Get thumbnail pixmap for specified image.
     * @param entry image entry
     * @return thumbnail pixmap or nullptr if thumbnail not yet loaded
     */
    const Pixmap* get_thumbnail(const ImageEntryPtr& entry);

    /**
     * Load image thumbnail.
     * @param entry image entry to load
     */
    void load_thumbnail(const ImageEntryPtr& entry);

    /**
     * Load thumbnail from persistent storage.
     * @param entry image entry for thumbnail
     * @return pixmap with thumbnail
     */
    [[nodiscard]] Pixmap pstore_load(const ImageEntryPtr& entry) const;

    /**
     * Save thumbnail on persistent storage.
     * @param entry image entry for thumbnail
     * @param thumb pixmap with thumbnail
     */
    void pstore_save(const ImageEntryPtr& entry, const Pixmap& thumb) const;

private:
    Layout layout;         ///< Thumbnail layout
    Aspect aspect;         ///< Thumbnail aspect ratio
    size_t border_size;    ///< Selected tile border size
    double selected_scale; ///< Selected tile scale

    argb_t clr_window;     ///< Window background
    argb_t clr_background; ///< Tile background
    argb_t clr_select;     ///< Selected tile background for transparent images
    argb_t clr_border;     ///< Selected tile border

    bool hover_select; ///< Mouse hover selection
    Point pointer;              ///< Last pointer position
    bool pointer_valid = false; ///< Pointer position is known
    double scroll_speed = 1.5; ///< Touchpad scroll speed multiplier

    ThreadPool tpool; ///< Loading threads

    bool pstore_enable; ///< Use persistent storage for thumbnails
    std::filesystem::path pstore_path; ///< Persistent storage path

    std::unordered_map<ImageEntryPtr, Pixmap> thumbs; ///< Loaded thumbnails
    std::set<ImageEntryPtr> crld_thumbs; ///< Currently loading thumbnails

    std::atomic<size_t> visible_first = 1; ///< First visible image index
    std::atomic<size_t> visible_last = 0;  ///< Last visible image index

    // hold the first frame until visible thumbnails are loaded
    std::atomic<bool> warmup = false; ///< Waiting for visible thumbnails
    std::chrono::steady_clock::time_point warmup_end; ///< Warmup deadline
    FdTimer warmup_timer; ///< Timer to finish warmup

    // animations
    std::unordered_map<ImageEntryPtr, double> fade; ///< Fade-in progress
    ImageEntryPtr sel_last;  ///< Currently selected (animated) entry
    ImageEntryPtr sel_prev;  ///< Previously selected entry
    double sel_anim = 1.0;   ///< Selection animation progress
    bool free_scroll = false; ///< Scrolling by wheel/touchpad (not keyboard)
    FlingTracker fling_tracker; ///< Touchpad fling velocity estimation
    double fling_carry = 0; ///< Fling velocity interrupted by a new gesture

    // scroll bar
    struct ScrollBar {
        double alpha = 0;      ///< Current visibility
        double last_pos = 0;   ///< Scroll offset at last frame
        std::chrono::steady_clock::time_point show_until; ///< Hide time
        FdTimer timer;         ///< Timer to start hiding
        bool hover = false;    ///< Pointer is over the scroll bar
        bool drag = false;     ///< Thumb is dragged
        ssize_t drag_offset = 0; ///< Pointer offset inside the thumb
        Rectangle track;       ///< Track area from the last frame
        Rectangle thumb;       ///< Thumb area from the last frame
    } sbar;

    bool preload;      ///< Enable/disable preloading of invisible thumbnails
    size_t cache_size; ///< Max number of thumbnails in cache
    std::mutex mutex;  ///< Sync mutex for thumbnails cache access
};
