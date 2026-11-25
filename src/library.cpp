// SPDX-License-Identifier: MIT
// Photo library: saved albums, Photos/Albums views and their overlay UI.

#include "library.hpp"

#include "application.hpp"
#include "folderpicker.hpp"
#include "font.hpp"
#include "gallery.hpp"
#include "imagelist.hpp"
#include "log.hpp"
#include "shapes.hpp"
#include "videoplayer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace {

// UI metrics in logical pixels
constexpr double SWITCHER_HEIGHT = 44;
constexpr double SWITCHER_INSET = 4;
constexpr double SWITCHER_PAD = 22;
constexpr double SWITCHER_BOTTOM = 22;
constexpr double HEADER_HEIGHT = 60;
constexpr double BUTTON_HEIGHT = 34;
constexpr double BUTTON_PAD = 16;
constexpr double FONT_TITLE = 17;
constexpr double FONT_SMALL = 13;
constexpr double FILTER_GAP = 12;
constexpr double MENU_WIDTH = 230;
constexpr double MENU_ITEM = 52;
constexpr double MENU_PAD = 8;
constexpr double MENU_RADIUS = 24;
constexpr double MENU_ICON = 24;
constexpr double MENU_SPEED = 22.0;

// Colors
constexpr argb_t CLR_PANEL = { 0xf0, 0x2a, 0x2a, 0x2c };
constexpr argb_t CLR_HIGHLIGHT = { 0xff, 0x4a, 0x4a, 0x4e };
constexpr argb_t CLR_TEXT = { 0xff, 0xf2, 0xf2, 0xf2 };
constexpr argb_t CLR_TEXT_DIM = { 0xff, 0xb4, 0xb4, 0xb8 };
constexpr argb_t CLR_BUTTON = { 0xff, 0x3a, 0x3a, 0x3e };
constexpr argb_t CLR_ACCENT = { 0xff, 0x8a, 0xb4, 0xf8 };
constexpr argb_t CLR_ACCENT_TEXT = { 0xff, 0x0e, 0x1f, 0x3a };
constexpr argb_t CLR_MENU = { 0xf5, 0x2c, 0x2c, 0x2f };
constexpr argb_t CLR_SEPARATOR = { 0x28, 0xff, 0xff, 0xff };

// Highlight slide speed
constexpr double HIGHLIGHT_SPEED = 18.0;

/** File extensions treated as album items (cover/count only). */
const std::unordered_set<std::string> MEDIA_EXT = {
    // images
    "jpg", "jpeg", "jpe", "jfif", "png", "apng", "gif", "webp", "heic",
    "heif", "hif", "avif", "jxl", "bmp", "dib", "tif", "tiff", "svg", "svgz",
    "tga", "qoi", "exr", "jp2", "j2k", "jpx", "pnm", "pbm", "pgm", "ppm",
    "ff", "dcm", "xbm", "kra", "six", "sixel",
    // camera raw
    "dng", "cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "orf",
    "rw2", "raf", "pef", "srw", "erf", "kdc", "dcr", "mrw", "3fr", "x3f",
    "iiq", "rwl",
    // video
    "mp4", "mkv", "webm", "mov", "avi", "m4v", "3gp", "mts", "m2ts", "mpg",
    "mpeg", "wmv", "flv", "ogv",
};

/** Normalize directory path: absolute, no dots, no trailing slash. */
std::filesystem::path normalize(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec) {
        abs = path;
    }
    abs = abs.lexically_normal();
    if (!abs.has_filename() && abs.has_parent_path() &&
        abs != abs.root_path()) {
        abs = abs.parent_path();
    }
    return abs;
}

} // namespace

bool Library::is_media(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    if (ext.size() < 2) {
        return false;
    }
    ext.erase(0, 1);
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return MEDIA_EXT.contains(ext);
}

namespace {

/** Extensions without reliable signature: accepted without content check. */
const std::unordered_set<std::string> NO_SIGNATURE_EXT = {
    "tga", "svg", "svgz", "xbm", "six", "sixel", "mts", "m2ts", "mpg", "mpeg",
};

/** Get lower case file extension without dot. */
std::string lower_ext(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    if (!ext.empty()) {
        ext.erase(0, 1);
    }
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

/**
 * Check file content: does the header look like any known photo/video
 * format. Conservative: rejects only files that match no media signature
 * (e.g. archives or documents with an image extension).
 * @param path file to check
 * @return false if the file is definitely not a media file
 */
bool sniff_media(const std::filesystem::path& path)
{
    const std::string ext = lower_ext(path);
    if (NO_SIGNATURE_EXT.contains(ext)) {
        return true;
    }

    uint8_t hdr[136] = {};
    size_t len = 0;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return true; // unreadable now: let the decoder decide later
        }
        file.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        len = static_cast<size_t>(file.gcount());
    }
    if (len < 4) {
        return false; // empty or truncated
    }

    const auto at = [&](const size_t offset, const std::string_view sig) {
        return offset + sig.size() <= len &&
            std::memcmp(hdr + offset, sig.data(), sig.size()) == 0;
    };
    using namespace std::string_view_literals;

    return at(0, "\xFF\xD8\xFF"sv) ||                     // jpeg
        at(0, "\x89PNG"sv) ||                              // png
        at(0, "GIF8"sv) ||                                 // gif
        (at(0, "RIFF"sv) && (at(8, "WEBP"sv) || at(8, "AVI "sv))) ||
        at(0, "BM"sv) ||                                   // bmp
        at(0, "II*\0"sv) || at(0, "MM\0*"sv) ||             // tiff, raw
        at(0, "IIRO"sv) || at(0, "IIRS"sv) || at(0, "IIU\0"sv) || // orf, rw2
        at(0, "FUJIFILM"sv) || at(6, "HEAPCCDR"sv) ||      // raf, crw
        at(0, "FOVb"sv) || at(0, "\0MRM"sv) ||              // x3f, mrw
        at(4, "ftyp"sv) || at(4, "moov"sv) || at(4, "mdat"sv) || // iso bmff
        at(4, "wide"sv) || at(4, "free"sv) || at(4, "skip"sv) ||
        at(0, "\xFF\x0A"sv) ||                              // jxl
        at(0, "\0\0\0\x0CJXL "sv) || at(0, "\0\0\0\x0CjP  "sv) ||
        at(0, "\xFF\x4F\xFF\x51"sv) ||                     // j2k
        at(0, "\x1A\x45\xDF\xA3"sv) ||                     // mkv, webm
        at(0, "\x30\x26\xB2\x75"sv) ||                     // wmv
        at(0, "FLV"sv) || at(0, "OggS"sv) ||
        at(0, "\0\0\x01\xBA"sv) || at(0, "\0\0\x01\xB3"sv) || // mpeg
        at(0, "qoif"sv) || at(0, "\x76\x2F\x31\x01"sv) ||   // qoi, exr
        (hdr[0] == 'P' && hdr[1] >= '1' && hdr[1] <= '7') || // pnm
        at(0, "farbfeld"sv) || at(128, "DICM"sv) ||
        (ext == "kra" && at(0, "PK\x03\x04"sv));          // krita (zip)
}

/** Get path of the cache file with known broken media files. */
std::filesystem::path bad_files_path()
{
    std::filesystem::path dir;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        dir = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            return {};
        }
        dir = std::filesystem::path(home) / ".cache";
    }
    return dir / "swayimg" / "bad-media";
}

/** Get file stamp (size and mtime) to detect changes. */
std::pair<uintmax_t, int64_t> file_stamp(const std::filesystem::path& path)
{
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return { 0, 0 };
    }
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return { 0, 0 };
    }
    return { size, static_cast<int64_t>(mtime.time_since_epoch().count()) };
}

/** Get path of the config file with saved albums. */
std::filesystem::path config_file()
{
    std::filesystem::path dir;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        dir = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            return {};
        }
        dir = std::filesystem::path(home) / ".config";
    }
    return dir / "swayimg" / "albums";
}

/** Convert UTF-8 to wide string. */
std::wstring to_wide(const std::string& text)
{
    std::wstring wide(text.size() + 1, 0);
    const size_t len = std::mbstowcs(wide.data(), text.c_str(), wide.size());
    if (len == static_cast<size_t>(-1)) {
        wide.clear();
        for (const char c : text) {
            wide += (c < ' ' || c > '~') ? L'?' : static_cast<wchar_t>(c);
        }
        return wide;
    }
    wide.resize(len);
    return wide;
}

/** Get text width in pixels. */
size_t text_width(FontSet& font, const std::wstring& text)
{
    size_t width = 0;
    for (const wchar_t ch : text) {
        const Pixmap* glyph = font.get_glyph(ch);
        if (glyph) {
            width += glyph->width();
        }
    }
    return width;
}

/** Get text height in pixels. */
size_t text_height(FontSet& font)
{
    const Pixmap* glyph = font.get_glyph(L'A');
    return glyph ? glyph->height() : 0;
}

/** Trim text to fit into specified width. */
std::wstring fit_text(FontSet& font, const std::wstring& text,
                      const size_t max_width)
{
    if (text_width(font, text) <= max_width) {
        return text;
    }
    std::wstring trimmed = text;
    while (!trimmed.empty() && text_width(font, trimmed + L'…') > max_width) {
        trimmed.pop_back();
    }
    return trimmed.empty() ? trimmed : trimmed + L'…';
}

/** Draw text line. */
void draw_text(Canvas& wnd, FontSet& font, const std::wstring& text,
               const Point& pos, const argb_t& color)
{
    Point pt = pos;
    for (const wchar_t ch : text) {
        const Pixmap* glyph = font.get_glyph(ch);
        if (glyph) {
            if (glyph->format() == Pixmap::ARGB) {
                wnd.blend(*glyph, pt);
            } else {
                wnd.mask(*glyph, pt, color);
            }
            pt.x += glyph->width();
        }
    }
}

/** Apply opacity to color. */
argb_t fade(const argb_t& color, const double opacity)
{
    argb_t clr = color;
    clr.a = static_cast<argb_t::channel>(
        std::clamp(color.a * opacity, 0.0, 255.0));
    return clr;
}

/** Distance from point to segment. */
double segment_distance(const double px, const double py, const double ax,
                        const double ay, const double bx, const double by)
{
    const double dx = bx - ax;
    const double dy = by - ay;
    const double len = dx * dx + dy * dy;
    double t = len ? ((px - ax) * dx + (py - ay) * dy) / len : 0;
    t = std::clamp(t, 0.0, 1.0);
    const double cx = ax + t * dx - px;
    const double cy = ay + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

} // namespace

Library& Library::self()
{
    static Library singleton;
    return singleton;
}

void Library::startup(StartupParams& params)
{
    load();
    load_bad();

    Application::self().add_fdpoll(toast_timer, [this] {
        toast_timer.reset(0, 0);
        Application::redraw(); // start hiding the notification
    });

    // remember files that look like media but can't be decoded
    Gallery::self().on_load_failed = [](const std::filesystem::path& path) {
        Application::self().add_event(AppEvent::Callback { [path] {
            Library::self().mark_bad({ path });
        } });
    };
    struct HighlightInit {
        Library& lib;
        ~HighlightInit() { lib.hl_pos = lib.view == View::Photos ? 0.0 : 1.0; }
    } hl_init { *this };

    std::vector<std::filesystem::path>& sources = params.sources;

    if (sources.empty()) {
        // started without arguments: Photos view (see fill_photos)
        view = View::Photos;
        if (!params.mode.is_locked()) {
            params.mode = AppMode::Gallery;
        }
        return;
    }

    if (sources.size() == 1 && sources[0] == ImageEntry::SRC_STDIN) {
        view = View::None;
        return;
    }

    // started with files/directories: open folder of the first one as album
    bool all_dirs = true;
    for (const auto& src : sources) {
        std::error_code ec;
        if (!std::filesystem::is_directory(src, ec)) {
            all_dirs = false;
        }
    }
    std::error_code ec;
    const std::filesystem::path first = normalize(sources[0]);
    album = std::filesystem::is_directory(first, ec) ? first
                                                     : first.parent_path();
    view = View::Album;
    adjacent_start = !all_dirs;
    if (all_dirs && !params.mode.is_locked()) {
        params.mode = AppMode::Gallery;
    }
}

void Library::fill_photos(ImageList& il) const
{
    const bool recursive = il.recursive;
    for (const Album& it : albums) {
        std::error_code ec;
        if (it.in_photos && std::filesystem::is_directory(it.path, ec)) {
            il.recursive = it.recursive;
            il.add({ it.path });
        }
    }
    il.recursive = recursive;
}

bool Library::album_recursive(const std::filesystem::path& dir) const
{
    const Album* saved = find(dir);
    if (saved) {
        return saved->recursive;
    }
    return normalize(dir) == album && temp_recursive;
}

void Library::set_recursive(const std::filesystem::path& dir,
                            const bool enable)
{
    const std::filesystem::path path = normalize(dir);
    bool saved = false;
    for (Album& it : albums) {
        if (it.path == path) {
            it.recursive = enable;
            saved = true;
        }
    }
    if (saved) {
        save();
    } else if (path == album) {
        temp_recursive = enable;
    }
    if (view == View::Album && path == album) {
        open_album(album); // reload with/without nested folders
    }
}

bool Library::accepts(const std::filesystem::path& path) const
{
    if (!is_media(path) || is_bad(path)) {
        return false;
    }
    switch (filter) {
        case Filter::All:
            return true;
        case Filter::Photos:
            return !VideoPlayer::is_video(path);
        case Filter::Videos:
            return VideoPlayer::is_video(path);
    }
    return true;
}

void Library::set_filter(const Filter new_filter)
{
    if (filter == new_filter) {
        return;
    }
    filter = new_filter;
    if (view == View::Photos) {
        show_photos();
    } else if (view == View::Album) {
        open_album(album);
    }
}

void Library::show_photos()
{
    set_view(
        View::Photos,
        [this](ImageList& il) {
            fill_photos(il);
        },
        {}, ImageList::self().fsmon);
}

void Library::show_albums(const std::filesystem::path& select)
{
    cover_album.clear();
    album_info.clear();

    std::vector<std::filesystem::path> covers;
    std::vector<std::pair<std::filesystem::path, std::time_t>> empty;
    std::filesystem::path select_cover;

    for (const Album& it : albums) {
        const bool recursive = it.recursive;
        AlbumInfo info;
        std::vector<std::pair<std::filesystem::file_time_type,
                              std::filesystem::path>>
            candidates;

        const auto check = [&](const std::filesystem::directory_entry& de) {
            std::error_code ec;
            if (!de.is_regular_file(ec) || !is_media(de.path()) ||
                is_bad(de.path())) {
                return;
            }
            ++info.count;
            const auto mtime = de.last_write_time(ec);
            candidates.emplace_back(ec ? std::filesystem::file_time_type {}
                                       : mtime,
                                    de.path());
        };

        std::error_code ec;
        const auto opts =
            std::filesystem::directory_options::skip_permission_denied;
        if (recursive) {
            std::filesystem::recursive_directory_iterator dit(it.path, opts,
                                                              ec);
            for (; !ec && dit != std::filesystem::end(dit); dit.increment(ec)) {
                check(*dit);
            }
        } else {
            std::filesystem::directory_iterator dit(it.path, opts, ec);
            for (; !ec && dit != std::filesystem::end(dit); dit.increment(ec)) {
                check(*dit);
            }
        }

        // cover: the newest file that looks like a real image
        std::ranges::sort(candidates, [](const auto& l, const auto& r) {
            return l.first > r.first;
        });
        std::vector<std::filesystem::path> broken;
        for (const auto& [mtime, path] : candidates) {
            if (sniff_media(path)) {
                info.cover = path;
                break;
            }
            broken.push_back(path);
            --info.count;
        }
        mark_bad(broken);

        if (info.cover.empty()) {
            // empty album: the directory itself is the grid entry
            std::error_code mt_ec;
            const auto ftime = std::filesystem::last_write_time(it.path, mt_ec);
            std::time_t mtime = 0;
            if (!mt_ec) {
                mtime = std::chrono::system_clock::to_time_t(
                    std::chrono::time_point_cast<
                        std::chrono::system_clock::duration>(
                        ftime - std::filesystem::file_time_type::clock::now() +
                        std::chrono::system_clock::now()));
            }
            empty.emplace_back(it.path, mtime);
            info.cover = it.path;
        } else {
            info.cover = info.cover.lexically_normal();
            covers.push_back(info.cover);
        }
        cover_album.insert_or_assign(info.cover, it.path);
        if (it.path == select) {
            select_cover = info.cover;
        }
        album_info.insert_or_assign(it.path, info);
    }

    set_view(
        View::Albums,
        [covers, empty](ImageList& il) {
            if (!covers.empty()) {
                il.add(covers);
            }
            for (const auto& [path, mtime] : empty) {
                il.add_placeholder(path, mtime);
            }
        },
        select_cover, false);
}

void Library::open_album(const std::filesystem::path& dir)
{
    const std::filesystem::path path = normalize(dir);
    if (path != album) {
        temp_recursive = false;
    }
    album = path;
    const bool recursive = album_recursive(album);
    set_view(
        View::Album,
        [path, recursive](ImageList& il) {
            const bool prev = il.recursive;
            il.recursive = recursive;
            il.add({ path });
            il.recursive = prev;
        },
        {}, ImageList::self().fsmon);
}

void Library::back()
{
    if (view == View::Album) {
        show_albums(album);
    }
}

bool Library::add_album(const std::filesystem::path& dir)
{
    const std::filesystem::path path = normalize(dir);
    if (find(path)) {
        return false;
    }
    albums.push_back({ .path = path,
                       .in_photos = true,
                       .recursive = path == album && temp_recursive });
    std::ranges::sort(albums, [](const Album& l, const Album& r) {
        return l.path < r.path;
    });
    save();
    Application::redraw();
    return true;
}

void Library::remove_album(const std::filesystem::path& dir)
{
    const std::filesystem::path path = normalize(dir);
    std::erase_if(albums, [&path](const Album& it) {
        return it.path == path;
    });
    save();
    Application::redraw();
}

void Library::set_in_photos(const std::filesystem::path& dir,
                            const bool enable)
{
    const std::filesystem::path path = normalize(dir);
    for (Album& it : albums) {
        if (it.path == path) {
            it.in_photos = enable;
        }
    }
    save();
    Application::redraw();
}

const Library::Album* Library::find(const std::filesystem::path& dir) const
{
    const std::filesystem::path path = normalize(dir);
    for (const Album& it : albums) {
        if (it.path == path) {
            return &it;
        }
    }
    return nullptr;
}

bool Library::activate_entry(const ImageEntryPtr& entry)
{
    if (view != View::Albums || !entry) {
        return false;
    }
    const auto it = cover_album.find(entry->path);
    if (it != cover_album.end()) {
        open_album(it->second);
    }
    return true; // never open album covers in the viewer
}

void Library::load()
{
    albums.clear();

    const std::filesystem::path path = config_file();
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // "<+|-><flags> <path>", flags: r - include subfolders
        const size_t space = line.find(' ');
        if (line.empty() || (line[0] != '+' && line[0] != '-') ||
            space == std::string::npos || space + 1 >= line.size()) {
            continue;
        }
        Album it;
        it.in_photos = line[0] == '+';
        it.recursive = line.substr(1, space - 1).find('r') != std::string::npos;
        it.path = normalize(line.substr(space + 1));
        if (!find(it.path)) {
            albums.push_back(it);
        }
    }
}

void Library::save() const
{
    const std::filesystem::path path = config_file();
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            Log::error("Unable to save albums to {}", path.string());
            return;
        }
        file << "# swayimg albums: '+' shown in Photos, '-' hidden, "
                "'r' includes subfolders\n";
        for (const Album& it : albums) {
            file << (it.in_photos ? '+' : '-') << (it.recursive ? "r" : "")
                 << ' ' << it.path.string() << '\n';
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        Log::error("Unable to save albums to {}: {}", path.string(),
                   ec.message());
    }
}

void Library::set_view(const View new_view,
                       const std::function<void(ImageList&)>& load,
                       const std::filesystem::path& select, const bool watch)
{
    view = new_view;

    Gallery& gallery = Gallery::self();
    if (view == View::Albums) {
        gallery.tile_overlay = [this](Canvas& wnd, const ImageEntryPtr& entry,
                                      const Rectangle& tile, double opacity) {
            draw_caption(wnd, entry, tile, opacity);
        };
    } else {
        gallery.tile_overlay = nullptr;
    }

    update_margins();

    // replace image list (directories only, adjacent files not needed)
    ImageList& il = ImageList::self();
    const bool adjacent = il.adjacent;
    const bool fsmon = il.fsmon;
    il.adjacent = false;
    il.fsmon = watch;
    Application::self().replace_images(load, select);
    il.adjacent = adjacent;
    il.fsmon = fsmon;

    start_validation();
}

void Library::update_margins()
{
    if (view == View::None) {
        return;
    }

    if (Ui* ui = Application::get_ui()) {
        scale = ui->get_scale();
    }

    const size_t top = (view == View::Album || view == View::Albums)
        ? dp(HEADER_HEIGHT)
        : 0;
    const size_t bottom = dp(SWITCHER_HEIGHT + SWITCHER_BOTTOM * 2);
    if (top != margin_top || bottom != margin_bottom) {
        margin_top = top;
        margin_bottom = bottom;
        Gallery::self().set_margins(top, bottom);
    }
}

ssize_t Library::dp(const double px) const
{
    return static_cast<ssize_t>(std::lround(px * scale));
}

void Library::prepare_fonts()
{
    if (!font_title) {
        // never freed: avoid destruction order issues of static instances
        font_title = new FontSet();
        font_small = new FontSet();
        for (FontSet* font : { font_title, font_small }) {
            if (!font->load(std::string("sans-serif"))) {
                font->load(std::string("monospace"));
            }
        }
        font_title->set_size(FONT_TITLE);
        font_small->set_size(FONT_SMALL);
    }
    if (font_scale != scale) {
        font_scale = scale;
        font_title->set_scale(scale);
        font_small->set_scale(scale);
    }
}

void Library::draw(Canvas& wnd, const double opacity)
{
    hits.clear();
    if (view == View::None) {
        return;
    }

    update_margins();
    prepare_fonts();

    if (view == View::Album) {
        draw_header(wnd, opacity);
    } else if (view == View::Albums) {
        draw_albums_header(wnd, opacity);
    }

    // hint for empty views
    if (ImageList::self().size() == 0) {
        std::string hint;
        if (filter != Filter::All && view != View::Albums) {
            hint = filter == Filter::Videos ? "No videos here"
                                            : "No photos here";
        } else if (view == View::Photos) {
            hint = albums.empty()
                ? "No albums yet: add folders in Albums"
                : "No photos: show some albums in Photos";
        } else if (view == View::Albums) {
            hint = albums.empty() ? "No albums yet: add a folder to get started"
                                  : "Saved albums have no images";
        } else {
            hint = album_recursive(album)
                ? "No images in this folder and its subfolders"
                : "No images here: try Include subfolders";
        }
        const std::wstring text = to_wide(hint);
        const Size wsz = wnd.size();
        const ssize_t width = text_width(*font_title, text);
        draw_text(wnd, *font_title, text,
                  { static_cast<ssize_t>(wsz.width) / 2 - width / 2,
                    static_cast<ssize_t>(wsz.height) / 2 + dp(70) },
                  fade(CLR_TEXT_DIM, opacity));
    }

    const Rectangle switcher = draw_switcher(wnd, opacity);
    if (view == View::Albums) {
        menu_open = false; // filter applies to photos only
        menu_anim = 0;
    } else {
        const Rectangle button = draw_filter_button(wnd, switcher, opacity);
        if (menu_anim > 0) {
            draw_filter_menu(wnd, button, opacity * menu_anim);
        }
    }
}

Rectangle Library::draw_filter_button(Canvas& wnd, const Rectangle& switcher,
                                      const double opacity)
{
    const size_t size = switcher.height;
    const Rectangle rect { switcher.x + static_cast<ssize_t>(switcher.width) +
                               dp(FILTER_GAP),
                           switcher.y, size, size };
    wnd.mask(pill({ size, size }), { rect.x, rect.y },
             fade(menu_open ? CLR_HIGHLIGHT : CLR_PANEL, opacity));
    const size_t icon = dp(22);
    wnd.mask(Shapes::icon(Shapes::Icon::Filter, icon),
             { rect.x + static_cast<ssize_t>(size / 2 - icon / 2),
               rect.y + static_cast<ssize_t>(size / 2 - icon / 2) },
             fade(filter == Filter::All ? CLR_TEXT : CLR_ACCENT, opacity));
    hits.push_back({ rect, [this] {
                        menu_open = !menu_open;
                    } });
    return rect;
}

void Library::draw_filter_menu(Canvas& wnd, const Rectangle& button,
                               const double opacity)
{
    struct Item {
        Filter value;
        Shapes::Icon icon;
        const wchar_t* label;
    };
    static constexpr Item items[] = {
        { Filter::All,    Shapes::Icon::All,   L"All"    },
        { Filter::Photos, Shapes::Icon::Photo, L"Photos" },
        { Filter::Videos, Shapes::Icon::Video, L"Videos" },
    };
    constexpr size_t count = sizeof(items) / sizeof(items[0]);

    const size_t width = dp(MENU_WIDTH);
    const size_t item_h = dp(MENU_ITEM);
    const size_t pad = dp(MENU_PAD);
    const size_t height = item_h * count + pad * 2;
    const ssize_t slide = std::lround((1.0 - menu_anim) * dp(10));
    const Rectangle panel {
        button.x + static_cast<ssize_t>(button.width) -
            static_cast<ssize_t>(width),
        button.y - dp(FILTER_GAP) - static_cast<ssize_t>(height) + slide,
        width, height
    };

    wnd.mask(Shapes::rounded_rect({ width, height }, dp(MENU_RADIUS)),
             { panel.x, panel.y }, fade(CLR_MENU, opacity));

    const size_t icon = dp(MENU_ICON);
    const ssize_t th = text_height(*font_title);
    std::vector<HitArea> menu_hits;
    for (size_t i = 0; i < count; ++i) {
        const Item& item = items[i];
        const bool selected = item.value == filter;
        const argb_t color = selected ? CLR_ACCENT : CLR_TEXT;
        const ssize_t y = panel.y + pad + i * item_h;
        const ssize_t cy = y + static_cast<ssize_t>(item_h / 2);
        const ssize_t text_x = panel.x + dp(60);

        wnd.mask(Shapes::icon(item.icon, icon),
                 { panel.x + dp(20), cy - static_cast<ssize_t>(icon / 2) },
                 fade(color, opacity));
        draw_text(wnd, *font_title, item.label, { text_x, cy - th / 2 },
                  fade(color, opacity));
        if (selected) {
            wnd.mask(Shapes::icon(Shapes::Icon::Check, icon),
                     { panel.x + static_cast<ssize_t>(width) - dp(20) -
                           static_cast<ssize_t>(icon),
                       cy - static_cast<ssize_t>(icon / 2) },
                     fade(CLR_ACCENT, opacity));
        }
        if (i + 1 < count) {
            wnd.fill_blend({ text_x, y + static_cast<ssize_t>(item_h),
                             static_cast<size_t>(panel.x + width - dp(16) -
                                                 text_x),
                             static_cast<size_t>(std::max<ssize_t>(dp(1), 1)) },
                           fade(CLR_SEPARATOR, opacity));
        }

        const Filter value = item.value;
        menu_hits.push_back({ { panel.x, y, width, item_h }, [this, value] {
                                 menu_open = false;
                                 set_filter(value);
                             } });
    }
    // swallow clicks on the panel padding
    menu_hits.push_back({ panel, [] {} });

    if (menu_open) {
        // menu is on top: check its items first
        hits.insert(hits.begin(), menu_hits.begin(), menu_hits.end());
    }
}

Rectangle Library::draw_switcher(Canvas& wnd, const double opacity)
{
    const Size wsz = wnd.size();

    const std::wstring labels[2] = { L"Photos", L"Albums" };
    size_t seg_width = 0;
    for (const auto& label : labels) {
        seg_width = std::max(seg_width, text_width(*font_title, label));
    }
    seg_width += dp(SWITCHER_PAD) * 2;

    const ssize_t inset = dp(SWITCHER_INSET);
    const size_t height = dp(SWITCHER_HEIGHT);
    const size_t width = seg_width * 2 + inset * 2;
    const Point pos {
        .x = static_cast<ssize_t>(wsz.width / 2) -
            static_cast<ssize_t>(width / 2),
        .y = static_cast<ssize_t>(wsz.height) - dp(SWITCHER_BOTTOM) -
            static_cast<ssize_t>(height),
    };

    // panel and sliding highlight
    wnd.mask(pill({ width, height }), pos, fade(CLR_PANEL, opacity));
    const size_t hl_height = height - inset * 2;
    const Point hl_pos_px {
        .x = pos.x + inset + static_cast<ssize_t>(std::lround(hl_pos * seg_width)),
        .y = pos.y + inset,
    };
    wnd.mask(pill({ seg_width, hl_height }), hl_pos_px,
             fade(CLR_HIGHLIGHT, opacity));

    // labels
    const double target = view == View::Photos ? 0.0 : 1.0;
    const ssize_t th = text_height(*font_title);
    for (size_t i = 0; i < 2; ++i) {
        const ssize_t tw = text_width(*font_title, labels[i]);
        const ssize_t seg_x = pos.x + inset + i * seg_width;
        const Point tpos {
            .x = seg_x + static_cast<ssize_t>(seg_width / 2) - tw / 2,
            .y = pos.y + static_cast<ssize_t>(height / 2) - th / 2,
        };
        const bool active = std::abs(target - static_cast<double>(i)) < 0.5;
        draw_text(wnd, *font_title, labels[i], tpos,
                  fade(active ? CLR_TEXT : CLR_TEXT_DIM, opacity));

        const Rectangle rect { seg_x, pos.y, seg_width, height };
        if (i == 0) {
            hits.push_back({ rect, [this] {
                                if (view != View::Photos) {
                                    show_photos();
                                }
                            } });
        } else {
            hits.push_back({ rect, [this] {
                                if (view == View::Album) {
                                    back();
                                } else if (view != View::Albums) {
                                    show_albums();
                                }
                            } });
        }
    }
    return { pos.x, pos.y, width, height };
}

void Library::add_folders()
{
    FolderPicker::pick(
        "Add album folders",
        [this](const std::vector<std::filesystem::path>& paths) {
            std::filesystem::path first;
            for (const auto& path : paths) {
                std::error_code ec;
                if (std::filesystem::is_directory(path, ec) &&
                    add_album(path) && first.empty()) {
                    first = normalize(path);
                }
            }
            if (!first.empty() && view == View::Albums) {
                show_albums(first); // refresh grid, select new album
            }
        });
}

void Library::draw_albums_header(Canvas& wnd, const double opacity)
{
    const Size wsz = wnd.size();
    const size_t height = draw_header_bg(wnd, opacity);
    const ssize_t margin = dp(18);

    // add folder button
    const ssize_t btn_y =
        static_cast<ssize_t>(height / 2) - dp(BUTTON_HEIGHT) / 2;
    const Rectangle rect =
        draw_button(wnd, "+ Add folder",
                    { static_cast<ssize_t>(wsz.width) - dp(14), btn_y }, true,
                    opacity);
    hits.push_back({ rect, [this] {
                        add_folders();
                    } });

    // title and number of albums
    const std::wstring title = L"Albums";
    const std::wstring count = to_wide(std::format(
        "{} {}", albums.size(), albums.size() == 1 ? "album" : "albums"));
    const ssize_t th = text_height(*font_title);
    const ssize_t sh = text_height(*font_small);
    const ssize_t title_y = static_cast<ssize_t>(height / 2) - th / 2;
    draw_text(wnd, *font_title, title, { margin, title_y },
              fade(CLR_TEXT, opacity));
    draw_text(wnd, *font_small, count,
              { margin + static_cast<ssize_t>(text_width(*font_title, title)) +
                    dp(12),
                title_y + th - sh },
              fade(CLR_TEXT_DIM, opacity));
}

size_t Library::draw_header_bg(Canvas& wnd, const double opacity)
{
    const Size wsz = wnd.size();
    const size_t height = dp(HEADER_HEIGHT);

    // translucent background with a soft bottom edge
    argb_t bkg = Gallery::self().get_window_color();
    bkg.a = 0xe8;
    wnd.fill_blend({ 0, 0, wsz.width, height }, fade(bkg, opacity));
    const size_t edge = dp(12);
    for (size_t i = 0; i < 4; ++i) {
        argb_t clr = bkg;
        clr.a = static_cast<argb_t::channel>(0x50 - i * 0x14);
        wnd.fill_blend({ 0, static_cast<ssize_t>(height + i * edge / 4),
                         wsz.width, edge / 4 },
                       fade(clr, opacity));
    }

    return height;
}

void Library::draw_header(Canvas& wnd, const double opacity)
{
    const Size wsz = wnd.size();
    const size_t height = draw_header_bg(wnd, opacity);

    // back icon
    const size_t icon = dp(28);
    if (chevron.width() != icon) {
        chevron.create(Pixmap::GS, icon, icon);
        const double thick = icon * 0.075;
        for (size_t y = 0; y < icon; ++y) {
            for (size_t x = 0; x < icon; ++x) {
                const double px = x + 0.5;
                const double py = y + 0.5;
                const double d = std::min(
                    segment_distance(px, py, icon * 0.62, icon * 0.2,
                                     icon * 0.32, icon * 0.5),
                    segment_distance(px, py, icon * 0.32, icon * 0.5,
                                     icon * 0.62, icon * 0.8));
                const double cov = std::clamp(thick + 0.5 - d, 0.0, 1.0);
                *static_cast<uint8_t*>(chevron.ptr(x, y)) =
                    static_cast<uint8_t>(cov * 255);
            }
        }
    }
    const ssize_t margin = dp(14);
    const ssize_t icon_y = static_cast<ssize_t>(height / 2 - icon / 2);
    wnd.mask(chevron, { margin, icon_y }, fade(CLR_TEXT, opacity));

    // buttons (right to left)
    const Album* saved = find(album);
    ssize_t right = static_cast<ssize_t>(wsz.width) - margin;
    const ssize_t btn_y =
        static_cast<ssize_t>(height / 2) - dp(BUTTON_HEIGHT) / 2;
    const std::filesystem::path path = album;
    if (!saved) {
        const Rectangle rect =
            draw_button(wnd, "+ Add album", { right, btn_y }, true, opacity);
        hits.push_back({ rect, [this, path] {
                            add_album(path);
                        } });
        right = rect.x - dp(10);
    } else {
        Rectangle rect =
            draw_button(wnd, "Remove album", { right, btn_y }, false, opacity);
        hits.push_back({ rect, [this, path] {
                            remove_album(path);
                        } });
        right = rect.x - dp(10);

        const bool on = saved->in_photos;
        rect = draw_button(wnd, on ? "Shown in Photos" : "Show in Photos",
                           { right, btn_y }, on, opacity);
        hits.push_back({ rect, [this, path, on] {
                            set_in_photos(path, !on);
                        } });
        right = rect.x - dp(10);
    }

    // include nested folders
    {
        const bool on = album_recursive(path);
        const Rectangle rect =
            draw_button(wnd, on ? "Including subfolders" : "Include subfolders",
                        { right, btn_y }, on, opacity);
        hits.push_back({ rect, [this, path, on] {
                            set_recursive(path, !on);
                        } });
        right = rect.x - dp(10);
    }

    // title and number of items
    const ssize_t title_x = margin + static_cast<ssize_t>(icon) + dp(10);
    std::string name = album.filename().string();
    if (name.empty()) {
        name = album.string();
    }
    const std::wstring count =
        to_wide(std::format("{} items", ImageList::self().size()));
    const ssize_t count_w = text_width(*font_small, count);
    const ssize_t avail = right - title_x - count_w - dp(24);
    const std::wstring title = fit_text(
        *font_title, to_wide(name), static_cast<size_t>(std::max<ssize_t>(avail, 0)));
    const ssize_t title_w = text_width(*font_title, title);
    const ssize_t th = text_height(*font_title);
    const ssize_t sh = text_height(*font_small);
    const ssize_t title_y = static_cast<ssize_t>(height / 2) - th / 2;
    draw_text(wnd, *font_title, title, { title_x, title_y },
              fade(CLR_TEXT, opacity));
    draw_text(wnd, *font_small, count,
              { title_x + title_w + dp(12), title_y + th - sh },
              fade(CLR_TEXT_DIM, opacity));

    // back area: icon and title
    hits.push_back({ { 0, 0, static_cast<size_t>(title_x + title_w + dp(8)),
                       height },
                     [this] {
                         back();
                     } });
}

Rectangle Library::draw_button(Canvas& wnd, const std::string& text,
                               const Point& right_top, const bool primary,
                               const double opacity)
{
    const std::wstring label = to_wide(text);
    const size_t height = dp(BUTTON_HEIGHT);
    const size_t width = text_width(*font_small, label) + dp(BUTTON_PAD) * 2;
    const Rectangle rect { right_top.x - static_cast<ssize_t>(width),
                           right_top.y, width, height };

    wnd.mask(pill({ width, height }), { rect.x, rect.y },
             fade(primary ? CLR_ACCENT : CLR_BUTTON, opacity));

    const ssize_t th = text_height(*font_small);
    draw_text(wnd, *font_small, label,
              { rect.x + dp(BUTTON_PAD),
                rect.y + static_cast<ssize_t>(height / 2) - th / 2 },
              fade(primary ? CLR_ACCENT_TEXT : CLR_TEXT, opacity));
    return rect;
}

void Library::draw_caption(Canvas& wnd, const ImageEntryPtr& entry,
                           const Rectangle& tile, const double opacity)
{
    const auto it = cover_album.find(entry->path);
    if (it == cover_album.end() || !font_title) {
        return;
    }
    const std::filesystem::path& dir = it->second;

    if (tile.height < static_cast<size_t>(dp(80)) ||
        tile.width < static_cast<size_t>(dp(80))) {
        return; // too small for text
    }

    // gradient at the bottom for text readability
    const size_t band = std::min<size_t>(tile.height / 2, dp(72));
    constexpr size_t steps = 12;
    for (size_t i = 0; i < steps; ++i) {
        const size_t y0 = band * i / steps;
        const size_t y1 = band * (i + 1) / steps;
        const argb_t clr { static_cast<argb_t::channel>(0xc0 * (i + 1) / steps),
                           0, 0, 0 };
        wnd.fill_blend({ tile.x,
                         tile.y + static_cast<ssize_t>(tile.height - band + y0),
                         tile.width, y1 - y0 },
                       fade(clr, opacity));
    }

    // album name and info
    const ssize_t pad = dp(10);
    const size_t max_w = tile.width > static_cast<size_t>(pad * 2)
        ? tile.width - pad * 2
        : 0;
    std::string name = dir.filename().string();
    if (name.empty()) {
        name = dir.string();
    }
    const std::wstring title = fit_text(*font_title, to_wide(name), max_w);

    std::string info;
    const auto info_it = album_info.find(dir);
    if (info_it != album_info.end()) {
        info = info_it->second.count
            ? std::format("{} items", info_it->second.count)
            : std::string("Empty");
    }
    const Album* saved = find(dir);
    if (saved && !saved->in_photos) {
        info += info.empty() ? "Hidden from Photos" : " · hidden from Photos";
    }
    const std::wstring sub = fit_text(*font_small, to_wide(info), max_w);

    const ssize_t th = text_height(*font_title);
    const ssize_t sh = text_height(*font_small);
    const ssize_t bottom = tile.y + static_cast<ssize_t>(tile.height) - pad;
    draw_text(wnd, *font_small, sub, { tile.x + pad, bottom - sh },
              fade(CLR_TEXT_DIM, opacity));
    draw_text(wnd, *font_title, title, { tile.x + pad, bottom - sh - th },
              fade(CLR_TEXT, opacity));
}

const Pixmap& Library::pill(const Size& size)
{
    return Shapes::pill(size);
}

void Library::toast(const std::string& message)
{
    constexpr std::chrono::milliseconds duration(1600);
    toast_text = to_wide(message);
    toast_until = std::chrono::steady_clock::now() + duration;
    toast_timer.reset(duration.count() + 20, 0);
    Application::redraw();
}

void Library::draw_toast(Canvas& wnd, const bool gallery)
{
    if (toast_alpha <= 0 || toast_text.empty()) {
        return;
    }
    if (Ui* ui = Application::get_ui()) {
        scale = ui->get_scale();
    }
    prepare_fonts();

    const Size wsz = wnd.size();
    const ssize_t tw = text_width(*font_small, toast_text);
    const ssize_t th = text_height(*font_small);
    const size_t height = dp(36);
    const size_t width = tw + dp(20) * 2;
    ssize_t bottom = static_cast<ssize_t>(wsz.height) - dp(28);
    if (gallery && view != View::None) {
        bottom = static_cast<ssize_t>(wsz.height) - dp(SWITCHER_BOTTOM) -
            dp(SWITCHER_HEIGHT) - dp(14);
    }
    const ssize_t slide = std::lround((1.0 - toast_alpha) * dp(8));
    const Point pos { static_cast<ssize_t>(wsz.width / 2) -
                          static_cast<ssize_t>(width / 2),
                      bottom - static_cast<ssize_t>(height) + slide };
    wnd.mask(pill({ width, height }), pos, fade(CLR_MENU, toast_alpha));
    draw_text(wnd, *font_small, toast_text,
              { pos.x + dp(20),
                pos.y + static_cast<ssize_t>(height / 2) - th / 2 },
              fade(CLR_TEXT, toast_alpha));
}

bool Library::animate(const double dt)
{
    bool in_progress = false;

    const double toast_target =
        std::chrono::steady_clock::now() < toast_until ? 1.0 : 0.0;
    if (toast_alpha != toast_target) {
        toast_alpha += (toast_target - toast_alpha) *
            (1.0 - std::exp(-dt * (toast_target > 0 ? 25.0 : 8.0)));
        if (std::abs(toast_target - toast_alpha) < 0.01) {
            toast_alpha = toast_target;
        }
        in_progress |= toast_alpha != toast_target;
    }

    if (view == View::None) {
        return in_progress;
    }

    const double menu_target = menu_open ? 1.0 : 0.0;
    if (menu_anim != menu_target) {
        menu_anim += (menu_target - menu_anim) *
            (1.0 - std::exp(-dt * MENU_SPEED));
        if (std::abs(menu_target - menu_anim) < 0.01) {
            menu_anim = menu_target;
        }
        in_progress = menu_anim != menu_target;
    }

    const double target = view == View::Photos ? 0.0 : 1.0;
    if (hl_pos == target) {
        return in_progress;
    }
    hl_pos += (target - hl_pos) * (1.0 - std::exp(-dt * HIGHLIGHT_SPEED));
    if (std::abs(target - hl_pos) < 0.002) {
        hl_pos = target;
    }
    return in_progress || hl_pos != target;
}

bool Library::handle_mbutton(const InputMouse::mouse_btn_t button,
                             const bool pressed, const Point& pos)
{
    if (view == View::None || button != InputMouse::BUTTON_LEFT) {
        return false;
    }

    if (!pressed) {
        const bool consumed = pressed_hit;
        pressed_hit = false;
        return consumed;
    }

    for (const HitArea& hit : hits) {
        const Rectangle& r = hit.rect;
        if (pos.x >= r.x && pos.x < r.x + static_cast<ssize_t>(r.width) &&
            pos.y >= r.y && pos.y < r.y + static_cast<ssize_t>(r.height)) {
            pressed_hit = true;
            const auto action = hit.action; // hits may be rebuilt
            action();
            Application::redraw();
            return true;
        }
    }

    // click outside the open menu closes it
    if (menu_open) {
        menu_open = false;
        pressed_hit = true;
        Application::redraw();
        return true;
    }

    // ignore clicks on header background
    if ((view == View::Album || view == View::Albums) &&
        pos.y < dp(HEADER_HEIGHT)) {
        pressed_hit = true;
        return true;
    }

    return false;
}

void Library::load_bad()
{
    bad_files.clear();
    std::ifstream file(bad_files_path());
    if (!file.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        // "<size> <mtime> <path>"
        const size_t sp1 = line.find(' ');
        const size_t sp2 =
            sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) {
            continue;
        }
        try {
            const uintmax_t size = std::stoull(line.substr(0, sp1));
            const int64_t mtime = std::stoll(line.substr(sp1 + 1, sp2 - sp1));
            bad_files.insert_or_assign(line.substr(sp2 + 1),
                                       std::make_pair(size, mtime));
        } catch (...) {
        }
    }
}

void Library::save_bad() const
{
    const std::filesystem::path path = bad_files_path();
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            return;
        }
        for (const auto& [file_path, stamp] : bad_files) {
            file << stamp.first << ' ' << stamp.second << ' '
                 << file_path.string() << '\n';
        }
    }
    std::filesystem::rename(tmp, path, ec);
}

bool Library::is_bad(const std::filesystem::path& path) const
{
    const auto it = bad_files.find(path);
    // file changed since it was found broken: check it again
    return it != bad_files.end() && it->second == file_stamp(path);
}

void Library::mark_bad(const std::vector<std::filesystem::path>& paths)
{
    bool changed = false;
    for (const auto& path : paths) {
        const auto stamp = file_stamp(path);
        if (stamp.first == 0 && stamp.second == 0) {
            continue; // file vanished
        }
        bad_files.insert_or_assign(path, stamp);
        changed = true;
    }
    if (changed) {
        save_bad();
    }
}

void Library::start_validation()
{
    const uint64_t gen = ++validate_gen;
    if (view != View::Photos && view != View::Album) {
        return;
    }

    // check files in the gallery order: visible part is checked first
    std::vector<std::filesystem::path> paths;
    for (const ImageEntryPtr& entry : ImageList::self().get_all()) {
        if (!entry->is_special()) {
            paths.push_back(entry->path);
        }
    }
    if (paths.empty()) {
        return;
    }

    std::thread([this, gen, paths = std::move(paths)] {
        std::vector<std::filesystem::path> bad;
        const auto flush = [&bad] {
            if (!bad.empty()) {
                Application::self().add_event(AppEvent::Callback {
                    [list = std::move(bad)] {
                        Library& lib = Library::self();
                        Log::verbose("{} files are not media, skipped",
                                     list.size());
                        lib.mark_bad(list);
                        Application::self().remove_images(list);
                    } });
                bad.clear();
            }
        };
        auto last_flush = std::chrono::steady_clock::now();
        for (const auto& path : paths) {
            if (validate_gen != gen) {
                break; // view changed
            }
            if (!sniff_media(path)) {
                bad.push_back(path);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush > std::chrono::milliseconds(300)) {
                flush();
                last_flush = now;
            }
        }
        flush();
    }).detach();
}

bool Library::handle_key(const InputKeyboard& key)
{
    if (menu_open && key.key == XKB_KEY_Escape) {
        menu_open = false;
        Application::redraw();
        return true;
    }
    if (view == View::Album && key.mods == KEYMOD_NONE &&
        (key.key == XKB_KEY_Escape || key.key == XKB_KEY_BackSpace)) {
        back();
        return true;
    }
    return false;
}
