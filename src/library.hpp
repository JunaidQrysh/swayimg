// SPDX-License-Identifier: MIT
// Photo library: saved albums, Photos/Albums views and their overlay UI.

#pragma once

#include "canvas.hpp"
#include "fdevent.hpp"
#include "image.hpp"
#include "imagelist.hpp"
#include "pathhash.hpp"
#include "input.hpp"
#include "sparam.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class FontSet;

/**
 * Photo library.
 * Albums are user selected folders saved in the config directory. The app
 * shows one of the views:
 * - Photos: all images of the albums marked as "shown in Photos";
 * - Albums: grid of saved albums (cover image, name, number of items);
 * - Album: images of a single folder (saved album or temporary one opened
 *   from the command line).
 */
class Library {
public:
    /** Library views. */
    enum class View : uint8_t {
        None,   ///< Plain image list (stdin etc.), no library UI
        Photos, ///< All photos from albums shown in Photos
        Albums, ///< Grid of albums
        Album,  ///< Single album
    };

    /** Media type filter for Photos and Album views. */
    enum class Filter : uint8_t {
        All,    ///< Photos and videos
        Photos, ///< Photos only
        Videos, ///< Videos only
    };

    /** Saved album. */
    struct Album {
        std::filesystem::path path; ///< Album directory
        bool in_photos = true;      ///< Show album images in Photos view
        bool recursive = false;     ///< Include images from nested folders
    };

    /**
     * Get global instance of the library.
     * @return library instance
     */
    static Library& self();

    /**
     * Load saved albums and set up the initial view from startup sources:
     * no sources - Photos view, otherwise the folder of the first source is
     * opened as an album.
     * @param params startup parameters (sources and mode may be changed)
     */
    void startup(StartupParams& params);

    /**
     * Check if images from the startup sources should include all files of
     * their directories (album opened from a file).
     * @return true if adjacent files must be loaded
     */
    [[nodiscard]] bool startup_adjacent() const { return adjacent_start; }

    /**
     * Check if file is a photo or video by its extension.
     * Library views list only such files.
     * @param path file path
     * @return true if file is a media file
     */
    static bool is_media(const std::filesystem::path& path);

    /**
     * Check if file is known to be broken (not decodable media).
     * @param path file path
     * @return true if file must be skipped
     */
    [[nodiscard]] bool is_bad(const std::filesystem::path& path) const;

    /**
     * Remember broken files (persistent, until the file changes).
     * @param paths files to remember
     */
    void mark_bad(const std::vector<std::filesystem::path>& paths);

    /**
     * Check content of all listed files in background and remove the ones
     * that are not media files.
     */
    void start_validation();

    /**
     * Check if file must be listed in library views: media file of the
     * currently selected type, not known to be broken.
     * @param path file path
     * @return true if file is accepted
     */
    [[nodiscard]] bool accepts(const std::filesystem::path& path) const;

    /**
     * Set media type filter and reload current view.
     * @param new_filter filter to set
     */
    void set_filter(const Filter new_filter);

    /** Show Photos view. */
    void show_photos();

    /**
     * Get directories of albums shown in Photos.
     * @return directories to load
     */
    [[nodiscard]] std::vector<ImageList::ScanSource> photo_sources() const;

    /**
     * Get filter for loading files in background: same as accepts(), but
     * works with a copy of the current state.
     * @return file filter
     */
    [[nodiscard]] ImageList::ScanFilter scan_filter() const;

    /**
     * Get id of the current media type filter for the list cache.
     * @return filter id
     */
    [[nodiscard]] std::string cache_tag() const;

    /**
     * Check if album includes nested folders.
     * @param dir album directory
     * @return true if images are loaded recursively
     */
    [[nodiscard]] bool album_recursive(const std::filesystem::path& dir) const;

    /**
     * Include/exclude nested folders of the album.
     * @param dir album directory
     * @param enable flag to set
     */
    void set_recursive(const std::filesystem::path& dir, const bool enable);

    /**
     * Show Albums view.
     * @param select album to select in the grid
     */
    void show_albums(const std::filesystem::path& select = {});

    /**
     * Show single album.
     * @param dir album directory
     */
    void open_album(const std::filesystem::path& dir);

    /**
     * Go back from album to albums grid.
     */
    void back();

    /**
     * Add album to the library.
     * @param dir album directory
     * @return false if album already exists
     */
    bool add_album(const std::filesystem::path& dir);

    /**
     * Remove album from the library (files are not touched).
     * @param dir album directory
     */
    void remove_album(const std::filesystem::path& dir);

    /**
     * Show/hide album images in Photos view.
     * @param dir album directory
     * @param enable flag to set
     */
    void set_in_photos(const std::filesystem::path& dir, const bool enable);

    /**
     * Get saved album.
     * @param dir album directory
     * @return album description or nullptr if album is not saved
     */
    [[nodiscard]] const Album* find(const std::filesystem::path& dir) const;

    /**
     * Get current view.
     * @return current view
     */
    [[nodiscard]] View get_view() const { return view; }

    /**
     * Get directory of the currently opened album.
     * @return album directory (Album view only)
     */
    [[nodiscard]] const std::filesystem::path& get_album() const
    {
        return album;
    }

    /**
     * Activate gallery entry instead of opening viewer (Albums view: open
     * the album represented by the cover entry).
     * @param entry selected gallery entry
     * @return true if handled, false to continue default action
     */
    bool activate_entry(const ImageEntryPtr& entry);

    /**
     * Update gallery insets for the current view and window scale.
     */
    void update_margins();

    /**
     * Draw library overlay (mode switcher, album header) on top of gallery.
     * @param wnd target canvas
     * @param opacity overlay opacity
     */
    void draw(Canvas& wnd, const double opacity);

    /**
     * Show short notification above the bottom controls.
     * @param message text to show
     */
    void toast(const std::string& message);

    /**
     * Draw notification (any mode).
     * @param wnd target canvas
     * @param gallery true if gallery with bottom controls is shown
     */
    void draw_toast(Canvas& wnd, const bool gallery);

    /**
     * Advance animations.
     * @param dt time since previous frame in seconds
     * @return true if animation is in progress
     */
    bool animate(const double dt);

    /**
     * Handle mouse button in gallery mode.
     * @param button changed button
     * @param pressed true if pressed
     * @param pos pointer coordinates
     * @return true if event was consumed by the overlay
     */
    bool handle_mbutton(const InputMouse::mouse_btn_t button,
                        const bool pressed, const Point& pos);

    /**
     * Handle key press in gallery mode.
     * @param key key description
     * @return true if event was consumed
     */
    bool handle_key(const InputKeyboard& key);

private:
    /** Album scan result. */
    struct AlbumInfo {
        std::filesystem::path cover; ///< Newest image in the album
        size_t count = 0;            ///< Number of images
    };

    /** Clickable overlay area. */
    struct HitArea {
        Rectangle rect;               ///< Area on the window
        std::function<void()> action; ///< Click handler
    };

    /** Load saved albums from the config file. */
    void load();

    /** Load list of broken files from cache. */
    void load_bad();

    /** Save list of broken files to cache. */
    void save_bad() const;

    /** Save albums to the config file. */
    void save() const;

    /**
     * Switch view and replace image list.
     * @param new_view view to set
     * @param load function that fills the image list
     * @param select path of the entry to select
     * @param watch use file system monitor
     */
    void set_view(const View new_view,
                  const std::function<void(ImageList&)>& load,
                  const std::filesystem::path& select, const bool watch);

    /**
     * Switch view and load directories into image list in background.
     * @param new_view view to set
     * @param sources directories to load
     * @param watch use file system monitor
     */
    void set_view(const View new_view, std::vector<ImageList::ScanSource> sources,
                  const bool watch);

    /**
     * Switch view: set view specific gallery settings.
     * @param new_view view to set
     */
    void prepare_view(const View new_view);

    /**
     * Check if file of specified type must be listed.
     * @param type media type filter
     * @param path file path
     * @return true if file is accepted
     */
    static bool filter_accepts(const Filter type,
                               const std::filesystem::path& path);

    /** Load fonts and apply window scale. */
    void prepare_fonts();

    /**
     * Draw floating Photos/Albums switcher.
     * @return switcher rectangle
     */
    Rectangle draw_switcher(Canvas& wnd, const double opacity);

    /**
     * Draw filter button next to the switcher.
     * @param switcher switcher rectangle
     * @return button rectangle
     */
    Rectangle draw_filter_button(Canvas& wnd, const Rectangle& switcher,
                                 const double opacity);

    /**
     * Draw filter menu above the button.
     * @param button filter button rectangle
     */
    void draw_filter_menu(Canvas& wnd, const Rectangle& button,
                          const double opacity);

    /** Draw header of the album view. */
    void draw_header(Canvas& wnd, const double opacity);

    /** Draw header of the albums grid view. */
    void draw_albums_header(Canvas& wnd, const double opacity);

    /**
     * Draw header background.
     * @return header height
     */
    size_t draw_header_bg(Canvas& wnd, const double opacity);

    /** Show folder chooser and add selected folders as albums. */
    void add_folders();

    /** Draw date section header in photo views. */
    void draw_section(Canvas& wnd, const ImageEntryPtr& entry,
                      const Rectangle& rect);

    /** Draw caption on album tile in the Albums view. */
    void draw_caption(Canvas& wnd, const ImageEntryPtr& entry,
                      const Rectangle& tile, const double opacity);

    /**
     * Draw text button.
     * @return button rectangle
     */
    Rectangle draw_button(Canvas& wnd, const std::string& text,
                          const Point& right_top, const bool primary,
                          const double opacity);

    /**
     * Get rounded rectangle (pill) mask of specified size.
     * @param size mask size
     * @return grayscale mask
     */
    const Pixmap& pill(const Size& size);

    /**
     * Scale logical pixels to window buffer pixels.
     * @param px logical pixels
     * @return buffer pixels
     */
    [[nodiscard]] ssize_t dp(const double px) const;

private:
    std::vector<Album> albums;            ///< Saved albums
    std::filesystem::path album;          ///< Album shown in Album view
    View view = View::None;               ///< Current view
    bool adjacent_start = false;          ///< Load startup dir from a file
    bool temp_recursive = false; ///< Subfolders of unsaved album included
    PathMap<std::pair<uintmax_t, int64_t>>
        bad_files;                         ///< Broken files: size, mtime
    std::atomic<uint64_t> validate_gen = 0; ///< Background check generation
    PathMap<std::filesystem::path>
        cover_album;                      ///< Cover image to album map
    PathMap<AlbumInfo>
        album_info;                       ///< Albums scan results

    // UI
    FontSet* font_title = nullptr; ///< Font for titles
    FontSet* font_small = nullptr; ///< Font for secondary text
    double font_scale = 0;         ///< Scale applied to fonts
    double scale = 1.0;            ///< Window scale
    Pixmap chevron;                    ///< Back icon mask
    std::vector<HitArea> hits;         ///< Clickable areas from last frame
    bool pressed_hit = false;          ///< Mouse press consumed by overlay
    double hl_pos = 0;                 ///< Switcher highlight position
    Filter filter = Filter::All;       ///< Media type filter
    std::wstring toast_text;           ///< Notification text
    std::chrono::steady_clock::time_point toast_until; ///< Hide time
    double toast_alpha = 0;            ///< Notification visibility
    FdTimer toast_timer;               ///< Timer to start hiding
    bool menu_open = false;            ///< Filter menu is open
    double menu_anim = 0;              ///< Filter menu appearance progress
    size_t margin_top = 0;             ///< Gallery top inset
    size_t margin_bottom = 0;          ///< Gallery bottom inset
    size_t section_height = 0;         ///< Gallery section header height
};
