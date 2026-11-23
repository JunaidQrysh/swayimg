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

    /**
     * Scroll the gallery (mouse wheel or touchpad).
     * @param delta scroll delta (wheel steps, fractional for touchpad)
     */
    void scroll(const double delta);

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

protected:
    void switch_current() override;
    void handle_scroll_stop() override;
    void handle_imagelist(const ImageListEvent event,
                          const std::vector<ImageEntryPtr>& entries) override;

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

    bool preload;      ///< Enable/disable preloading of invisible thumbnails
    size_t cache_size; ///< Max number of thumbnails in cache
    std::mutex mutex;  ///< Sync mutex for thumbnails cache access
};
