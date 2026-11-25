// SPDX-License-Identifier: MIT
// Antialiased shape masks for UI overlays.

#include "shapes.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace {

/** Signed distance to rounded rectangle centered at (cx, cy). */
double rrect_distance(const double px, const double py, const double cx,
                      const double cy, const double hw, const double hh,
                      const double r)
{
    const double qx = std::abs(px - cx) - (hw - r);
    const double qy = std::abs(py - cy) - (hh - r);
    const double ox = std::max(qx, 0.0);
    const double oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
}

/** Distance from point to segment. */
double segment_distance(const double px, const double py, const double ax,
                        const double ay, const double bx, const double by)
{
    const double dx = bx - ax;
    const double dy = by - ay;
    const double len = dx * dx + dy * dy;
    const double t =
        len ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len, 0.0, 1.0)
            : 0.0;
    const double cx = ax + t * dx - px;
    const double cy = ay + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

/** Icon primitive in unit coordinates (0..1). */
struct Prim {
    enum Type : uint8_t { Line, Frame, Box, Dot } type;
    double x0, y0, x1, y1; ///< Line ends or rectangle corners, dot center
    double r;              ///< Corner/dot radius
};

/** Icon definitions: stroke primitives. */
std::vector<Prim> icon_prims(const Shapes::Icon icon)
{
    using enum Shapes::Icon;
    switch (icon) {
        case Filter:
            return { { Prim::Line, 0.16, 0.30, 0.84, 0.30, 0 },
                     { Prim::Line, 0.28, 0.50, 0.72, 0.50, 0 },
                     { Prim::Line, 0.40, 0.70, 0.60, 0.70, 0 } };
        case Check:
            return { { Prim::Line, 0.16, 0.52, 0.40, 0.76, 0 },
                     { Prim::Line, 0.40, 0.76, 0.86, 0.26, 0 } };
        case All:
            return { { Prim::Line, 0.30, 0.12, 0.88, 0.12, 0 },
                     { Prim::Line, 0.88, 0.12, 0.88, 0.70, 0 },
                     { Prim::Frame, 0.12, 0.30, 0.72, 0.88, 0.08 },
                     { Prim::Line, 0.25, 0.60, 0.37, 0.72, 0 },
                     { Prim::Line, 0.37, 0.72, 0.60, 0.46, 0 } };
        case Photo:
            return { { Prim::Frame, 0.12, 0.16, 0.88, 0.84, 0.10 },
                     { Prim::Line, 0.20, 0.74, 0.42, 0.50, 0 },
                     { Prim::Line, 0.42, 0.50, 0.62, 0.70, 0 },
                     { Prim::Line, 0.58, 0.64, 0.68, 0.55, 0 },
                     { Prim::Line, 0.68, 0.55, 0.82, 0.70, 0 },
                     { Prim::Dot, 0.66, 0.34, 0, 0, 0.07 } };
        case Video:
            return { { Prim::Frame, 0.10, 0.26, 0.66, 0.74, 0.08 },
                     { Prim::Line, 0.66, 0.44, 0.90, 0.30, 0 },
                     { Prim::Line, 0.90, 0.30, 0.90, 0.70, 0 },
                     { Prim::Line, 0.90, 0.70, 0.66, 0.56, 0 } };
    }
    return {};
}

} // namespace

const Pixmap& Shapes::rounded_rect(const Size& size, const size_t radius)
{
    static std::map<std::tuple<size_t, size_t, size_t>, Pixmap> cache;

    const auto key = std::make_tuple(size.width, size.height, radius);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    if (cache.size() > 64) {
        cache.clear();
    }

    Pixmap pm;
    pm.create(Pixmap::GS, std::max<size_t>(size.width, 1),
              std::max<size_t>(size.height, 1));
    const double w = pm.width();
    const double h = pm.height();
    const double r = std::min({ static_cast<double>(radius), w / 2, h / 2 });
    for (size_t y = 0; y < pm.height(); ++y) {
        for (size_t x = 0; x < pm.width(); ++x) {
            const double d =
                rrect_distance(x + 0.5, y + 0.5, w / 2, h / 2, w / 2, h / 2, r);
            *static_cast<uint8_t*>(pm.ptr(x, y)) =
                static_cast<uint8_t>(std::clamp(0.5 - d, 0.0, 1.0) * 255);
        }
    }
    return cache.insert_or_assign(key, std::move(pm)).first->second;
}

const Pixmap& Shapes::pill(const Size& size)
{
    return rounded_rect(size, std::min(size.width, size.height) / 2);
}

const Pixmap& Shapes::icon(const Icon icon, const size_t size)
{
    static std::map<std::pair<Icon, size_t>, Pixmap> cache;

    const auto key = std::make_pair(icon, size);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    const std::vector<Prim> prims = icon_prims(icon);
    const double s = std::max<size_t>(size, 1);
    const double half = std::max(s * 0.04, 0.75); // half of stroke width

    Pixmap pm;
    pm.create(Pixmap::GS, s, s);
    for (size_t y = 0; y < pm.height(); ++y) {
        for (size_t x = 0; x < pm.width(); ++x) {
            const double px = x + 0.5;
            const double py = y + 0.5;
            double cov = 0;
            for (const Prim& p : prims) {
                double c = 0;
                switch (p.type) {
                    case Prim::Line:
                        c = half + 0.5 -
                            segment_distance(px, py, p.x0 * s, p.y0 * s,
                                             p.x1 * s, p.y1 * s);
                        break;
                    case Prim::Frame:
                    case Prim::Box: {
                        const double hw = (p.x1 - p.x0) * s / 2;
                        const double hh = (p.y1 - p.y0) * s / 2;
                        const double d = rrect_distance(
                            px, py, p.x0 * s + hw, p.y0 * s + hh, hw, hh,
                            p.r * s);
                        c = p.type == Prim::Box ? 0.5 - d
                                                : half + 0.5 - std::abs(d);
                        break;
                    }
                    case Prim::Dot:
                        c = p.r * s + 0.5 -
                            std::hypot(px - p.x0 * s, py - p.y0 * s);
                        break;
                }
                cov = std::max(cov, std::clamp(c, 0.0, 1.0));
            }
            *static_cast<uint8_t*>(pm.ptr(x, y)) =
                static_cast<uint8_t>(cov * 255);
        }
    }
    return cache.insert_or_assign(key, std::move(pm)).first->second;
}

const Pixmap& Shapes::triangle(const Size& size, const bool up)
{
    static std::map<std::tuple<size_t, size_t, bool>, Pixmap> cache;

    const auto key = std::make_tuple(size.width, size.height, up);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    if (cache.size() > 64) {
        cache.clear();
    }

    Pixmap pm;
    pm.create(Pixmap::GS, std::max<size_t>(size.width, 1),
              std::max<size_t>(size.height, 1));
    const double w = pm.width();
    const double h = pm.height();
    // apex at the top center, base at the bottom (flipped for down)
    const double half = w / 2;
    constexpr int ss = 4; // supersampling per axis
    for (size_t y = 0; y < pm.height(); ++y) {
        for (size_t x = 0; x < pm.width(); ++x) {
            int inside = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const double px = x + (sx + 0.5) / ss;
                    double py = y + (sy + 0.5) / ss;
                    if (!up) {
                        py = h - py;
                    }
                    // inside if below both slanted edges
                    const double dx = std::abs(px - half);
                    if (dx * h <= py * half && py <= h) {
                        ++inside;
                    }
                }
            }
            *static_cast<uint8_t*>(pm.ptr(x, y)) =
                static_cast<uint8_t>(inside * 255 / (ss * ss));
        }
    }
    return cache.insert_or_assign(key, std::move(pm)).first->second;
}
