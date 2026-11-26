// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#pragma once

#include "image.hpp"
#include "pathhash.hpp"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

/** Thread-safe list of images. */
class ImageList {
public:
    /** Order of the image list. */
    enum class Order : uint8_t {
        None,    ///< Unsorted (system depended)
        Alpha,   ///< Lexicographic sort
        Numeric, ///< Numeric sort
        Mtime,   ///< Modification time sort
        Size,    ///< Size sort
        Random,  ///< Random order
    };

    /** Direction of the next entry. */
    enum class Dir : uint8_t {
        First,      ///< First entry in the list
        Last,       ///< Last entry in the list
        Next,       ///< Next entry
        Prev,       ///< Previous entry
        NextParent, ///< Next entry with different parent
        PrevParent, ///< Previous entry with different parent
        Random,     ///< Random entry
    };

    using EntriesArray = std::vector<ImageEntryPtr>;
    /** Map of path to entry. Key is a view of the path string of the
     * entry (`entry->path.native()`): copying std::filesystem::path is
     * expensive (each component is a separate allocated object). */
    using EntriesMap = std::unordered_map<std::string_view, ImageEntryPtr>;

    /** Directory to load in background. */
    struct ScanSource {
        std::filesystem::path path; ///< Directory path
        bool recursive;             ///< Load nested directories
    };

    /** Part of the image list loaded in background. */
    struct ScanBatch {
        uint64_t id = 0;     ///< Scan id
        EntriesArray all;    ///< All entries found so far (sorted)
        EntriesArray added;  ///< Entries found since the previous batch
        EntriesArray removed; ///< Entries of vanished or changed files
        /** Map of `added` entries (first batch only): built in background
         * to save the main thread from inserting them one by one. */
        std::shared_ptr<EntriesMap> index;
        std::vector<std::filesystem::path> dirs; ///< New directories to watch
        Order order = Order::None; ///< Order used for sorting
        bool reverse = false;      ///< Reverse order used for sorting
        bool done = false;         ///< Last batch of the scan
    };
    using ScanBatchPtr = std::shared_ptr<const ScanBatch>;
    /** Filter for files found while scanning (called in background). */
    using ScanFilter = std::function<bool(const std::filesystem::path&)>;
    /** Batch handler (called in background), must pass batch to
     * scan_apply() in the main thread. */
    using ScanPost = std::function<void(const ScanBatchPtr&)>;

    /**
     * Get global instance of image list.
     * @return image list instance
     */
    static ImageList& self();

    /** Constructor. */
    ImageList();

    /** Destructor. */
    ~ImageList();

    ImageList(const ImageList&) = delete;
    ImageList& operator=(const ImageList&) = delete;

    /**
     * Start loading directories in a background thread, the previous scan
     * is stopped. Found entries are passed in batches to `post`.
     * The result is cached: the next scan of the same sources posts the
     * cached list first, then updates it with changes found on disk.
     * Main thread only.
     * @param sources directories to load
     * @param filter filter for found files (nullptr: accept all)
     * @param post batch handler
     * @param cache_tag id of the filter used in the cache key, empty to
     *        disable cache
     */
    void scan_start(std::vector<ScanSource> sources, ScanFilter filter,
                    ScanPost post, std::string cache_tag);

    /**
     * Stop background loading, batches already posted are ignored.
     * Main thread only.
     */
    void scan_stop();

    /**
     * Add entries of the background scan batch to the list.
     * Main thread only.
     * @param batch batch to apply
     * @param added entries added to the list
     * @param removed entries removed from the list
     * @return false if batch belongs to stopped scan
     */
    bool scan_apply(const ScanBatch& batch, EntriesArray& added,
                    EntriesArray& removed);

    /**
     * Check if background loading is in progress.
     * @return true if not all entries are loaded yet
     */
    [[nodiscard]] bool scanning() const { return scan_active; }

    /**
     * Add all images in specified sources.
     * @param sources list of sources to load
     * @return list of added entries in source order
     */
    EntriesArray add(const std::vector<std::filesystem::path>& sources);

    /**
     * Add entry without checking or expanding the path (e.g. a directory
     * shown as an empty album).
     * @param path entry path
     * @param mtime modification time used for sorting
     * @return added entry, nullptr if already exists
     */
    ImageEntryPtr add_placeholder(const std::filesystem::path& path,
                                  const std::time_t mtime);

    /**
     * Remove all given paths from the list.
     * @param sources entries paths to remove
     * @return list of removed entries
     */
    EntriesArray remove(const std::vector<std::filesystem::path>& sources);

    /**
     * Remove image entry from the list.
     * @param entry entry to remove
     */
    void remove(const ImageEntryPtr& entry);

    /**
     * Clear image list.
     * @return list of removed entries
     */
    EntriesArray clear();

    /**
     * Get number of entries in the image list.
     * @return image list size
     */
    size_t size();

    /**
     * Get image list order.
     * @return order of the image list
     */
    Order get_order() const { return order; }

    /**
     * Set order and sort the image list.
     * @param new_order new order of the image list
     */
    void set_order(const Order new_order);

    /**
     * Enable/disable reverse order.
     * @param enable state to set
     */
    void set_reverse(const bool enable);

    /**
     * Get image list reverse mode.
     * @return reverse mode of the image list
     */
    bool get_reverse() const { return reverse; }

    /**
     * Check if directory is hidden (name starts with a dot): such
     * directories are skipped when loading nested folders.
     * @param path directory path
     * @return true if directory is hidden
     */
    static bool is_hidden(const std::filesystem::path& path)
    {
        const std::string name = path.filename().string();
        return name.size() > 1 && name[0] == '.' && name != "..";
    }

    /**
     * Get list generation: changes whenever entries are added, removed or
     * reordered.
     * @return generation counter
     */
    [[nodiscard]] uint64_t generation() const { return gen; }

    /**
     * Find image entry by source path.
     * @param path path to the file or special source
     * @return image entry or nullptr if entry not found
     */
    ImageEntryPtr find(const std::filesystem::path& path);

    /**
     * Get array with copy of all entries in the list.
     * @return array with all entries
     */
    EntriesArray get_all();

    /**
     * Get next entry from the list.
     * @param from starting image entry
     * @param dir next entry direction
     * @return image entry or nullptr if entry not found
     */
    ImageEntryPtr get(const ImageEntryPtr& from, const Dir dir);

    /**
     * Get image entry in specified distance from the start.
     * @param from starting image entry
     * @param distance number entries to skip
     * @return image entry or nullptr if distance if out of bounds
     */
    ImageEntryPtr get(const ImageEntryPtr& from, const ssize_t distance);

    /**
     * Get distance between two image entries.
     * @param from,to image entries
     * @return number of entries in range [start,end]
     */
    ssize_t distance(const ImageEntryPtr& from, const ImageEntryPtr& to);

private:
    /**
     * Get child entries by directory path.
     * @param path parent directory
     * @return list of entries
     */
    EntriesArray get_child(const std::filesystem::path& path) const;

    /**
     * Get the nearest entry for specified removed one.
     * @param entry removed image entry
     * @param forward direction of nearest returned entry
     * @return image entry or nullptr if entry not found
     */
    ImageEntryPtr get_removed(const ImageEntryPtr& entry,
                              const bool forward) const;

    /**
     * Get the nearest entry with different parent.
     * @param from starting image entry
     * @param forward direction of nearest returned entry
     * @return image entry or nullptr if entry not found
     */
    ImageEntryPtr get_diffparent(const ImageEntryPtr& from,
                                 const bool forward) const;

    /**
     * Get random entry.
     * @param excl excluded image entry
     * @return image entry or nullptr if entry not found
     */
    ImageEntryPtr get_random(const ImageEntryPtr& excl) const;

    /**
     * Add file, directory or special source to the list.
     * @param path path to the file or special source
     * @return list of added entries
     */
    EntriesArray add_any(const std::filesystem::path& path);

    /**
     * Add files from the directory to the list.
     * @param path path to the directory
     * @return list with added entries
     */
    EntriesArray add_dir(const std::filesystem::path& path);

    /**
     * Add file to the list.
     * @param path path to the file
     * @return added entry
     */
    ImageEntryPtr add_file(const std::filesystem::path& path);

    /**
     * Add new entry to the list.
     * @param path path to the image file
     * @param mtime file modification time
     * @param size size of the file
     * @return added entry (nullptr if already exists)
     */
    ImageEntryPtr add_entry(const std::filesystem::path& path,
                            const std::time_t mtime = 0, const size_t size = 0);

    /**
     * Sort and reindex image list.
     */
    void sort();

    /**
     * Sort specified image list.
     * @param entries image list to sort
     */
    void sort(EntriesArray& entries) const;

    /**
     * Reindex the image list.
     * @param index (0-based) to start reindexing from
     */
    void reindex(const size_t index = 0);

private:
    EntriesArray entries_arr; ///< Array of image entries
    EntriesMap entries_map;   ///< Map of path to image entries

    std::shared_mutex mutex; ///< Image list mutex

    Order order;  ///< Image list order
    bool reverse; ///< Reverse order flag
    std::atomic<uint64_t> gen = 0; ///< List generation

    // Background loading (main thread only)
    std::thread scan_thread;                         ///< Scanner thread
    std::shared_ptr<std::atomic<bool>> scan_cancel;  ///< Scan stop flag
    uint64_t scan_id = 0;                            ///< Current scan id
    bool scan_active = false;                        ///< Scan in progress
    bool scan_watch = false;                         ///< Watch scanned dirs
    /** Scanned entries replaced with existing ones (same path). */
    std::unordered_map<const ImageEntry*, ImageEntryPtr> scan_alias;

public:
    bool recursive; ///< Read directories recursively
    /** Filter for files found in directories (nullptr: accept all). */
    std::function<bool(const std::filesystem::path&)> file_filter;
    bool adjacent;  ///< Open adjacent files from the same directory
    bool fsmon;     ///< FS monitor usage
};
