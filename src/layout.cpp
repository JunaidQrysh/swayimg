// SPDX-License-Identifier: MIT
// Thumbnail layout for gallery mode.
// Copyright (C) 2025 Artem Senichev <artemsen@gmail.com>

#include "layout.hpp"

#include "defaults.hpp"
#include "imagelist.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace {

// Smooth scrolling speed: higher value means faster approach to the target
constexpr double SMOOTH_SPEED = 16.0;
// Kinetic scrolling friction: velocity decay rate per second
constexpr double FLING_FRICTION = 3.5;
// Kinetic scrolling stops below this velocity (pixels per second)
constexpr double FLING_MIN_VELOCITY = 15.0;

} // namespace

Layout::Layout()
    : thumb_size(Defaults::gallery::thumb_size)
    , thumb_padding(Defaults::gallery::thumb_padding)
{
}

void Layout::update()
{
    recalc();
}

void Layout::set_window_size(const Size& size)
{
    const bool anchored = window && sel_entry && !sel_entry->removed && total;
    const double anchor = anchored ? row_top(selected_row()) - scroll : 0;

    window = size;
    recalc();

    if (sel_entry) {
        if (anchored) {
            // keep the selected thumbnail at the same place
            velocity = 0;
            target = std::clamp(row_top(selected_row()) - anchor, 0.0,
                                max_scroll());
            scroll = target;
            rebuild();
        } else {
            center_selection();
        }
    }
}

void Layout::set_thumb_size(const size_t size)
{
    const bool anchored = window && sel_entry && !sel_entry->removed && total;
    const double anchor = anchored ? row_top(selected_row()) - scroll : 0;

    thumb_size = size;
    recalc();

    if (anchored && sel_entry) {
        velocity = 0;
        target =
            std::clamp(row_top(selected_row()) - anchor, 0.0, max_scroll());
        scroll = target;
        rebuild();
    }
}

void Layout::set_padding(const size_t padding)
{
    const bool anchored = window && sel_entry && !sel_entry->removed && total;
    const double anchor = anchored ? row_top(selected_row()) - scroll : 0;

    thumb_padding = padding;
    recalc();

    if (anchored && sel_entry) {
        velocity = 0;
        target =
            std::clamp(row_top(selected_row()) - anchor, 0.0, max_scroll());
        scroll = target;
        rebuild();
    }
}

void Layout::select(const ImageEntryPtr& image)
{
    sel_entry = image;
    recalc();
    if (sel_entry) {
        center_selection();
    }
}

void Layout::select_keep_scroll(const ImageEntryPtr& image)
{
    sel_entry = image;
    recalc();
    if (!sel_entry || !total) {
        return;
    }
    if (!window) {
        center_selection(); // not shown yet
        return;
    }

    const double top = row_top(selected_row()) - thumb_padding;
    const double bottom = row_top(selected_row()) + thumb_size;
    const double view = std::clamp(scroll, 0.0, max_scroll());
    double new_scroll = view;
    if (bottom <= view || top >= view + window.height) {
        center_selection(); // out of view
        return;
    }
    if (top < view) {
        new_scroll = top; // partially visible: scroll as little as possible
    } else if (bottom > view + window.height) {
        new_scroll = bottom - window.height;
    }

    velocity = 0;
    target = std::clamp(new_scroll, 0.0, max_scroll());
    scroll = target;
    rebuild();
}

bool Layout::select(const Direction dir)
{
    assert(sel_entry);

    ImageList& il = ImageList::self();
    ImageEntryPtr next = nullptr;
    double page = 0;

    switch (dir) {
        case Direction::First:
            next = il.get(nullptr, ImageList::Dir::First);
            break;
        case Direction::Last:
            next = il.get(nullptr, ImageList::Dir::Last);
            break;
        case Direction::Up:
            next = il.get(sel_entry, -static_cast<ssize_t>(columns));
            if (!next) {
                next = il.get(nullptr, ImageList::Dir::First);
            }
            break;
        case Direction::Down:
            next = il.get(sel_entry, columns);
            if (!next) {
                next = il.get(nullptr, ImageList::Dir::Last);
            }
            break;
        case Direction::Left:
            next = il.get(sel_entry, ImageList::Dir::Prev);
            break;
        case Direction::Right:
            next = il.get(sel_entry, ImageList::Dir::Next);
            break;
        case Direction::PgUp:
            next = il.get(sel_entry, -static_cast<ssize_t>(columns * rows));
            if (!next) {
                next = il.get(nullptr, ImageList::Dir::First);
            }
            page = -static_cast<double>(rows * (thumb_size + thumb_padding));
            break;
        case Direction::PgDown:
            next = il.get(sel_entry, columns * rows);
            if (!next) {
                next = il.get(nullptr, ImageList::Dir::Last);
            }
            page = static_cast<double>(rows * (thumb_size + thumb_padding));
            break;
    }

    if (next == sel_entry) {
        next = nullptr;
    }

    if (next) {
        sel_entry = next;
        velocity = 0;
        if (page != 0) {
            // keep selection at the same place on the screen
            scroll_to(target + page);
        }
        ensure_visible();
    }

    return !!next;
}

bool Layout::select(const Point& pos)
{
    const Thumbnail* thumb = at(pos);
    const bool new_select = thumb && thumb->img != sel_entry;

    if (new_select) {
        sel_entry = thumb->img;
    }

    return new_select;
}

const Layout::Thumbnail* Layout::at(const Point& pos) const
{
    for (const auto& it : scheme) {
        if (pos.x >= it.pos.x &&
            pos.x < it.pos.x + static_cast<ssize_t>(thumb_size) &&
            pos.y >= it.pos.y &&
            pos.y < it.pos.y + static_cast<ssize_t>(thumb_size)) {
            return &it;
        }
    }
    return nullptr;
}

ImageEntryPtr Layout::get_selected() const
{
    return sel_entry;
}

bool Layout::is_visible(const ImageEntryPtr& entry) const
{
    assert(entry);

    if (scheme.empty()) {
        return false;
    }

    const ImageEntryPtr& first = scheme.front().img;
    const ImageEntryPtr& last = scheme.back().img;

    // skip check if scheme entries have been removed since last update
    if (!first || first->removed || !last || last->removed || entry->removed) {
        return false;
    }

    ImageList& il = ImageList::self();
    return il.distance(entry, first) <= 0 && il.distance(entry, last) >= 0;
}

const std::vector<Layout::Thumbnail>& Layout::get_scheme() const
{
    return scheme;
}

void Layout::set_smooth(const bool enable)
{
    smooth = enable;
    if (!smooth) {
        stop_scroll();
    }
}

void Layout::scroll_by(const double distance, const bool immediate)
{
    velocity = 0;
    target = std::clamp(target + distance, 0.0, max_scroll());
    if (immediate || !smooth) {
        scroll = target;
        rebuild();
    }
}

void Layout::fling(const double speed)
{
    if (smooth) {
        scroll = target;
        velocity = speed;
    }
}

void Layout::stop_scroll()
{
    velocity = 0;
    if (target != scroll) {
        target = scroll;
    }
}

bool Layout::animate(const double dt)
{
    if (!smooth) {
        return false;
    }

    const double prev = scroll;

    if (velocity != 0) {
        // kinetic scrolling: scroll position and target move together
        double next = scroll + velocity * dt;
        velocity *= std::exp(-dt * FLING_FRICTION);
        const double max = max_scroll();
        if (next <= 0 || next >= max) {
            next = std::clamp(next, 0.0, max);
            velocity = 0;
        }
        if (std::abs(velocity) < FLING_MIN_VELOCITY) {
            velocity = 0;
        }
        scroll = next;
        target = next;
    } else if (scroll != target) {
        // smooth scrolling: exponential approach to the target
        scroll += (target - scroll) * (1.0 - std::exp(-dt * SMOOTH_SPEED));
        if (std::abs(target - scroll) < 0.5) {
            scroll = target;
        }
    }

    if (scroll != prev) {
        rebuild();
    }

    return is_animating();
}

bool Layout::is_animating() const
{
    return velocity != 0 || scroll != target;
}

bool Layout::keep_selection_visible()
{
    if (!sel_entry || sel_entry->removed || !total || !window) {
        return false;
    }

    const double cell = thumb_size + thumb_padding;
    const size_t row = selected_row();
    const double top = row_top(row) - scroll;
    if (top >= 0 && top + thumb_size <= window.height) {
        return false; // fully visible
    }

    // range of fully visible rows
    const double base = scroll - offset_y - thumb_padding;
    ssize_t first = static_cast<ssize_t>(std::ceil(base / cell));
    ssize_t last = static_cast<ssize_t>(
        std::floor((base + window.height - thumb_size) / cell));
    first = std::clamp<ssize_t>(first, 0, total_rows - 1);
    last = std::clamp<ssize_t>(last, 0, total_rows - 1);

    size_t new_row;
    if (first > last) {
        // thumbnail is larger than window: use the central row
        new_row = std::clamp<ssize_t>(
            std::floor((scroll + window.height / 2.0 - offset_y) / cell), 0,
            total_rows - 1);
    } else {
        new_row = std::cmp_less(row, first) ? first : last;
    }
    if (new_row == row) {
        return false;
    }

    const size_t col = sel_entry->index % columns;
    const size_t index = std::min(new_row * columns + col, total - 1);
    ImageList& il = ImageList::self();
    const ImageEntryPtr first_entry = il.get(nullptr, ImageList::Dir::First);
    const ImageEntryPtr entry = first_entry ? il.get(first_entry, index) : nullptr;
    if (!entry || entry == sel_entry) {
        return false;
    }
    sel_entry = entry;
    return true;
}

void Layout::recalc()
{
    if (!window) {
        return; // not initialized
    }

    ImageList& il = ImageList::self();
    total = il.size();
    if (!total) {
        sel_entry = nullptr;
        scheme.clear();
        return;
    }
    if (!sel_entry || sel_entry->removed) {
        sel_entry = il.get(nullptr, ImageList::Dir::First);
    }

    const size_t cell = thumb_size + thumb_padding;
    columns = std::max(static_cast<size_t>(1), window.width / cell);
    rows = std::max(static_cast<size_t>(1), window.height / cell);
    total_rows = (total + columns - 1) / columns;

    const size_t used_cols = std::min(columns, total);
    offset_x = (window.width - std::min(window.width, used_cols * cell)) / 2;
    const size_t content_height = total_rows * cell;
    offset_y = content_height < window.height
        ? (window.height - content_height) / 2
        : 0;

    const double max = max_scroll();
    target = std::clamp(target, 0.0, max);
    scroll = std::clamp(scroll, 0.0, max);

    rebuild();
}

void Layout::rebuild()
{
    scheme.clear();
    if (!total || !window) {
        return;
    }

    ImageList& il = ImageList::self();
    const ImageEntryPtr first_entry = il.get(nullptr, ImageList::Dir::First);
    if (!first_entry) {
        return;
    }

    const double cell = thumb_size + thumb_padding;
    size_t row = static_cast<size_t>(
        std::max(0.0, std::floor((scroll - offset_y) / cell)));
    size_t rel_row = 0;

    for (; row < total_rows; ++row) {
        const double top = row_top(row) - scroll;
        if (top >= window.height) {
            break; // below the window
        }
        if (top + thumb_size <= 0) {
            continue; // above the window
        }
        for (size_t col = 0; col < columns; ++col) {
            const size_t index = row * columns + col;
            if (index >= total) {
                break;
            }
            Thumbnail thumb;
            thumb.img = il.get(first_entry, index);
            if (!thumb.img) {
                break;
            }
            thumb.col = col;
            thumb.row = rel_row;
            thumb.pos.x = offset_x + col * cell + thumb_padding;
            thumb.pos.y = static_cast<ssize_t>(std::lround(top));
            scheme.emplace_back(thumb);
        }
        ++rel_row;
    }
}

double Layout::max_scroll() const
{
    const double content = total_rows * (thumb_size + thumb_padding);
    return std::max(0.0, content - window.height);
}

double Layout::row_top(const size_t row) const
{
    return offset_y + static_cast<double>(row * (thumb_size + thumb_padding)) +
        thumb_padding;
}

void Layout::scroll_to(const double offset)
{
    target = std::clamp(offset, 0.0, max_scroll());
    if (!smooth) {
        scroll = target;
        velocity = 0;
        rebuild();
    }
}

void Layout::ensure_visible()
{
    if (!sel_entry || !total) {
        return;
    }

    const double top = row_top(selected_row());
    const double bottom = top + thumb_size;

    double new_target = target;
    if (top - thumb_padding < new_target) {
        new_target = top - thumb_padding;
    } else if (bottom > new_target + window.height) {
        new_target = bottom - window.height;
    }
    scroll_to(new_target);
}

void Layout::center_selection()
{
    if (!sel_entry || !total) {
        return;
    }

    const ssize_t first =
        std::max<ssize_t>(0, static_cast<ssize_t>(selected_row()) -
                                 static_cast<ssize_t>(rows / 2));
    velocity = 0;
    target = std::clamp(row_top(first) - thumb_padding, 0.0, max_scroll());
    scroll = target;
    rebuild();
}

size_t Layout::selected_row() const
{
    assert(sel_entry);
    return sel_entry->index / columns;
}
