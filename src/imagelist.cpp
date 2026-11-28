// SPDX-License-Identifier: MIT
// List of images.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "imagelist.hpp"

#include "defaults.hpp"
#include "fsmonitor.hpp"
#include "log.hpp"

#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <unordered_set>

namespace {

/**
 * Comparison of two localized strings.
 * @param l,r strings to compare
 * @param numeric flag to use numeric compare
 * @return compare result
 */
int compare_strings(const std::string& l, const std::string& r,
                    const bool numeric)
{
    static const std::locale loc("");
    static const std::collate<char>& coll =
        std::use_facet<std::collate<char>>(loc);

    int cmp = 0;

    if (!numeric) {
        cmp = coll.compare(l.data(), l.data() + l.length(), r.data(),
                           r.data() + r.length());
    } else {
        const std::string digits = "0123456789";
        std::string ls(l);
        std::string rs(r);
        while (true) {
            const size_t ldstart = ls.find_first_of(digits);
            const size_t rdstart = rs.find_first_of(digits);
            if (ldstart == std::string::npos || rdstart != ldstart) {
                cmp = coll.compare(ls.data(), ls.data() + ls.length(),
                                   rs.data(), rs.data() + rs.length());
                break;
            }
            cmp = coll.compare(ls.data(), ls.data() + ldstart, rs.data(),
                               rs.data() + rdstart);
            if (cmp) {
                break;
            }

            ls.erase(0, ldstart);
            rs.erase(0, rdstart);

            try {
                const size_t lnum = std::stoull(ls);
                const size_t rnum = std::stoull(rs);
                if (lnum != rnum) {
                    cmp = lnum < rnum ? -1 : 1;
                    break;
                }
            } catch (const std::out_of_range&) {
                cmp = coll.compare(l.data(), l.data() + l.length(), r.data(),
                                   r.data() + r.length());
                break;
            }

            const size_t ldend = ls.find_first_not_of(digits);
            const size_t rdend = rs.find_first_not_of(digits);
            ls.erase(0, ldend);
            rs.erase(0, rdend);
        }
    }

    return cmp;
}

/**
 * Comparison of two paths.
 * @param l,r paths to compare
 * @param numeric flag to use numeric compare
 * @return compare result
 */
int compare_paths(const std::filesystem::path& l,
                  const std::filesystem::path& r, const bool numeric)
{
    // compare parents
    const std::filesystem::path l_parent = l.parent_path();
    const std::filesystem::path r_parent = r.parent_path();
    auto it_l = l_parent.begin();
    auto it_r = r_parent.begin();
    while (it_l != l_parent.end() && it_r != r_parent.end()) {
        const int cmp = compare_strings(*it_l, *it_r, numeric);
        if (cmp) {
            return cmp;
        }
        ++it_l;
        ++it_r;
    }
    if (it_l == l_parent.end() && it_r != r_parent.end()) {
        return -1;
    }
    if (it_l != l_parent.end() && it_r == r_parent.end()) {
        return 1;
    }

    // compare names
    return compare_strings(l.filename(), r.filename(), numeric);
}

/**
 * Comparison of two image entries.
 * @param l,r image entries to compare
 * @param order criterion for comparison
 * @return true if l < r
 */
bool compare_entries(const ImageEntry& l, const ImageEntry& r,
                     const ImageList::Order order)
{
    switch (order) {
        case ImageList::Order::Alpha:
            return compare_paths(l.path, r.path, false) < 0;
        case ImageList::Order::Numeric:
            return compare_paths(l.path, r.path, true) < 0;
        case ImageList::Order::Mtime:
            return l.mtime == r.mtime ? compare_paths(l.path, r.path, false) < 0
                                      : l.mtime < r.mtime;
        case ImageList::Order::Size:
            return l.size == r.size ? compare_paths(l.path, r.path, false) < 0
                                    : l.size < r.size;
        case ImageList::Order::None:
        case ImageList::Order::Random:
            break;
    }
    assert(false); // unreachable
    return false;
}

/**
 * Create image entry for the file.
 * @param path absolute path to the file
 * @return image entry or nullptr if file can't be used
 */
ImageEntryPtr create_entry(const std::filesystem::path& path)
{
    // Use non-throwing functions: the file can be removed or renamed at any
    // moment (temporary files of a browser or an editor), and callers (file
    // system event handlers) don't catch exceptions.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        Log::warning("File {} is not a regular, skipped", path.string());
        return nullptr;
    }

    const auto fs_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        Log::warning("File {} skipped: {}", path.string(), ec.message());
        return nullptr;
    }
    const auto sys_time =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            fs_time - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());

    const size_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        Log::warning("File {} skipped: {}", path.string(), ec.message());
        return nullptr;
    }

    ImageEntryPtr entry = std::make_shared<ImageEntry>();
    entry->path = path;
    entry->mtime = std::chrono::system_clock::to_time_t(sys_time);
    entry->size = size;
    return entry;
}

/**
 * Get path of the scan cache file.
 * @param key cache key (sources and filter)
 * @return path to the cache file, empty if no cache dir
 */
std::filesystem::path scan_cache_path(const std::string& key)
{
    std::filesystem::path dir;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        dir = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            return {};
        }
        dir = std::filesystem::path(home) / ".cache";
    }

    // FNV-1a: stable between builds, unlike std::hash
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char ch : key) {
        hash ^= static_cast<uint8_t>(ch);
        hash *= 0x100000001b3ULL;
    }

    return dir / "swayimg" / "scan" / std::format("{:016x}", hash);
}

/**
 * Background directory loader: walks directories and periodically posts
 * batches with all entries found so far, sorted in the list order.
 * The result is saved to a cache file: on the next scan of the same sources
 * the cached list is posted first, then the scan only adds new files and
 * removes the ones that vanished or changed.
 * Entries are not changed after posting, the main thread owns them.
 */
class Scanner {
public:
    Scanner(const uint64_t id, std::vector<ImageList::ScanSource> sources,
            ImageList::ScanFilter filter, ImageList::ScanPost post,
            std::string cache_tag, const ImageList::Order order,
            const bool reverse, std::shared_ptr<std::atomic<bool>> cancel)
        : id(id)
        , sources(std::move(sources))
        , filter(std::move(filter))
        , post(std::move(post))
        , cache_tag(std::move(cache_tag))
        , order(order)
        , reverse(reverse)
        , cancel(std::move(cancel))
        , by_name(order == ImageList::Order::Alpha ||
                  order == ImageList::Order::Numeric)
        , sorted(order != ImageList::Order::None &&
                 order != ImageList::Order::Random)
    {
    }

    /** Load all sources. */
    void run()
    {
        last_flush = std::chrono::steady_clock::now();

        // absolute paths of sources, they also form the cache key
        std::vector<ImageList::ScanSource> roots;
        std::string key = cache_tag;
        for (const auto& src : sources) {
            std::error_code ec;
            std::filesystem::path root =
                std::filesystem::absolute(src.path, ec).lexically_normal();
            if (ec) {
                Log::warning("Invalid path {}, skipped", src.path.string());
                continue;
            }
            key += '\n';
            key += src.recursive ? 'r' : '-';
            key += root.string();
            roots.push_back({ .path = std::move(root),
                              .recursive = src.recursive });
        }

        if (!cache_tag.empty()) {
            cache_path = scan_cache_path(key);
            if (!cache_path.empty() && load_cache()) {
                flush(false, true); // show cached list right away
            }
        }

        for (const auto& root : roots) {
            scan_dir(root.path, root.recursive);
            if (*cancel) {
                return;
            }
        }

        // cached files not found by the scan
        for (auto& it : cached) {
            removed.push_back(std::move(it.second));
        }
        if (!cached.empty()) {
            dirty = true;
            cached.clear();
        }

        if (order == ImageList::Order::Random && !cache_loaded) {
            static thread_local std::mt19937 engine(std::random_device {}());
            std::shuffle(all.begin(), all.end(), engine);
        }
        flush(true);

        if (dirty && !cache_path.empty() && !*cancel) {
            save_cache();
        }
    }

private:
    // first batch is sent early to show something as soon as possible
    static constexpr std::chrono::milliseconds FIRST_BATCH { 30 };
    static constexpr std::chrono::milliseconds NEXT_BATCH { 150 };

    // cache file header
    static constexpr const char* CACHE_MAGIC = "swayimg-scan-v1";

    /** Entry comparator in the list order. */
    bool less(const ImageEntryPtr& l, const ImageEntryPtr& r) const
    {
        return reverse ? compare_entries(*r, *l, order)
                       : compare_entries(*l, *r, order);
    }

    /**
     * Load list from the cache file.
     * @return true if cache was loaded
     */
    bool load_cache()
    {
        std::ifstream file(cache_path, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        // header: magic, order and reverse flag used for sorting
        std::string line;
        if (!std::getline(file, line)) {
            return false;
        }
        int cache_order = -1;
        int cache_reverse = -1;
        {
            const std::string magic = std::string(CACHE_MAGIC) + ' ';
            if (!line.starts_with(magic) ||
                std::sscanf(line.c_str() + magic.size(), "%d %d",
                            &cache_order, &cache_reverse) != 2) {
                return false;
            }
        }

        // entries: "<mtime> <size> <path>"
        while (std::getline(file, line)) {
            if (*cancel) {
                return false;
            }
            const char* begin = line.data();
            const char* end = begin + line.size();
            std::time_t mtime = 0;
            size_t size = 0;
            auto res = std::from_chars(begin, end, mtime);
            if (res.ec != std::errc() || res.ptr == end || *res.ptr != ' ') {
                continue;
            }
            res = std::from_chars(res.ptr + 1, end, size);
            if (res.ec != std::errc() || res.ptr == end || *res.ptr != ' ') {
                continue;
            }
            std::filesystem::path path(std::string(res.ptr + 1, end));
            if (filter && !filter(path)) {
                dirty = true; // e.g. file is known as broken now
                continue;
            }
            ImageEntryPtr entry = std::make_shared<ImageEntry>();
            entry->path = std::move(path);
            entry->mtime = mtime;
            entry->size = size;
            if (cached.try_emplace(entry->path.native(), entry).second) {
                all.push_back(std::move(entry));
            }
        }

        // sort only if the list order was changed since the cache was saved
        if (cache_order != static_cast<int>(order) ||
            cache_reverse != static_cast<int>(reverse)) {
            dirty = true;
            if (sorted) {
                std::ranges::sort(all, [this](const auto& l, const auto& r) {
                    return less(l, r);
                });
            }
        }
        if (order == ImageList::Order::Random) {
            static thread_local std::mt19937 engine(std::random_device {}());
            std::shuffle(all.begin(), all.end(), engine);
        }

        cache_loaded = true;
        unposted = all; // must be registered by the main thread
        return !all.empty();
    }

    /** Save list to the cache file. */
    void save_cache() const
    {
        std::error_code ec;
        std::filesystem::create_directories(cache_path.parent_path(), ec);
        if (ec) {
            return;
        }

        // unique temporary file: other instances may save the same cache
        std::filesystem::path tmp = cache_path;
        tmp += std::format(".{}.tmp", getpid());
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                return;
            }
            file << CACHE_MAGIC << ' ' << static_cast<int>(order) << ' '
                 << static_cast<int>(reverse) << '\n';
            for (const ImageEntryPtr& entry : all) {
                const std::string& path = entry->path.native();
                if (path.find('\n') == std::string::npos) {
                    file << entry->mtime << ' ' << entry->size << ' ' << path
                         << '\n';
                }
            }
            if (!file) {
                file.close();
                std::filesystem::remove(tmp, ec);
                return;
            }
        }
        std::filesystem::rename(tmp, cache_path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
        }
    }

    /**
     * Load directory.
     * @param dir absolute path to the directory
     * @param recursive load nested directories
     */
    void scan_dir(const std::filesystem::path& dir, const bool recursive)
    {
        if (watch_dirs.insert(dir).second) {
            dirs.push_back(dir);
        }

        // Handle errors per entry: a single entry that can't be checked (a
        // symlink loop, a link to an unreadable directory, an I/O error) must
        // not hide all the other files of the directory.
        std::vector<std::filesystem::path> files;
        std::vector<std::filesystem::path> subdirs;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        const std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            if (*cancel) {
                return;
            }
            std::filesystem::path child = it->path();
            std::error_code child_ec;
            if (std::filesystem::is_directory(child, child_ec)) {
                if (recursive && !ImageList::is_hidden(child)) {
                    subdirs.push_back(std::move(child));
                }
            } else if (child_ec) {
                Log::warning("File {} skipped: {}", child.string(),
                             child_ec.message());
            } else if (!filter || filter(child)) {
                files.push_back(std::move(child));
            }
            it.increment(ec);
        }

        // For name orders walk the tree in the list order: new entries are
        // appended to the end, already shown part of the list stays in place.
        // Files of a directory precede files of its subdirectories.
        if (by_name) {
            const bool numeric = order == ImageList::Order::Numeric;
            std::ranges::sort(subdirs, [this, numeric](const auto& l,
                                                       const auto& r) {
                const int cmp =
                    compare_strings(l.filename(), r.filename(), numeric);
                return reverse ? cmp > 0 : cmp < 0;
            });
        }
        if (!(by_name && reverse)) {
            add_files(files);
        }
        for (const auto& sub : subdirs) {
            scan_dir(sub, recursive);
            if (*cancel) {
                return;
            }
        }
        if (by_name && reverse) {
            add_files(files);
        }
    }

    /**
     * Create entries for files.
     * @param files absolute paths to the files
     */
    void add_files(const std::vector<std::filesystem::path>& files)
    {
        for (const auto& path : files) {
            if (*cancel) {
                return;
            }
            if (seen.contains(path.native())) {
                continue; // same file from another source
            }
            ImageEntryPtr entry = create_entry(path);

            // file from the cache: keep its entry if file wasn't changed
            const auto it = cached.find(path.native());
            if (it != cached.end()) {
                ImageEntryPtr prev = std::move(it->second);
                cached.erase(it);
                if (entry && entry->mtime == prev->mtime &&
                    entry->size == prev->size) {
                    seen.insert(path.native());
                    continue;
                }
                removed.push_back(std::move(prev));
            }

            if (entry) {
                seen.insert(path.native());
                pending.push_back(std::move(entry));
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= (posted ? NEXT_BATCH : FIRST_BATCH)) {
                flush(false);
            }
        }
    }

    /**
     * Apply pending changes and post batch to the main thread.
     * @param done last batch flag
     * @param force post batch even if nothing changed
     */
    void flush(const bool done, const bool force = false)
    {
        last_flush = std::chrono::steady_clock::now();
        if (pending.empty() && removed.empty() && !done && !force) {
            return;
        }

        if (!removed.empty()) {
            std::unordered_set<const ImageEntry*> gone;
            gone.reserve(removed.size());
            for (const ImageEntryPtr& entry : removed) {
                gone.insert(entry.get());
            }
            std::erase_if(all, [&gone](const ImageEntryPtr& entry) {
                return gone.contains(entry.get());
            });
            dirty = true;
        }

        if (!pending.empty()) {
            dirty = true;
            const size_t mid = all.size();
            if (!sorted) {
                all.insert(all.end(), pending.begin(), pending.end());
            } else {
                std::ranges::sort(pending, [this](const auto& l,
                                                  const auto& r) {
                    return less(l, r);
                });
                all.insert(all.end(), pending.begin(), pending.end());
                if (mid && less(all[mid], all[mid - 1])) {
                    std::inplace_merge(all.begin(), all.begin() + mid,
                                       all.end(),
                                       [this](const auto& l, const auto& r) {
                                           return less(l, r);
                                       });
                }
            }
        }

        auto batch = std::make_shared<ImageList::ScanBatch>();
        batch->id = id;
        batch->all = all;
        batch->added = std::move(pending);
        if (!unposted.empty()) {
            // cached entries are new to the main thread too
            batch->added.insert(batch->added.end(), unposted.begin(),
                                unposted.end());
            unposted.clear();
        }
        batch->removed = std::move(removed);
        if (!posted && !batch->added.empty()) {
            // the first batch is usually the whole cached list
            batch->index = std::make_shared<ImageList::EntriesMap>();
            batch->index->reserve(batch->added.size());
            for (const ImageEntryPtr& entry : batch->added) {
                batch->index->emplace(entry->path.native(), entry);
            }
        }
        batch->dirs = std::move(dirs);
        batch->order = order;
        batch->reverse = reverse;
        batch->done = done;
        pending.clear();
        removed.clear();
        dirs.clear();

        if (!*cancel) {
            post(batch);
            posted = true;
        }
    }

    const uint64_t id;
    const std::vector<ImageList::ScanSource> sources;
    const ImageList::ScanFilter filter;
    const ImageList::ScanPost post;
    const std::string cache_tag;
    const ImageList::Order order;
    const bool reverse;
    const std::shared_ptr<std::atomic<bool>> cancel;
    const bool by_name; ///< List is sorted by path
    const bool sorted;  ///< List is sorted (not random or unordered)

    ImageList::EntriesArray all;     ///< All found entries, sorted
    ImageList::EntriesArray pending; ///< Entries found since last batch
    ImageList::EntriesArray removed; ///< Entries removed since last batch
    std::vector<std::filesystem::path> dirs; ///< Dirs found since last batch
    std::unordered_set<std::string> seen; ///< Found files
    PathSet watch_dirs; ///< Found dirs
    std::chrono::steady_clock::time_point last_flush; ///< Last batch time
    bool posted = false; ///< At least one batch was posted

    std::filesystem::path cache_path; ///< Cache file (empty: no cache)
    /** Cached entries not yet found by the scan. */
    ImageList::EntriesMap cached;
    ImageList::EntriesArray unposted; ///< Cached entries not yet posted
    bool cache_loaded = false; ///< Cache file was loaded
    bool dirty = false;        ///< List differs from the cache file
};

} // anonymous namespace

ImageList& ImageList::self()
{
    static ImageList singleton;
    return singleton;
}

ImageList::ImageList()
    : order(Defaults::imglist::order)
    , reverse(Defaults::imglist::reverse)
    , recursive(Defaults::imglist::recursive)
    , adjacent(Defaults::imglist::adjacent)
    , fsmon(Defaults::imglist::fsmon)
{
}

ImageList::~ImageList()
{
    scan_stop();
}

void ImageList::scan_start(std::vector<ScanSource> sources, ScanFilter filter,
                           ScanPost post, std::string cache_tag)
{
    scan_stop();

    scan_cancel = std::make_shared<std::atomic<bool>>(false);
    scan_active = true;
    scan_watch = fsmon;

    scan_thread = std::thread(
        [scanner = std::make_shared<Scanner>(
             scan_id, std::move(sources), std::move(filter), std::move(post),
             std::move(cache_tag), order, reverse, scan_cancel)] {
            scanner->run();
        });
}

void ImageList::scan_stop()
{
    if (scan_cancel) {
        *scan_cancel = true;
        scan_cancel.reset();
    }
    if (scan_thread.joinable()) {
        scan_thread.join();
    }
    ++scan_id; // drop batches that are already in the queue
    scan_active = false;
    scan_alias.clear();
}

bool ImageList::scan_apply(const ScanBatch& batch, EntriesArray& added,
                           EntriesArray& removed)
{
    added.clear();
    removed.clear();
    if (!scan_active || batch.id != scan_id) {
        return false; // batch of stopped scan
    }

    const std::scoped_lock lock(mutex);

    if (scan_watch) {
        FsMonitor& fsmon = FsMonitor::self();
        for (const auto& dir : batch.dirs) {
            fsmon.add(dir);
        }
    }

    // drop entries of files that vanished or changed since cached
    for (const ImageEntryPtr& entry : batch.removed) {
        const auto it = entries_map.find(entry->path.native());
        if (it != entries_map.end() && it->second == entry) {
            entries_map.erase(it);
            removed.push_back(entry);
        }
        entry->removed = true;
    }

    // register new entries, paths that already exist in the list (e.g. file
    // given on the command line) keep their current entries
    if (batch.index && batch.index->size() == batch.added.size()) {
        // map is prepared in background: merge current entries into it
        EntriesMap& index = *batch.index;
        for (const auto& [path, entry] : entries_map) {
            const auto it = index.find(path);
            if (it == index.end()) {
                index.emplace(path, entry);
            } else {
                // key is a view of the scanned entry: replace it too
                it->second->removed = true;
                scan_alias.insert_or_assign(it->second.get(), entry);
                index.erase(it);
                index.emplace(entry->path.native(), entry);
            }
        }
        entries_map.swap(index);
        added.reserve(batch.added.size());
        for (const ImageEntryPtr& entry : batch.added) {
            if (!entry->removed) {
                added.push_back(entry);
            }
        }
    } else {
        for (const ImageEntryPtr& entry : batch.added) {
            const auto [it, inserted] =
                entries_map.try_emplace(entry->path.native(), entry);
            if (inserted) {
                added.push_back(entry);
            } else {
                entry->removed = true;
                scan_alias.insert_or_assign(entry.get(), it->second);
            }
        }
    }

    // build the new list from the sorted scan result, skipping entries
    // removed while the scan was in progress
    EntriesArray list;
    list.reserve(batch.all.size());
    for (const ImageEntryPtr& entry : batch.all) {
        if (!entry->removed) {
            list.push_back(entry);
        } else {
            const auto it = scan_alias.find(entry.get());
            if (it != scan_alias.end() && !it->second->removed) {
                list.push_back(it->second);
            }
        }
    }

    bool resort = batch.order != order || batch.reverse != reverse;
    if (list.size() + removed.size() != entries_arr.size() + added.size()) {
        // entries added from other sources during the scan
        std::unordered_set<const ImageEntry*> known;
        known.reserve(list.size());
        for (const ImageEntryPtr& entry : list) {
            known.insert(entry.get());
        }
        for (const ImageEntryPtr& entry : entries_arr) {
            if (!known.contains(entry.get())) {
                list.push_back(entry);
            }
        }
        resort = true;
    }

    entries_arr = std::move(list);
    if (resort) {
        sort(entries_arr);
    }
    reindex();

    if (batch.done) {
        scan_active = false;
        scan_alias.clear();
    }

    return true;
}

ImageList::EntriesArray
ImageList::add(const std::vector<std::filesystem::path>& sources)
{
    EntriesArray added;
    const std::scoped_lock lock(mutex);
    for (const auto& path : sources) {
        EntriesArray entries = add_any(path);
        added.reserve(added.size() + entries.size());
        added.insert(added.end(), entries.begin(), entries.end());
    }

    sort();

    return added;
}

ImageEntryPtr ImageList::add_placeholder(const std::filesystem::path& path,
                                         const std::time_t mtime)
{
    const std::scoped_lock lock(mutex);
    const ImageEntryPtr entry = add_entry(path, mtime, 0);
    if (entry) {
        sort();
    }
    return entry;
}

ImageList::EntriesArray
ImageList::remove(const std::vector<std::filesystem::path>& sources)
{
    if (sources.empty()) {
        return {};
    }

    const std::scoped_lock lock(mutex);

    if (entries_arr.empty()) {
        return {};
    }

    EntriesArray removed;

    for (const auto& path : sources) {
        // get absolute path
        std::filesystem::path abs_path;
        if (ImageEntry::is_special(path)) {
            abs_path = path;
        } else {
            try {
                abs_path = std::filesystem::absolute(path).lexically_normal();
            } catch (const std::filesystem::filesystem_error&) {
                continue;
            }
        }

        std::error_code dir_ec;
        if (std::filesystem::is_directory(abs_path, dir_ec)) {
            // remove all child entries
            for (const ImageEntryPtr& entry : get_child(abs_path)) {
                entry->removed = true;
                entries_map.erase(entry->path.native());
                entries_arr[entry->index] = nullptr;
                removed.emplace_back(entry);
            }
            FsMonitor::self().remove(abs_path);
        } else {
            // remove single entry
            const auto it = entries_map.find(abs_path.native());
            if (it != entries_map.end()) {
                const ImageEntryPtr entry = it->second;
                entry->removed = true;
                entries_map.erase(it);
                entries_arr[entry->index] = nullptr;
                removed.emplace_back(entry);
            }
        }
    }

    if (!removed.empty()) {
        FsMonitor& fsmon = FsMonitor::self();
        for (const ImageEntryPtr& entry : removed) {
            fsmon.remove(entry->path);
        }
        std::erase_if(entries_arr, [](const ImageEntryPtr& entry) {
            return !entry;
        });
        reindex();
    }

    return removed;
}

void ImageList::remove(const ImageEntryPtr& entry)
{
    assert(entry);

    const std::scoped_lock lock(mutex);

    // The viewer and its preloader can both remove the same entry (when it
    // can't be decoded). The second call must not touch the list: the stale
    // index would point to another entry.
    if (entry->removed) {
        return;
    }

    entries_map.erase(entry->path.native());
    entries_arr.erase(entries_arr.begin() + entry->index);
    reindex(entry->index);

    entry->removed = true;
    FsMonitor::self().remove(entry->path);
}

ImageList::EntriesArray ImageList::clear()
{
    scan_stop(); // otherwise next batch brings entries back

    const std::scoped_lock lock(mutex);

    EntriesArray removed = entries_arr;
    entries_map.clear();
    entries_arr.clear();
    ++gen;

    FsMonitor::self().clear();

    return removed;
}

size_t ImageList::size()
{
    const std::shared_lock lock(mutex);
    return entries_arr.size();
}

void ImageList::set_order(const Order new_order)
{
    if (order != new_order || new_order == Order::Random) {
        order = new_order;
        const std::scoped_lock lock(mutex);
        sort();
    }
}

void ImageList::set_comparator(Comparator cmp)
{
    if (!cmp && !comparator) {
        return;
    }
    const std::scoped_lock lock(mutex);
    comparator = std::move(cmp);
    sort();
}

void ImageList::set_reverse(const bool enable)
{
    if (reverse != enable) {
        reverse = enable;
        const std::scoped_lock lock(mutex);
        sort();
    }
}

ImageEntryPtr ImageList::find(const std::filesystem::path& path)
{
    std::string search = path.string();
    if (search.empty()) {
        return nullptr;
    }
    if (!ImageEntry::is_special(search)) {
        try {
            search = std::filesystem::absolute(path).lexically_normal();
        } catch (const std::filesystem::filesystem_error&) {
            return nullptr;
        }
    }

    const std::shared_lock lock(mutex);

    const auto it = entries_map.find(search);
    return it == entries_map.end() ? nullptr : it->second;
}

ImageList::EntriesArray ImageList::get_all()
{
    const std::shared_lock lock(mutex);
    return entries_arr;
}

ImageEntryPtr ImageList::get(const ImageEntryPtr& from, const Dir dir)
{
    const std::shared_lock lock(mutex);

    if (entries_arr.empty()) {
        return nullptr;
    }
    if (dir == Dir::First) {
        return entries_arr.front();
    }
    if (dir == Dir::Last) {
        return entries_arr.back();
    }

    assert(from);

    // handle removed entry: return nearest entry
    if (from->removed) {
        const bool forward =
            dir != ImageList::Dir::Prev && dir != ImageList::Dir::PrevParent;
        return get_removed(from, forward);
    }

    ImageEntryPtr entry = nullptr;

    switch (dir) {
        case Dir::First:
        case Dir::Last:
            assert(false && "should be already handled");
            break;
        case Dir::Next:
            if (from->index + 1 < entries_arr.size()) {
                entry = entries_arr[from->index + 1];
            }
            break;
        case Dir::Prev:
            if (entries_arr.size() > 1 && from->index > 0) {
                entry = entries_arr[from->index - 1];
            }
            break;
        case Dir::NextParent:
            entry = get_diffparent(from, true);
            break;
        case Dir::PrevParent:
            entry = get_diffparent(from, false);
            break;
        case Dir::Random:
            entry = get_random(from);
            break;
    }

    return entry;
}

ImageEntryPtr ImageList::get(const ImageEntryPtr& from, const ssize_t distance)
{
    const std::shared_lock lock(mutex);

    assert(from && !from->removed);

    const size_t index = from->index;
    if (index + distance >= entries_arr.size() ||
        static_cast<ssize_t>(index) + distance < 0) {
        return nullptr;
    }

    return entries_arr[index + distance];
}

ssize_t ImageList::distance(const ImageEntryPtr& from, const ImageEntryPtr& to)
{
    const std::shared_lock lock(mutex);

    assert(from && !from->removed);
    assert(from->index < entries_arr.size());
    assert(to && !to->removed);
    assert(to->index < entries_arr.size());

    return static_cast<ssize_t>(to->index) - static_cast<ssize_t>(from->index);
}

ImageList::EntriesArray
ImageList::get_child(const std::filesystem::path& path) const
{
    assert(path.is_absolute());

    EntriesArray child;

    for (const ImageEntryPtr& entry : entries_arr) {
        const std::filesystem::path rel = entry->path.lexically_relative(path);
        if (!rel.empty() && !rel.string().starts_with("..")) {
            child.push_back(entry);
        }
    }

    return child;
}

ImageEntryPtr ImageList::get_removed(const ImageEntryPtr& entry,
                                     const bool forward) const
{
    assert(entry->removed);

    if (entries_arr.empty()) {
        return nullptr;
    }

    size_t index = entry->index;
    if (index && !forward) {
        --index;
    }
    index = std::min(index, entries_arr.size() - 1);

    return entries_arr[index];
}

ImageEntryPtr ImageList::get_diffparent(const ImageEntryPtr& from,
                                        const bool forward) const
{
    assert(from && !from->removed);

    const std::filesystem::path from_parent = from->path.parent_path();

    const ssize_t size = static_cast<ssize_t>(entries_arr.size());
    const ssize_t direction_sign = forward ? 1 : -1;
    ssize_t index = static_cast<ssize_t>(from->index) + direction_sign;

    while (index >= 0 && index < size) {
        ImageEntryPtr entry = entries_arr[index];
        if (from_parent != entry->path.parent_path()) {
            return entry;
        }
        index += direction_sign;
    }

    return nullptr;
}

ImageEntryPtr ImageList::get_random(const ImageEntryPtr& excl) const
{
    ImageEntryPtr entry = excl;

    if (entries_arr.size() > 1) {
        while (entry == excl) {
            entry = entries_arr[rand() % entries_arr.size()];
        }
    }

    return entry == excl ? nullptr : entry;
}

ImageList::EntriesArray ImageList::add_any(const std::filesystem::path& path)
{
    if (ImageEntry::is_special(path)) {
        const ImageEntryPtr entry = add_entry(path);
        if (entry) {
            return { entry };
        }
        return {};
    }

    std::filesystem::path abs_path;
    try {
        abs_path = std::filesystem::absolute(path).lexically_normal();
        if (!std::filesystem::exists(abs_path)) {
            Log::warning("File {} not found, skipped", abs_path.string());
            return {};
        }
    } catch (const std::filesystem::filesystem_error&) {
        Log::warning("Invalid path {}, skipped", path.string());
        return {};
    }

    EntriesArray added;
    std::error_code dir_ec; // the path can vanish after the check above
    if (!std::filesystem::is_directory(abs_path, dir_ec)) {
        const ImageEntryPtr entry = add_file(abs_path);
        if (entry) {
            added.emplace_back(entry);
        }

        if (!adjacent) {
            if (fsmon) {
                FsMonitor::self().add(abs_path);
            }
            return added;
        }
        abs_path = abs_path.parent_path();
    }

    EntriesArray dir_entries = add_dir(abs_path);
    if (!dir_entries.empty()) {
        sort(dir_entries);
        added.reserve(added.size() + dir_entries.size());
        added.insert(added.end(), dir_entries.begin(), dir_entries.end());
    }

    return added;
}

ImageList::EntriesArray ImageList::add_dir(const std::filesystem::path& path)
{
    EntriesArray added;

    if (fsmon) {
        FsMonitor::self().add(path);
    }

    // Handle errors per entry: a single entry that can't be checked (a
    // symlink loop, a link to an unreadable directory, an I/O error) must not
    // hide all the other files of the directory.
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        const std::filesystem::path child_path = it->path();
        std::error_code child_ec;
        if (std::filesystem::is_directory(child_path, child_ec)) {
            if (recursive && !is_hidden(child_path)) {
                EntriesArray entries = add_dir(child_path);
                added.reserve(added.size() + entries.size());
                added.insert(added.end(), entries.begin(), entries.end());
            }
        } else if (child_ec) {
            Log::warning("File {} skipped: {}", child_path.string(),
                         child_ec.message());
        } else if (!file_filter || file_filter(child_path)) {
            const ImageEntryPtr entry = add_file(child_path);
            if (entry) {
                added.push_back(entry);
            }
        }
        it.increment(ec);
    }

    return added;
}

ImageEntryPtr ImageList::add_file(const std::filesystem::path& path)
{
    assert(path.is_absolute());

    const ImageEntryPtr entry = create_entry(path);
    return entry ? add_entry(path, entry->mtime, entry->size) : nullptr;
}

ImageEntryPtr ImageList::add_entry(const std::filesystem::path& path,
                                   const std::time_t mtime, const size_t size)
{
    ImageEntryPtr entry = nullptr;

    if (!entries_map.contains(path.native())) {
        entry = std::make_shared<ImageEntry>();
        entry->path = path;
        entry->mtime = mtime;
        entry->size = size;
        entries_map.insert({ entry->path.native(), entry });
        entries_arr.push_back(entry);
        ++gen;
    }

    return entry;
}

void ImageList::sort()
{
    sort(entries_arr);
    reindex();
}

void ImageList::sort(EntriesArray& entries) const
{
    if (comparator) {
        std::sort(entries.begin(), entries.end(),
                  [this](const ImageEntryPtr& l, const ImageEntryPtr& r) {
                      return comparator(*l, *r);
                  });
    } else if (order == Order::None) {
        // nothing to do
    } else if (order == Order::Random) {
        // shuffle list
        EntriesArray tmp(entries.begin(), entries.end());
        static std::random_device rdev;
        static std::mt19937 engine(rdev());
        std::shuffle(tmp.begin(), tmp.end(), engine);
        entries.assign(tmp.begin(), tmp.end());
    } else {
        std::sort(entries.begin(), entries.end(),
                  [this](const ImageEntryPtr& l, const ImageEntryPtr& r) {
                      const bool cmp = compare_entries(*l, *r, order);
                      return reverse ? !cmp : cmp;
                  });
    }
}

void ImageList::reindex(const size_t index)
{
    ++gen;
    const size_t sz = entries_arr.size();
    for (size_t i = index; i < sz; ++i) {
        entries_arr[i]->index = i;
    }
}
