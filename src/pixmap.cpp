// SPDX-License-Identifier: MIT
// Pixel map.
// Copyright (C) 2024 Artem Senichev <artemsen@gmail.com>

#include "pixmap.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

void Pixmap::touch()
{
    static std::atomic<uint64_t> counter = 0;
    pm_id = ++counter;
}

Pixmap::Pixmap(const Format format, const size_t width, const size_t height,
               void* data) noexcept
{
    attach(format, width, height, data);
}

void Pixmap::create(const Format format, const size_t width,
                    const size_t height, const bool clear)
{
    touch();
    pm_format = format;
    pm_width = width;
    pm_height = height;

    switch (pm_format) {
        case Format::ARGB:
        case Format::RGB:
            pm_bpp = sizeof(argb_t);
            break;
        case Format::GS:
            pm_bpp = sizeof(uint8_t);
            break;
    }

    pm_stride = pm_width * pm_bpp;

    pm_ext = nullptr;
    const size_t prev = pm_data.size();
    const size_t size = pm_height * pm_stride;
    pm_data.resize(size);
    if (clear && size > prev) {
        std::memset(pm_data.data() + prev, 0, size - prev); // new data only
    }
}

void Pixmap::attach(const Format format, const size_t width,
                    const size_t height, void* data, const size_t stride)
{
    touch();
    pm_format = format;
    pm_width = width;
    pm_height = height;

    switch (pm_format) {
        case Format::ARGB:
        case Format::RGB:
            pm_bpp = sizeof(argb_t);
            break;
        case Format::GS:
            pm_bpp = sizeof(uint8_t);
            break;
    }

    pm_stride = stride ? stride : pm_width * pm_bpp;

    pm_ext = reinterpret_cast<uint8_t*>(data);
    pm_data.clear();
}

void Pixmap::free()
{
    pm_id = 0;
    pm_width = 0;
    pm_height = 0;
    pm_bpp = 0;
    pm_stride = 0;
    pm_ext = nullptr;
    pm_data.clear();
}

Pixmap Pixmap::submap(const Rectangle& rect)
{
    Pixmap sub;

    const Rectangle full { 0, 0, pm_width, pm_height };
    const Rectangle visible = full.intersect(rect);

    if (visible) {
        sub.attach(pm_format, visible.width, visible.height,
                   ptr(visible.x, visible.y), pm_stride);
    }

    return sub;
}

void Pixmap::flip_vertical()
{
    touch();
    assert(format() == Format::RGB || format() == Format::ARGB);

    const size_t line_sz = pm_width * pm_bpp;
    std::vector<uint8_t> buffer(line_sz);

    const size_t y_end = pm_height / 2;
    for (size_t y = 0; y < y_end; ++y) {
        void* src = ptr(0, y);
        void* dst = ptr(0, pm_height - y - 1);
        std::memcpy(buffer.data(), dst, line_sz);
        std::memcpy(dst, src, line_sz);
        std::memcpy(src, buffer.data(), line_sz);
    }
}

void Pixmap::flip_horizontal()
{
    touch();
    assert(format() == Format::RGB || format() == Format::ARGB);

    argb_t* pixdata = reinterpret_cast<argb_t*>(ptr(0, 0));
    const size_t stride = pm_stride / sizeof(argb_t);

    for (size_t y = 0; y < pm_height; ++y) {
        argb_t* row = pixdata + y * stride;
        const size_t x_end = pm_width / 2;
        for (size_t x = 0; x < x_end; ++x) {
            std::swap(row[x], row[pm_width - x - 1]);
        }
    }
}

void Pixmap::rotate(const size_t angle)
{
    touch();
    assert(format() == Format::RGB || format() == Format::ARGB);
    assert(angle == 90 || angle == 180 || angle == 270);

    if (angle == 180) {
        const size_t total_pixels = pm_width * pm_height;
        const size_t half_pixels = total_pixels / 2;
        argb_t* pixdata = &at(0, 0);
        for (size_t i = 0; i < half_pixels; ++i) {
            argb_t* color1 = &pixdata[i];
            argb_t* color2 = &pixdata[total_pixels - i - 1];
            const argb_t swap = *color1;
            *color1 = *color2;
            *color2 = swap;
        }
    } else if (angle == 90 || angle == 270) {
        // create temporary buffer with origin data
        Pixmap origin;
        origin.create(pm_format, pm_width, pm_height, false);
        std::memcpy(origin.ptr(0, 0), ptr(0, 0), pm_height * pm_stride);

        // change orientation
        pm_width = origin.pm_height;
        pm_height = origin.pm_width;
        pm_stride = pm_width * pm_bpp;

        // rotate by blocks (cache friendly) in multiple threads
        constexpr size_t block = 64;
        const size_t rows = (pm_height + block - 1) / block;
        const size_t nthreads = std::clamp<size_t>(
            std::min<size_t>(std::thread::hardware_concurrency() / 2, rows), 1,
            8);
        const argb_t* src = &origin.at(0, 0);
        const size_t src_w = origin.pm_width;
        const size_t src_h = origin.pm_height;
        const size_t dst_w = pm_width;
        argb_t* dst = &at(0, 0);

        const auto worker = [&](const size_t first) {
            for (size_t by = first; by < rows; by += nthreads) {
                const size_t y0 = by * block;
                const size_t y1 = std::min(y0 + block, pm_height);
                for (size_t x0 = 0; x0 < pm_width; x0 += block) {
                    const size_t x1 = std::min(x0 + block, pm_width);
                    for (size_t y = y0; y < y1; ++y) {
                        argb_t* line = dst + y * dst_w;
                        for (size_t x = x0; x < x1; ++x) {
                            // 90: (x,y) <- (y, h-1-x); 270: (x,y) <- (w-1-y, x)
                            const size_t ox = angle == 90 ? y : src_w - 1 - y;
                            const size_t oy = angle == 90 ? src_h - 1 - x : x;
                            line[x] = src[oy * src_w + ox];
                        }
                    }
                }
            }
        };

        std::vector<std::thread> threads;
        for (size_t i = 1; i < nthreads; ++i) {
            threads.emplace_back(worker, i);
        }
        worker(0);
        for (auto& it : threads) {
            it.join();
        }
    }
}

void Pixmap::fill(const Rectangle& rect, const argb_t& color)
{
    touch();
    assert(format() == Format::RGB || format() == Format::ARGB);

    const Rectangle visible = rect.intersect({ 0, 0, pm_width, pm_height });
    if (!visible) {
        return; // out of pixmap
    }

    // Wayland compositors expect premultiplied alpha in ARGB buffers
    const argb_t clr = format() == Format::ARGB ? color.premultiplied() : color;

    // create pattern
    const size_t pattern_sz = visible.width * sizeof(argb_t);
    argb_t* pattern = &at(visible.x, visible.y);
    for (size_t x = 0; x < visible.width; ++x) {
        pattern[x] = clr;
    }

    // copy pattern
    const size_t y_end = visible.y + visible.height;
    for (size_t y = visible.y + 1; y < y_end; ++y) {
        std::memcpy(&at(visible.x, y), pattern, pattern_sz);
    }
}

void Pixmap::fill_blend(const Rectangle& rect, const argb_t& color)
{
    touch();
    assert(format() == Format::ARGB);

    const Rectangle visible = rect.intersect({ 0, 0, pm_width, pm_height });
    if (!visible) {
        return; // out of pixmap
    }

    const uint32_t alpha = color.a;
    if (alpha == argb_t::min) {
        return; // fully transparent, nothing changes
    }

    const size_t width = visible.width;
    const size_t stride = pm_stride / sizeof(argb_t);
    argb_t* base = reinterpret_cast<argb_t*>(ptr(visible.x, visible.y));

    // fully opaque: equivalent to a plain fill
    if (alpha == argb_t::max) {
        for (size_t y = 0; y < visible.height; ++y) {
            argb_t* row = base + y * stride;
            for (size_t x = 0; x < width; ++x) {
                row[x] = color;
            }
        }
        return;
    }

    // general case: constant blend factors, hoisted once
    for (size_t y = 0; y < visible.height; ++y) {
        argb_t* row = base + y * stride;
        for (size_t x = 0; x < width; ++x) {
            row[x].blend(color);
        }
    }
}

void Pixmap::grid(const Rectangle& rect, const size_t size, const argb_t& clr0,
                  const argb_t& clr1)
{
    touch();
    assert(format() == Format::ARGB);

    const Rectangle visible = rect.intersect({ 0, 0, pm_width, pm_height });
    if (!visible) {
        return; // out of pixmap
    }

    // fill patterns (premultiplied alpha, as required by Wayland)
    const argb_t clr0_pm = clr0.premultiplied();
    const argb_t clr1_pm = clr1.premultiplied();
    std::vector<argb_t> patterns[2];
    patterns[0].resize(visible.width);
    patterns[1].resize(visible.width);
    for (size_t i = 0; i < visible.width; ++i) {
        const size_t tile = (i / size) % 2;
        patterns[0][i] = tile ? clr0_pm : clr1_pm;
        patterns[1][i] = tile ? clr1_pm : clr0_pm;
    }

    // copy patterns
    const size_t pattern_sz = visible.width * sizeof(argb_t);
    for (size_t y = 0; y < visible.height; ++y) {
        const size_t shift = (y / size) % 2;
        void* line = ptr(visible.x, visible.y + y);
        std::memcpy(line, patterns[shift].data(), pattern_sz);
    }
}

void Pixmap::rectangle(const Rectangle& rect, const argb_t& color,
                       const size_t thickness)
{
    assert(format() == Format::ARGB);

    Rectangle line;

    // top
    line = rect;
    line.height = thickness;
    fill(line, color);

    // bottom
    line.y = rect.y + rect.height - thickness;
    fill(line, color);

    const ssize_t fill_v = static_cast<ssize_t>(rect.height) - thickness * 2;
    if (fill_v > 0) {
        line.y = rect.y + thickness;
        line.width = thickness;
        line.height = fill_v;

        // left
        line.x = rect.x;
        fill(line, color);

        // right
        line.x = rect.x + rect.width - thickness;
        fill(line, color);
    }
}

void Pixmap::mask(const Pixmap& pm, const Point& pos, const argb_t& color)
{
    touch();
    assert(format() == Format::RGB || format() == Format::ARGB);
    assert(pm.format() == Format::GS);

    const Rectangle image { pos, pm };
    const Rectangle visible = image.intersect({ 0, 0, pm_width, pm_height });
    if (!visible) {
        return; // out of pixmap
    }

    const size_t diff_x = visible.x - image.x;
    const size_t diff_y = visible.y - image.y;

    argb_t over { argb_t::min, color.r, color.g, color.b };

    const uint8_t* srow0 = reinterpret_cast<const uint8_t*>(pm.ptr(0, 0));
    const size_t sstride = pm.pm_stride;
    argb_t* drow0 = reinterpret_cast<argb_t*>(ptr(visible.x, visible.y));
    const size_t dstride = pm_stride / sizeof(argb_t);
    const size_t width = visible.width;

    for (size_t y = 0; y < visible.height; ++y) {
        const uint8_t* srow = srow0 + (diff_y + y) * sstride + diff_x;
        argb_t* drow = drow0 + y * dstride;
        for (size_t x = 0; x < width; ++x) {
            over.a = std::min(srow[x], color.a);
            drow[x].blend(over);
        }
    }
}

void Pixmap::copy(const Pixmap& pm, const Point& pos)
{
    touch();
    assert(format() == Format::RGB || format() == Format::ARGB);
    assert(pm.format() == Format::RGB || pm.format() == Format::ARGB);

    const Rectangle image { pos, pm };
    const Rectangle visible = image.intersect({ 0, 0, pm_width, pm_height });
    if (!visible) {
        return; // out of pixmap
    }

    const size_t diff_x = visible.x - image.x;
    const size_t diff_y = visible.y - image.y;

    const size_t line_sz = visible.width * pm_bpp;
    for (size_t y = 0; y < visible.height; ++y) {
        const void* src = pm.ptr(diff_x, diff_y + y);
        void* dst = ptr(visible.x, visible.y + y);
        std::memcpy(dst, src, line_sz);
    }
}

void Pixmap::blend(const Pixmap& pm, const Point& pos)
{
    touch();
    assert(format() == Format::RGB || format() == Format::ARGB);
    assert(pm.format() == Format::RGB || pm.format() == Format::ARGB);
    assert(format() == Format::ARGB || pm.format() == Format::ARGB);

    const Rectangle image { pos, pm };
    const Rectangle visible = image.intersect({ 0, 0, pm_width, pm_height });
    if (!visible) {
        return; // out of pixmap
    }

    const size_t diff_x = visible.x - image.x;
    const size_t diff_y = visible.y - image.y;

    const size_t width = visible.width;

    const argb_t* srow0 = reinterpret_cast<const argb_t*>(pm.ptr(0, 0));
    const size_t sstride = pm.pm_stride / sizeof(argb_t);
    argb_t* drow0 = reinterpret_cast<argb_t*>(ptr(visible.x, visible.y));
    const size_t dstride = pm_stride / sizeof(argb_t);

    for (size_t y = 0; y < visible.height; ++y) {
        const argb_t* srow = srow0 + (diff_y + y) * sstride + diff_x;
        argb_t* drow = drow0 + y * dstride;
        for (size_t x = 0; x < width; ++x) {
            drow[x].blend(srow[x]);
        }
    }
}

void Pixmap::abgr_to_argb()
{
    foreach([](argb_t& color) {
        color.b = color.b ^ color.r;
        color.r = color.b ^ color.r;
        color.b = color.b ^ color.r;
    });
}
