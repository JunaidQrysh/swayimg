// SPDX-License-Identifier: MIT
// Software canvas: draws on a pixmap using CPU.

#pragma once

#include "canvas.hpp"

class SwCanvas : public Canvas {
public:
    /**
     * Constructor.
     * @param target target pixmap (window buffer)
     */
    explicit SwCanvas(Pixmap& target)
        : pm(target)
    {
    }

    [[nodiscard]] Size size() const override { return pm; }

    void fill(const Rectangle& rect, const argb_t& color) override;
    void fill_blend(const Rectangle& rect, const argb_t& color) override;
    void fill_inverse(const Rectangle& preserve, const argb_t& color) override;
    void grid(const Rectangle& rect, const size_t cell, const argb_t& clr0,
              const argb_t& clr1) override;
    void rectangle(const Rectangle& rect, const argb_t& color,
                   const size_t thickness) override;
    void mask(const Pixmap& mask, const Point& pos,
              const argb_t& color) override;
    void blend(const Pixmap& src, const Point& pos) override;
    using Canvas::draw;
    void draw(const Pixmap& src, const Point& pos, const double scale,
              const Rectangle& clip, const double opacity) override;
    void background(const Background mode, const Rectangle& preserve) override;
    void dim_outside(const Point& center, const size_t radius,
                     const double factor) override;
    void raster(const std::function<void(Pixmap&)>& fn) override;

private:
    Pixmap& pm; ///< Target pixmap
};
