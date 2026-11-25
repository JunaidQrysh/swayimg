// SPDX-License-Identifier: MIT
// Gallery mode.
// Copyright (C) 2024 Artem Senichev <artemsen@gmail.com>

#include "gallery.hpp"

#include "application.hpp"
#include "defaults.hpp"
#include "fdothumb.hpp"
#include "formatfactory.hpp"
#include "imagelist.hpp"
#include "log.hpp"
#include "render.hpp"
#include "resources.hpp"
#include "shapes.hpp"
#include "text.hpp"
#include "videoplayer.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <thread>
#include <utility>

// Limits for thumbnail size and other parameters
constexpr size_t THUMB_SIZE_MIN = 10;
constexpr size_t THUMB_SIZE_MAX = 10000;
constexpr size_t PADDING_SIZE_MAX = 1000;
constexpr size_t BORDER_SIZE_MAX = 100;
constexpr double SSCALE_MAX = 10.0;

/**
 * Get number of threads used for loading thumbnails: a screen of thumbnails
 * is decoded in parallel, one CPU is left for the main (rendering) thread.
 * @return number of threads
 */
static size_t thumb_load_threads()
{
    constexpr size_t min_threads = 4;
    const size_t cpus = std::thread::hardware_concurrency();
    return std::max(min_threads, cpus > 1 ? cpus - 1 : 1);
}

// Max time to wait for visible thumbnails before the first frame
constexpr std::chrono::milliseconds WARMUP_TIMEOUT(150);

// Animation durations in seconds
constexpr double FADE_TIME = 0.18;
constexpr double SELECT_TIME = 0.12;

// Touchpad scroll: pixels per scroll unit (wayland axis value / 16)
constexpr double FINGER_SCROLL_FACTOR = 16.0;
// Fling: min and max velocity (pixels per second)
constexpr double FLING_MIN_VELOCITY = 300.0;
constexpr double FLING_MAX_VELOCITY = 20000.0;
// Mouse wheel: scroll distance for one step relative to thumbnail size
constexpr double WHEEL_SCROLL_FACTOR = 0.6;

// Scroll bar: time to stay visible after scrolling, fade speed, sizes
constexpr std::chrono::milliseconds SCROLLBAR_LINGER(1000);
constexpr double SCROLLBAR_FADE_SPEED = 10.0;
constexpr double SCROLLBAR_HANDLE_W = 30;
constexpr double SCROLLBAR_HANDLE_H = 54;
constexpr double SCROLLBAR_ARROW_W = 10;
constexpr double SCROLLBAR_ARROW_H = 6;
constexpr double SCROLLBAR_ARROW_GAP = 5;
constexpr double SCROLLBAR_MARGIN = 6;
constexpr argb_t SCROLLBAR_CLR = { 0xf0, 0x30, 0x30, 0x33 };
constexpr argb_t SCROLLBAR_CLR_ACTIVE = { 0xf8, 0x48, 0x48, 0x4c };
constexpr argb_t SCROLLBAR_ARROW_CLR = { 0xff, 0xd8, 0xd8, 0xdc };

/** Ease out cubic. */
static double ease_out(const double t)
{
    const double inv = 1.0 - std::clamp(t, 0.0, 1.0);
    return 1.0 - inv * inv * inv;
}

// Service records in the thumbnail meta block
constexpr const char* THUMB_META_INODE = "swayimg.inode";
constexpr const char* THUMB_META_SIZE = "swayimg.size";

Gallery& Gallery::self()
{
    static Gallery singleton;
    return singleton;
}

Gallery::Gallery()
    : aspect(Defaults::gallery::aspect)
    , border_size(Defaults::gallery::border_size)
    , selected_scale(Defaults::gallery::selected_scale)
    , clr_window(Defaults::gallery::clr_window)
    , clr_background(Defaults::gallery::clr_background)
    , clr_select(Defaults::gallery::clr_select)
    , clr_border(Defaults::gallery::clr_border)
    , hover_select(Defaults::gallery::hover_select)
    , tpool(thumb_load_threads())
    , pstore_enable(Defaults::gallery::pstore_enable)
    , pstore_path(Defaults::gallery::pstore_path())
    , preload(Defaults::gallery::preload)
    , cache_size(Defaults::gallery::cache_size)
{
    mark_color = Defaults::gallery::mark_color;
    pinch_factor = Defaults::gallery::pinch_factor;

    layout.set_smooth(true);

    text_scheme[static_cast<size_t>(Text::TopLeft)].assign(
        Defaults::gallery::text_scheme_tl.begin(),
        Defaults::gallery::text_scheme_tl.end());
    text_scheme[static_cast<size_t>(Text::TopRight)].assign(
        Defaults::gallery::text_scheme_tr.begin(),
        Defaults::gallery::text_scheme_tr.end());

    Defaults::gallery::bind_inputs(this);
}

bool Gallery::select(const Layout::Direction dir)
{
    assert(is_active());

    if (!layout.get_selected() || !layout.select(dir)) {
        return false;
    }
    free_scroll = false;
    refresh();
    Application::redraw();
    switch_current();
    return true;
}

bool Gallery::select(const Point& pt)
{
    assert(is_active());

    if (!layout.select(pt)) {
        return false;
    }
    refresh();
    switch_current();
    return true;
}

void Gallery::reload()
{
    // stop loader
    tpool.cancel();
    tpool.wait();

    {
        // clear cache
        const std::scoped_lock lock(mutex);
        thumbs.clear();
    }

    refresh();
}

void Gallery::set_thumb_aspect(const Aspect ratio)
{
    aspect = ratio;
    if (is_active()) {
        reload();
        Application::redraw();
    }
}

void Gallery::set_thumb_size(const size_t size)
{
    layout.set_thumb_size(std::clamp(size, THUMB_SIZE_MIN, THUMB_SIZE_MAX));
    if (is_active()) {
        refresh();
        Application::redraw();
    }
}

void Gallery::set_padding_size(const size_t size)
{
    layout.set_padding(std::min(size, PADDING_SIZE_MAX));
    if (is_active()) {
        refresh();
        Application::redraw();
    }
}

void Gallery::set_border_size(const size_t size)
{
    border_size = std::min(size, BORDER_SIZE_MAX);
    if (is_active()) {
        Application::redraw();
    }
}

void Gallery::set_border_color(const argb_t& color)
{
    clr_border = color;
    if (is_active()) {
        Application::redraw();
    }
}

void Gallery::set_selected_scale(const double scale)
{
    selected_scale = std::min(scale, SSCALE_MAX);
    if (is_active()) {
        Application::redraw();
    }
}

void Gallery::set_selected_color(const argb_t& color)
{
    clr_select = color;
    if (is_active()) {
        Application::redraw();
    }
}

void Gallery::set_background_color(const argb_t& color)
{
    clr_background = color;
    if (is_active()) {
        Application::redraw();
    }
}

void Gallery::set_window_color(const argb_t& color)
{
    clr_window = color;
    if (is_active()) {
        Application::redraw();
    }
}

void Gallery::set_scroll_speed(const double speed)
{
    scroll_speed = std::clamp(speed, 0.1, 10.0);
}

void Gallery::enable_hover(const bool enable)
{
    hover_select = enable;
}

void Gallery::set_cache_size(const size_t size)
{
    cache_size = size;
    if (is_active()) {
        refresh();
    }
}

void Gallery::enable_preload(const bool enable)
{
    preload = enable;
    if (is_active()) {
        refresh();
    }
}

void Gallery::enable_pstore(const bool enable)
{
    pstore_enable = enable;
}

void Gallery::set_pstore_path(const std::filesystem::path& path)
{
    pstore_path = path;
}

void Gallery::set_margins(const size_t top, const size_t bottom)
{
    layout.set_margins(top, bottom);
    if (is_active()) {
        refresh();
        Application::redraw();
    }
}

void Gallery::drop_cache()
{
    tpool.cancel();
    tpool.wait();
    const std::scoped_lock lock(mutex);
    thumbs.clear();
    crld_thumbs.clear();
    fade.clear();
    sel_last = nullptr;
    sel_prev = nullptr;
    sel_anim = 1.0;
    layout.select(nullptr);
    layout.reset_scroll();
}

void Gallery::initialize()
{
    Application::self().add_fdpoll(warmup_timer, [this] {
        warmup_timer.reset(0, 0);
        Application::redraw();
    });
    Application::self().add_fdpoll(sbar.timer, [this] {
        sbar.timer.reset(0, 0);
        if (is_active()) {
            Application::redraw(); // start hiding the scroll bar
        }
    });
}

void Gallery::activate(const ImageEntryPtr& entry, const Size& wnd)
{
    AppMode::activate(entry, wnd);
    // keep scroll position if the image is visible (back from viewer)
    layout.select_keep_scroll(entry && !entry->removed ? entry : nullptr);
    layout.set_window_size(wnd);

    // postpone the first frame until visible thumbnails are loaded
    warmup = true;
    warmup_end = std::chrono::steady_clock::now() + WARMUP_TIMEOUT;
    warmup_timer.reset(WARMUP_TIMEOUT.count(), 0);
    {
        const std::scoped_lock lock(mutex);
        fade.clear();
    }

    refresh();
    switch_current();

    // no selection animation on activation
    sel_last = layout.get_selected();
    sel_prev = nullptr;
    sel_anim = 1.0;
    free_scroll = false;

    // scroll bar appears only on scrolling
    sbar.last_pos = layout.get_scroll();
    sbar.alpha = 0;
    sbar.drag = false;
    sbar.hover = false;
    sbar.show_until = {};
}

void Gallery::deactivate()
{
    tpool.cancel();
    tpool.wait();
    warmup = false;
    warmup_timer.reset(0, 0);
    layout.stop_scroll();
}

ImageEntryPtr Gallery::get_current()
{
    return layout.get_selected();
}

bool Gallery::set_current(const ImageEntryPtr& entry)
{
    layout.select(entry && !entry->removed ? entry : nullptr);
    refresh();
    switch_current();
    return true;
}

void Gallery::window_resize(const Size& wnd)
{
    layout.set_window_size(wnd);
    refresh();
}

void Gallery::window_redraw(Canvas& wnd)
{
    const ImageEntryPtr current = layout.get_selected();
    if (!current) {
        draw_empty(wnd, clr_window);
        return;
    }

    wnd.fill({ 0, 0, wnd.size().width, wnd.size().height }, clr_window);

    // draw all exclude the currently and previously selected (on top)
    const auto& scheme = layout.get_scheme();
    const Layout::Thumbnail* tsel = nullptr;
    const Layout::Thumbnail* tprev = nullptr;
    for (const auto& it : scheme) {
        if (it.img == current) {
            tsel = &it;
        } else if (it.img == sel_prev && sel_anim < 1.0) {
            tprev = &it;
        } else {
            draw(it, wnd);
        }
    }
    if (tprev) {
        draw(*tprev, wnd);
    }
    if (tsel) {
        draw(*tsel, wnd);
    }

    draw_scrollbar(wnd);
}

void Gallery::handle_mmove(const InputMouse&, const Point& pos, const Point&)
{
    if (sbar.drag) {
        drag_scrollbar(pos.y);
        return;
    }

    const bool hover = on_scrollbar(pos);
    if (hover != sbar.hover) {
        sbar.hover = hover;
        Application::redraw();
    }
    if (hover) {
        return;
    }

    if (hover_select && layout.select(pos)) {
        switch_current();
    }
}

void Gallery::handle_pinch(const double scale_delta, const double /*dx*/,
                           const double /*dy*/)
{
    const size_t size_factor =
        std::max(get_thumb_size(), static_cast<size_t>(100));
    const ssize_t size_delta = scale_delta * pinch_factor * size_factor;
    set_thumb_size(get_thumb_size() + size_delta);
}

void Gallery::handle_imagelist(const ImageListEvent event,
                               const std::vector<ImageEntryPtr>& entries)
{
    AppMode::handle_imagelist(event, entries);

    if (event == ImageListEvent::Modify || event == ImageListEvent::Remove) {
        // remove entry from cache
        const std::scoped_lock lock(mutex);
        for (const auto& entry : entries) {
            thumbs.erase(entry);
        }
    }

    if (event == ImageListEvent::Create) {
        if (!layout.get_selected()) {
            layout.select(entries.front());
            switch_current();
        }
    } else if (event == ImageListEvent::Remove) {
        const ImageEntryPtr entry = layout.get_selected();
        if (entry && entry->removed) {
            if (!layout.select(Layout::Right) && !layout.select(Layout::Left)) {
                layout.select(nullptr);
            }
            switch_current();
        }
    }

    layout.update();
    refresh();
    Application::redraw();
}

Rectangle Gallery::tile_rect(const Layout::Thumbnail& tlay,
                             const Size& wnd_size) const
{
    const bool selected = (tlay.img == layout.get_selected());
    const size_t tile_size = layout.get_thumb_size();

    // animated scale of the selected and previously selected tiles
    double tile_scale = 1.0;
    if (selected) {
        tile_scale += (selected_scale - 1.0) *
            (tlay.img == sel_last ? ease_out(sel_anim) : 1.0);
    } else if (tlay.img == sel_prev && sel_anim < 1.0) {
        tile_scale += (selected_scale - 1.0) * ease_out(1.0 - sel_anim);
    }

    // calculate tile position/size
    Rectangle tile {
        tlay.pos, { .width = tile_size, .height = tile_size }
    };
    if (tile_scale != 1.0) {
        tile.width *= tile_scale;
        tile.height *= tile_scale;
        tile.x -= tile.width / 2 - tile_size / 2;
        tile.y -= tile.height / 2 - tile_size / 2;

        // prevent going beyond the window
        if (tile.x + tile.width + border_size > wnd_size.width) {
            tile.x = wnd_size.width - tile.width - border_size;
        }
        if (std::cmp_less(tile.x, border_size)) {
            tile.x = border_size;
        }
        if (tile.y + tile.height + border_size > wnd_size.height) {
            tile.y = wnd_size.height - tile.height - border_size;
        }
        if (std::cmp_less(tile.y, border_size)) {
            tile.y = border_size;
        }
    }

    return tile;
}

Rectangle Gallery::get_tile_rect(const ImageEntryPtr& entry,
                                 const Size& wnd_size) const
{
    for (const auto& it : layout.get_scheme()) {
        if (it.img == entry) {
            return tile_rect(it, wnd_size);
        }
    }
    return {};
}

void Gallery::draw(const Layout::Thumbnail& tlay, Canvas& wnd)
{
    const std::scoped_lock lock(mutex);

    const bool selected = (tlay.img == layout.get_selected());
    const Pixmap* pm = get_thumbnail(tlay.img);

    // fade-in of newly loaded thumbnail
    double opacity = 1.0;
    const auto fade_it = fade.find(tlay.img);
    if (fade_it != fade.end()) {
        opacity = ease_out(fade_it->second);
    }

    const Rectangle tile = tile_rect(tlay, wnd.size());

    // draw background
    Rectangle bkg = tile;
    if (aspect == Aspect::Keep && pm && pm->width() != pm->height()) {
        const double ratio = static_cast<double>(pm->width()) / pm->height();
        if (ratio > 1.0) {
            bkg.height /= ratio;
        } else {
            bkg.width *= ratio;
        }
        bkg.x += tile.width / 2 - bkg.width / 2;
        bkg.y += tile.height / 2 - bkg.height / 2;
    }
    wnd.fill(bkg, selected ? clr_select : clr_background);

    if (pm) {
        // draw thumbnail
        const double scale_w = static_cast<double>(tile.width) / pm->width();
        const double scale_h = static_cast<double>(tile.height) / pm->height();
        const double scale = aspect == Aspect::Fill
            ? std::max(scale_w, scale_h)
            : std::min(scale_w, scale_h);

        const Point pos {
            .x = static_cast<ssize_t>(tile.width / 2) -
                static_cast<ssize_t>(scale * pm->width()) / 2,
            .y = static_cast<ssize_t>(tile.height / 2) -
                static_cast<ssize_t>(scale * pm->height()) / 2,
        };

        wnd.draw(*pm, { .x = tile.x + pos.x, .y = tile.y + pos.y }, scale, tile,
                 opacity);
    } else if (bkg.width > Resource::file.width() &&
               bkg.height > Resource::file.height()) {
        // draw icon
        const ssize_t x = bkg.x + static_cast<ssize_t>(bkg.width / 2) -
            static_cast<ssize_t>(Resource::file.width() / 2);
        const ssize_t y = bkg.y + static_cast<ssize_t>(bkg.height / 2) -
            static_cast<ssize_t>(Resource::file.height() / 2);
        wnd.mask(Resource::file, { .x = x, .y = y },
                 { 0x20, 0xff, 0xff, 0xff });
    }

    // draw video label
    if (VideoPlayer::is_video(tlay.img) &&
        bkg.width > Resource::video.width() * 2 &&
        bkg.height > Resource::video.height() * 2) {
        const ssize_t margin = 5;
        wnd.mask(Resource::video, { .x = bkg.x + margin, .y = bkg.y + margin },
                 { 0xa0, argb_t::max, argb_t::max, argb_t::max });
    }

    // draw border
    if (selected && clr_border.a && border_size) {
        Rectangle border = bkg;
        border.x -= border_size;
        border.y -= border_size;
        border.width += border_size * 2;
        border.height += border_size * 2;
        wnd.rectangle(border, clr_border, border_size);
    }

    if (tile_overlay) {
        tile_overlay(wnd, tlay.img, bkg, opacity);
    }

    // draw mark icon
    if (tlay.img->mark) {
        const ssize_t margin = 5;
        const ssize_t x = bkg.x + static_cast<ssize_t>(bkg.width) -
            static_cast<ssize_t>(Resource::mark.width()) - margin;
        const ssize_t y = bkg.y + static_cast<ssize_t>(bkg.height) -
            static_cast<ssize_t>(Resource::mark.height()) - margin;
        wnd.mask(Resource::mark, { .x = x, .y = y }, mark_color);
    }
}

void Gallery::refresh()
{
    const std::scoped_lock lock(mutex);
    update_visible();
    clear_invisible();
    requeue_loading();
}

void Gallery::update_visible()
{
    const auto& scheme = layout.get_scheme();
    if (scheme.empty()) {
        visible_first = 1;
        visible_last = 0;
    } else {
        visible_first = scheme.front().img->index;
        visible_last = scheme.back().img->index;
    }
}

void Gallery::on_scrolled()
{
    const auto& scheme = layout.get_scheme();
    const size_t first = scheme.empty() ? 1 : scheme.front().img->index;
    const size_t last = scheme.empty() ? 0 : scheme.back().img->index;
    if (first != visible_first || last != visible_last) {
        refresh();
    }
}

bool Gallery::ready_to_draw()
{
    if (!warmup) {
        return true;
    }

    bool ready = std::chrono::steady_clock::now() >= warmup_end;
    if (!ready) {
        // list is loading in background: wait for the first entries
        ready = !layout.get_scheme().empty() || !ImageList::self().scanning();
        const std::scoped_lock lock(mutex);
        for (const auto& it : layout.get_scheme()) {
            if (!thumbs.contains(it.img)) {
                ready = false;
                break;
            }
        }
    }

    if (ready) {
        warmup = false;
        warmup_timer.reset(0, 0);
    }
    return ready;
}

bool Gallery::animate(const double dt)
{
    bool in_progress = false;

    // scrolling
    if (layout.is_animating()) {
        in_progress = layout.animate(dt);
        if (free_scroll && layout.keep_selection_visible()) {
            follow_selection();
        }
        on_scrolled();
    }

    // scroll bar: show while scrolling, hover or drag, then fade out
    const double pos = layout.get_scroll();
    const auto now = std::chrono::steady_clock::now();
    if (pos != sbar.last_pos) {
        sbar.last_pos = pos;
        sbar.show_until = now + SCROLLBAR_LINGER;
        sbar.timer.reset(SCROLLBAR_LINGER.count() + 20, 0);
    }
    const bool show = layout.get_max_scroll() > 0 &&
        (sbar.drag || sbar.hover || now < sbar.show_until);
    const double sb_target = show ? 1.0 : 0.0;
    if (sbar.alpha != sb_target) {
        sbar.alpha +=
            (sb_target - sbar.alpha) * (1.0 - std::exp(-dt * SCROLLBAR_FADE_SPEED));
        if (std::abs(sb_target - sbar.alpha) < 0.01) {
            sbar.alpha = sb_target;
        }
        in_progress |= sbar.alpha != sb_target;
    }

    // selection
    if (sel_anim < 1.0) {
        sel_anim = std::min(1.0, sel_anim + dt / SELECT_TIME);
        in_progress |= sel_anim < 1.0;
    }

    // fade-in
    const std::scoped_lock lock(mutex);
    for (auto it = fade.begin(); it != fade.end();) {
        it->second += dt / FADE_TIME;
        if (it->second >= 1.0) {
            it = fade.erase(it);
        } else {
            in_progress = true;
            ++it;
        }
    }

    return in_progress;
}

void Gallery::scroll(const double delta)
{
    if (!layout.get_selected()) {
        return;
    }

    free_scroll = true;

    if (scroll_finger) {
        // touchpad: follow the finger, track velocity for kinetic scrolling
        // touchpad deltas are in logical pixels, layout uses buffer pixels
        const double distance = delta * FINGER_SCROLL_FACTOR * scroll_speed *
            Application::get_ui()->get_scale();

        // new gesture: remember velocity of the running fling (stacking)
        if (fling_tracker.empty()) {
            fling_carry = layout.get_velocity();
        }

        layout.scroll_by(distance, true);

        // keep recent samples (compositor timestamps) for fling velocity
        fling_tracker.add(scroll_time, 0, distance);

        if (layout.keep_selection_visible()) {
            follow_selection();
        }
        on_scrolled();
    } else {
        // mouse wheel: animated scroll
        layout.scroll_by(delta * layout.get_thumb_size() * WHEEL_SCROLL_FACTOR,
                         false);
    }

    Application::redraw();
}

void Gallery::handle_scroll_stop()
{
    double velocity = fling_tracker.release(scroll_time).second;

    // repeated flicks in the same direction accelerate scrolling
    if (fling_carry != 0 && (fling_carry > 0) == (velocity > 0) &&
        std::abs(velocity) >= FLING_MIN_VELOCITY) {
        velocity += fling_carry;
    }
    fling_carry = 0;

    if (std::abs(velocity) >= FLING_MIN_VELOCITY) {
        velocity = std::clamp(velocity, -FLING_MAX_VELOCITY,
                              FLING_MAX_VELOCITY);
        layout.fling(velocity);
        Application::redraw();
    }
}

void Gallery::draw_scrollbar(Canvas& wnd)
{
    const double max = layout.get_max_scroll();
    sbar.track = {};
    sbar.thumb = {};
    if (max <= 0) {
        return;
    }

    const double scale = Application::get_ui()->get_scale();
    const auto dp = [scale](const double px) {
        return static_cast<ssize_t>(std::lround(px * scale));
    };

    // fixed size handle moving along the right edge
    const Size wsz = wnd.size();
    const ssize_t margin = dp(SCROLLBAR_MARGIN);
    const size_t width = dp(SCROLLBAR_HANDLE_W);
    const size_t height = dp(SCROLLBAR_HANDLE_H);
    const ssize_t top = static_cast<ssize_t>(layout.get_margin_top()) + margin;
    const ssize_t bottom = static_cast<ssize_t>(wsz.height) -
        static_cast<ssize_t>(layout.get_margin_bottom()) - margin;
    if (bottom - top < static_cast<ssize_t>(height) * 2) {
        return; // window too small
    }
    const size_t track_len = bottom - top;

    const double frac = std::clamp(layout.get_scroll() / max, 0.0, 1.0);
    const ssize_t y =
        top + static_cast<ssize_t>(std::lround((track_len - height) * frac));
    const ssize_t x = static_cast<ssize_t>(wsz.width) - margin -
        static_cast<ssize_t>(width);

    // hover area: strip along the right edge
    sbar.track = { x - margin, top, width + margin * 2, track_len };
    sbar.thumb = { x, y, width, height };

    if (sbar.alpha <= 0) {
        return;
    }

    const auto fade = [this](argb_t clr) {
        clr.a = static_cast<argb_t::channel>(clr.a * sbar.alpha);
        return clr;
    };
    const bool active = sbar.hover || sbar.drag;
    wnd.mask(Shapes::pill({ width, height }), { x, y },
             fade(active ? SCROLLBAR_CLR_ACTIVE : SCROLLBAR_CLR));

    // up/down arrows
    const Size arrow { static_cast<size_t>(dp(SCROLLBAR_ARROW_W)),
                       static_cast<size_t>(dp(SCROLLBAR_ARROW_H)) };
    const ssize_t gap = dp(SCROLLBAR_ARROW_GAP) / 2;
    const ssize_t ax = x + static_cast<ssize_t>(width / 2) -
        static_cast<ssize_t>(arrow.width / 2);
    const ssize_t cy = y + static_cast<ssize_t>(height / 2);
    wnd.mask(Shapes::triangle(arrow, true),
             { ax, cy - gap - static_cast<ssize_t>(arrow.height) },
             fade(SCROLLBAR_ARROW_CLR));
    wnd.mask(Shapes::triangle(arrow, false), { ax, cy + gap },
             fade(SCROLLBAR_ARROW_CLR));
}

bool Gallery::on_scrollbar(const Point& pos) const
{
    const Rectangle& r = sbar.track;
    return r.width && pos.x >= r.x &&
        pos.x < r.x + static_cast<ssize_t>(r.width) && pos.y >= r.y &&
        pos.y < r.y + static_cast<ssize_t>(r.height);
}

void Gallery::drag_scrollbar(const ssize_t y)
{
    const Rectangle& track = sbar.track;
    const double max = layout.get_max_scroll();
    if (!track.height || max <= 0 || track.height <= sbar.thumb.height) {
        return;
    }
    const double range = track.height - sbar.thumb.height;
    const double frac =
        std::clamp((y - sbar.drag_offset - track.y) / range, 0.0, 1.0);

    free_scroll = true;
    layout.set_scroll(frac * max);
    if (layout.keep_selection_visible()) {
        follow_selection();
    }
    on_scrolled();
    Application::redraw();
}

bool Gallery::handle_mbutton(const InputMouse::mouse_btn_t button,
                             const bool pressed, const Point& pos)
{
    if (button != InputMouse::BUTTON_LEFT) {
        return false;
    }
    if (!pressed) {
        if (sbar.drag) {
            sbar.drag = false;
            sbar.hover = on_scrollbar(pos);
            sbar.show_until =
                std::chrono::steady_clock::now() + SCROLLBAR_LINGER;
            sbar.timer.reset(SCROLLBAR_LINGER.count() + 20, 0);
            Application::redraw();
            return true;
        }
        return false;
    }
    if (!on_scrollbar(pos)) {
        return false;
    }

    layout.stop_scroll();
    sbar.drag = true;
    const Rectangle& thumb = sbar.thumb;
    if (pos.y >= thumb.y &&
        pos.y < thumb.y + static_cast<ssize_t>(thumb.height)) {
        sbar.drag_offset = pos.y - thumb.y; // grab the handle
    } else {
        sbar.drag_offset = thumb.height / 2; // jump to the position
        drag_scrollbar(pos.y);
    }
    Application::redraw();
    return true;
}

void Gallery::follow_selection()
{
    AppMode::switch_current();
    sel_last = layout.get_selected();
    sel_prev = nullptr;
    sel_anim = 1.0;
}

void Gallery::switch_current()
{
    const ImageEntryPtr current = layout.get_selected();
    if (current != sel_last) {
        sel_prev = sel_last;
        sel_last = current;
        sel_anim = 0.0;
    }
    AppMode::switch_current();
}

void Gallery::requeue_loading()
{
    tpool.cancel();

    const std::vector<Layout::Thumbnail>& scheme = layout.get_scheme();
    if (scheme.empty()) {
        return;
    }

    const size_t index_first = scheme.front().img->index;
    const size_t index_last = scheme.back().img->index;

    size_t preload_fwd = preload ? cache_size / 2 : 0;
    size_t preload_back = preload_fwd;

    ImageList& il = ImageList::self();
    ImageEntryPtr fwd = layout.get_selected();
    ImageEntryPtr back = il.get(fwd, ImageList::Dir::Prev);

    // put thumbnail to loading queue, returns next thumb to load
    const auto queue_thumbnail = [&](const ImageEntryPtr& entry,
                                     const ImageList::Dir dir,
                                     size_t& preload_left) -> ImageEntryPtr {
        if (!get_thumbnail(entry) && !crld_thumbs.contains(entry)) {
            tpool.add([this, entry] {
                load_thumbnail(entry);
            });
        }
        ImageEntryPtr next = il.get(entry, dir);
        if (next) {
            const bool beyond_window =
                (dir == ImageList::Dir::Next && next->index > index_last) ||
                (dir == ImageList::Dir::Prev && next->index < index_first);
            if (beyond_window) {
                if (preload_left == 0) {
                    next = nullptr; // preload budget exhausted
                } else {
                    --preload_left;
                }
            }
        }
        return next;
    };

    while (fwd || back) {
        if (fwd) {
            fwd = queue_thumbnail(fwd, ImageList::Dir::Next, preload_fwd);
        }
        if (back) {
            back = queue_thumbnail(back, ImageList::Dir::Prev, preload_back);
        }
    }
}

void Gallery::clear_invisible()
{
    const std::vector<Layout::Thumbnail>& scheme = layout.get_scheme();
    if (scheme.empty()) {
        thumbs.clear();
        return;
    }

    if (thumbs.size() > scheme.size() + cache_size) {
        const size_t visible_first = scheme.front().img->index;
        const size_t visible_last = scheme.back().img->index;
        const size_t store_min =
            visible_first > cache_size / 2 ? visible_first - cache_size / 2 : 1;
        const size_t store_max = visible_last + cache_size - cache_size / 2;

        std::erase_if(thumbs,
                      [store_min, store_max](
                          const std::pair<ImageEntryPtr, Pixmap>& key_value) {
                          return key_value.first->index < store_min ||
                              key_value.first->index > store_max;
                      });
    }
}

const Pixmap* Gallery::get_thumbnail(const ImageEntryPtr& entry)
{
    const auto it = thumbs.find(entry);
    return it == thumbs.end() ? nullptr : &it->second;
}

void Gallery::load_thumbnail(const ImageEntryPtr& entry)
{
    {
        const std::scoped_lock lock(mutex);
        crld_thumbs.insert(entry);
    }

    // directory placeholder (empty album): nothing to load
    std::error_code dir_ec;
    if (!entry->is_special() &&
        std::filesystem::is_directory(entry->path, dir_ec)) {
        const std::scoped_lock lock(mutex);
        crld_thumbs.erase(entry);
        return;
    }

    const size_t thumb_size = layout.get_thumb_size();

    Pixmap pm;

    // try freedesktop thumbnail cache (x-large: up to 1024px)
    if (!entry->is_special() && thumb_size <= 1024) {
        const Pixmap fdo = FdoThumb::load(entry->path);
        if (fdo) {
            pm = ImageFormat::make_thumb(fdo, thumb_size,
                                         aspect == Aspect::Fill);
        }
    }

    if (!pm && pstore_enable && !entry->is_special()) {
        pm = pstore_load(entry);
    }

    if (!pm) {
        pm = FormatFactory::self().preview(entry, thumb_size,
                                           aspect == Aspect::Fill);
        if (!pm) {
            Log::verbose("Unable to load {}, removed from the list",
                         entry->path.string());
            if (on_load_failed) {
                on_load_failed(entry->path);
            }
            Application::self().add_event(AppEvent::FileRemove { entry->path });
        } else if (pstore_enable && !entry->is_special()) {
            pstore_save(entry, pm);
        }
    }

    const std::scoped_lock lock(mutex);
    crld_thumbs.erase(entry);
    if (pm) {
        thumbs.insert_or_assign(entry, pm);
        const size_t index = entry->index;
        if (index >= visible_first && index <= visible_last) {
            if (!warmup) {
                fade.insert_or_assign(entry, 0.0); // animate appearance
            }
            Application::redraw();
        }
    }
}

Pixmap Gallery::pstore_load(const ImageEntryPtr& entry) const
{
    std::filesystem::path thumb_path = pstore_path;
    thumb_path.concat(entry->path.string());

    // use non-throwing functions, this runs in a thread pool
    std::error_code ec;
    if (!std::filesystem::exists(thumb_path, ec)) {
        return {};
    }

    // get file stats
    struct stat st_image;
    struct stat st_thumb;
    if (stat(entry->path.c_str(), &st_image) == -1 ||
        stat(thumb_path.c_str(), &st_thumb) == -1) {
        return {};
    }

    // check if origin image is newer then thumbnail
    if (st_image.st_mtim.tv_sec > st_thumb.st_mtim.tv_sec) {
        return {};
    }

    // load thumbnail
    const ImageEntryPtr thumb_entry = std::make_shared<ImageEntry>();
    thumb_entry->path = thumb_path;
    // own thumbnails keep their info in PNG text fields, no EXIF
    const ImagePtr thumb_image = FormatFactory::self().load(thumb_entry, false);
    if (!thumb_image) {
        return {};
    }

    // check if size or inode of origin file were changed
    size_t tlink_inode = 0;
    const auto it_inode = thumb_image->meta.find(THUMB_META_INODE);
    if (it_inode != thumb_image->meta.end()) {
        tlink_inode = std::strtoul(it_inode->second.c_str(), nullptr, 10);
    }
    size_t tlink_size = 0;
    const auto it_size = thumb_image->meta.find(THUMB_META_SIZE);
    if (it_size != thumb_image->meta.end()) {
        tlink_size = std::strtoul(it_size->second.c_str(), nullptr, 10);
    }
    if (tlink_inode != st_image.st_ino ||
        std::cmp_not_equal(tlink_size, st_image.st_size)) {
        // remove thumbnail file
        std::filesystem::remove(thumb_path, ec);
        return {};
    }

    return thumb_image->frames[0].pm;
}

void Gallery::pstore_save(const ImageEntryPtr& entry, const Pixmap& thumb) const
{
    // fill meta info
    std::unordered_map<std::string, std::string> meta;
    struct stat st {};
    stat(entry->path.c_str(), &st);
    meta.insert_or_assign(THUMB_META_INODE, std::format("{}", st.st_ino));
    meta.insert_or_assign(THUMB_META_SIZE, std::format("{}", st.st_size));

    // save thumbnail
    std::filesystem::path thumb_path = pstore_path;
    thumb_path.concat(entry->path.string());
    // use non-throwing functions, this runs in a thread pool
    std::error_code ec;
    std::filesystem::create_directories(thumb_path.parent_path(), ec);
    if (ec) {
        return; // no storage, no thumbnail
    }
    FormatFactory::save(thumb, meta, thumb_path);
}
