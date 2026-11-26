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

void Layout::set_sections(Splitter fn, const size_t height)
{
    const bool anchored = window && sel_entry && !sel_entry->removed && total;
    const double anchor = anchored ? row_top(selected_row()) - scroll : 0;

    splitter = std::move(fn);
    header_size = height;
    starts_valid = false;
    recalc();

    if (anchored && sel_entry) {
        velocity = 0;
        target =
            std::clamp(row_top(selected_row()) - anchor, 0.0, max_scroll());
        scroll = target;
        rebuild();
    }
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

void Layout::set_margins(const size_t top, const size_t bottom)
{
    if (top == margin_top && bottom == margin_bottom) {
        return;
    }
    const bool anchored = window && sel_entry && !sel_entry->removed && total;
    const double anchor = anchored ? row_top(selected_row()) - scroll : 0;

    margin_top = top;
    margin_bottom = bottom;
    recalc();

    if (anchored && sel_entry) {
        velocity = 0;
        target =
            std::clamp(row_top(selected_row()) - anchor, 0.0, max_scroll());
        scroll = target;
        rebuild();
    }
}

void Layout::set_scroll(const double offset)
{
    velocity = 0;
    target = std::clamp(offset, 0.0, max_scroll());
    scroll = target;
    rebuild();
}

void Layout::reset_scroll()
{
    velocity = 0;
    scroll = 0;
    target = 0;
    rebuild();
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

    const double top = view_top(selected_row());
    const double bottom =
        row_top(selected_row()) + thumb_size + margin_bottom;
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

    // vertical movement keeps the column, rows of sections may be shorter
    const auto vertical = [&](const ssize_t delta) -> ImageEntryPtr {
        if (sel_entry->removed || !total || !total_rows) {
            return nullptr;
        }
        const size_t row = selected_row();
        const ssize_t to = static_cast<ssize_t>(row) + delta;
        if (to < 0) {
            return il.get(nullptr, ImageList::Dir::First);
        }
        if (std::cmp_greater_equal(to, total_rows)) {
            return il.get(nullptr, ImageList::Dir::Last);
        }
        return entry_at(index_at(to, col_of(sel_entry->index)));
    };

    switch (dir) {
        case Direction::First:
            next = il.get(nullptr, ImageList::Dir::First);
            break;
        case Direction::Last:
            next = il.get(nullptr, ImageList::Dir::Last);
            break;
        case Direction::Up:
            next = vertical(-1);
            break;
        case Direction::Down:
            next = vertical(1);
            break;
        case Direction::Left:
            next = il.get(sel_entry, ImageList::Dir::Prev);
            break;
        case Direction::Right:
            next = il.get(sel_entry, ImageList::Dir::Next);
            break;
        case Direction::PgUp:
        case Direction::PgDown:
            next = vertical(dir == Direction::PgUp
                                ? -static_cast<ssize_t>(rows)
                                : static_cast<ssize_t>(rows));
            if (next && next != sel_entry) {
                page = row_top(row_of(next->index)) - row_top(selected_row());
            }
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

    const size_t row = selected_row();
    const double top = row_top(row) - scroll;
    const double avail = view_height();
    if (top >= static_cast<double>(margin_top) &&
        top + thumb_size <= margin_top + avail) {
        return false; // fully visible
    }

    // range of fully visible rows
    const double view_top = scroll + margin_top;
    const ssize_t first =
        std::min(rows_above(view_top, true), total_rows - 1);
    const ssize_t last = static_cast<ssize_t>(rows_above(
                             view_top + avail - thumb_size, false)) -
        1;

    size_t new_row;
    if (first > last) {
        // thumbnail is larger than window: use the central row
        new_row = std::clamp<ssize_t>(
            static_cast<ssize_t>(rows_above(view_top + avail / 2.0, false)) -
                1,
            0, total_rows - 1);
    } else {
        new_row = std::cmp_less(row, first) ? first : last;
    }
    if (new_row == row) {
        return false;
    }

    const ImageEntryPtr entry =
        entry_at(index_at(new_row, col_of(sel_entry->index)));
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
        headers.clear();
        sections = { { 0, 0 } };
        total_rows = 0;
        return;
    }
    if (!sel_entry || sel_entry->removed) {
        sel_entry = il.get(nullptr, ImageList::Dir::First);
    }

    // split the list into sections (only when the list was changed)
    if (!splitter) {
        starts.clear();
    } else if (!starts_valid || list_gen != il.generation()) {
        list_gen = il.generation();
        starts = splitter(il.get_all());
        std::erase_if(starts, [this](size_t i) { return i >= total; });
        if (!starts.empty() && starts.front() != 0) {
            starts.insert(starts.begin(), 0);
        }
    }
    starts_valid = true;
    header = starts.empty() ? 0 : header_size;

    const size_t cell = thumb_size + thumb_padding;
    columns = std::max(static_cast<size_t>(1), window.width / cell);
    const size_t avail = static_cast<size_t>(view_height());
    rows = std::max(static_cast<size_t>(1), avail / cell);

    sections.clear();
    total_rows = 0;
    if (starts.empty()) {
        sections.push_back({ 0, 0 });
        total_rows = (total + columns - 1) / columns;
    } else {
        for (size_t i = 0; i < starts.size(); ++i) {
            const size_t end = i + 1 < starts.size() ? starts[i + 1] : total;
            if (end <= starts[i]) {
                continue; // unsorted or duplicate start
            }
            sections.push_back({ starts[i], total_rows });
            total_rows += (end - starts[i] + columns - 1) / columns;
        }
    }

    const size_t used_cols = std::min(columns, total);
    offset_x = (window.width - std::min(window.width, used_cols * cell)) / 2;
    const size_t content_height =
        total_rows * cell + sections.size() * header;
    offset_y = content_height < avail ? (avail - content_height) / 2 : 0;

    const double max = max_scroll();
    target = std::clamp(target, 0.0, max);
    scroll = std::clamp(scroll, 0.0, max);

    rebuild();
}

void Layout::rebuild()
{
    scheme.clear();
    headers.clear();
    if (!total || !window || !total_rows) {
        return;
    }

    ImageList& il = ImageList::self();
    const ImageEntryPtr first_entry = il.get(nullptr, ImageList::Dir::First);
    if (!first_entry) {
        return;
    }

    const double cell = thumb_size + thumb_padding;
    size_t row = rows_above(scroll - thumb_size, false);
    if (row) {
        --row; // last row starting above the window may be still visible
    }
    size_t rel_row = 0;

    // section headers
    if (header) {
        const size_t used_cols = std::min(columns, total);
        for (size_t sec = section_of_row(row); sec < sections.size(); ++sec) {
            const double top = row_top(sections[sec].row) - thumb_padding -
                header - scroll;
            if (top >= window.height) {
                break;
            }
            if (top + header <= 0) {
                continue;
            }
            Header hdr;
            hdr.first = il.get(first_entry, sections[sec].first);
            if (!hdr.first) {
                break;
            }
            hdr.rect = { offset_x + static_cast<ssize_t>(thumb_padding),
                         static_cast<ssize_t>(std::lround(top)),
                         used_cols * static_cast<size_t>(cell) -
                             thumb_padding,
                         header };
            headers.emplace_back(hdr);
        }
    }

    for (; row < total_rows; ++row) {
        const double top = row_top(row) - scroll;
        if (top >= window.height) {
            break; // below the window
        }
        if (top + thumb_size <= 0) {
            continue; // above the window
        }
        const size_t first = row_first(row);
        const size_t end = row_end(row);
        for (size_t index = first; index < end; ++index) {
            const size_t col = index - first;
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
    const double content = total_rows * (thumb_size + thumb_padding) +
        sections.size() * header;
    return std::max(0.0, content - view_height());
}

double Layout::view_height() const
{
    const size_t insets = margin_top + margin_bottom;
    return window.height > insets ? window.height - insets : 1;
}

double Layout::row_top(const size_t row) const
{
    return margin_top + offset_y +
        static_cast<double>(row * (thumb_size + thumb_padding)) +
        thumb_padding +
        static_cast<double>((section_of_row(row) + 1) * header);
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

    const size_t row = selected_row();
    const double bottom = row_top(row) + thumb_size;
    const double top = view_top(row);

    double new_target = target;
    if (top < new_target) {
        new_target = top;
    } else if (bottom > new_target + window.height - margin_bottom) {
        new_target = bottom - (window.height - margin_bottom);
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
    target = std::clamp(view_top(first), 0.0, max_scroll());
    scroll = target;
    rebuild();
}

size_t Layout::selected_row() const
{
    assert(sel_entry);
    return row_of(sel_entry->index);
}

double Layout::view_top(const size_t row) const
{
    double top = row_top(row) - thumb_padding - margin_top;
    if (sections[section_of_row(row)].row == row) {
        top -= header; // reveal section header
    }
    return top;
}

size_t Layout::section_of_index(const size_t index) const
{
    const auto it = std::upper_bound(
        sections.begin(), sections.end(), index,
        [](size_t idx, const Section& sec) { return idx < sec.first; });
    return it == sections.begin() ? 0 : std::distance(sections.begin(), it) - 1;
}

size_t Layout::section_of_row(const size_t row) const
{
    const auto it = std::upper_bound(
        sections.begin(), sections.end(), row,
        [](size_t r, const Section& sec) { return r < sec.row; });
    return it == sections.begin() ? 0 : std::distance(sections.begin(), it) - 1;
}

size_t Layout::row_of(const size_t index) const
{
    const Section& sec = sections[section_of_index(index)];
    return sec.row + (index - std::min(index, sec.first)) / columns;
}

size_t Layout::col_of(const size_t index) const
{
    const Section& sec = sections[section_of_index(index)];
    return (index - std::min(index, sec.first)) % columns;
}

size_t Layout::row_first(const size_t row) const
{
    const Section& sec = sections[section_of_row(row)];
    return sec.first + (row - std::min(row, sec.row)) * columns;
}

size_t Layout::row_end(const size_t row) const
{
    const size_t sec = section_of_row(row);
    const size_t sec_end =
        sec + 1 < sections.size() ? sections[sec + 1].first : total;
    return std::min(row_first(row) + columns, sec_end);
}

size_t Layout::index_at(const size_t row, const size_t col) const
{
    const size_t first = row_first(row);
    const size_t end = row_end(row);
    return end > first ? std::min(first + col, end - 1) : first;
}

ImageEntryPtr Layout::entry_at(const size_t index) const
{
    ImageList& il = ImageList::self();
    const ImageEntryPtr first = il.get(nullptr, ImageList::Dir::First);
    return first ? il.get(first, static_cast<ssize_t>(index)) : nullptr;
}

size_t Layout::rows_above(const double y, const bool strict) const
{
    // row tops grow monotonically: binary search
    size_t lo = 0;
    size_t hi = total_rows;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const double top = row_top(mid);
        if (strict ? top < y : top <= y) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}
