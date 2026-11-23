// SPDX-License-Identifier: MIT
// Software canvas: draws on a pixmap using CPU.

#include "swcanvas.hpp"

#include "render.hpp"

void SwCanvas::fill(const Rectangle& rect, const argb_t& color)
{
    pm.fill(rect, color);
}

void SwCanvas::fill_blend(const Rectangle& rect, const argb_t& color)
{
    pm.fill_blend(rect, color);
}

void SwCanvas::fill_inverse(const Rectangle& preserve, const argb_t& color)
{
    Render::self().fill_inverse(pm, preserve, color);
}

void SwCanvas::grid(const Rectangle& rect, const size_t cell,
                    const argb_t& clr0, const argb_t& clr1)
{
    pm.grid(rect, cell, clr0, clr1);
}

void SwCanvas::rectangle(const Rectangle& rect, const argb_t& color,
                         const size_t thickness)
{
    pm.rectangle(rect, color, thickness);
}

void SwCanvas::mask(const Pixmap& mask, const Point& pos, const argb_t& color)
{
    pm.mask(mask, pos, color);
}

void SwCanvas::blend(const Pixmap& src, const Point& pos)
{
    pm.blend(src, pos);
}

void SwCanvas::draw(const Pixmap& src, const Point& pos, const double scale,
                    const Rectangle& clip, const double /*opacity*/)
{
    const Rectangle full { 0, 0, pm.width(), pm.height() };
    const Rectangle visible = full.intersect(clip);
    if (!visible) {
        return;
    }
    if (visible.x == 0 && visible.y == 0 && visible.width == full.width &&
        visible.height == full.height) {
        Render::self().draw(pm, src, pos, scale);
    } else {
        Pixmap sub = pm.submap(visible);
        Render::self().draw(sub, src, { .x = pos.x - visible.x, .y = pos.y - visible.y },
                            scale);
    }
}

void SwCanvas::background(const Background mode, const Rectangle& preserve)
{
    switch (mode) {
        case Background::Mirror:
            Render::self().mirror_background(pm, preserve);
            break;
        case Background::Extend:
            Render::self().extend_background(pm, preserve);
            break;
    }
}

void SwCanvas::dim_outside(const Point& center, const size_t radius,
                           const double factor)
{
    Render::self().dim_outside(pm, center, radius, factor);
}

void SwCanvas::raster(const std::function<void(Pixmap&)>& fn)
{
    fn(pm);
}
