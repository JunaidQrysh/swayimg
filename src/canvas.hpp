// SPDX-License-Identifier: MIT
// Drawing surface interface (window canvas).

#pragma once

#include "pixmap.hpp"

#include <functional>
#include <memory>

/**
 * Abstract drawing surface used for window redraw.
 * Implementations: software (Pixmap based) and GPU (OpenGL ES).
 */
class Canvas {
public:
    /** Background fill modes around the image. */
    enum class Background : uint8_t {
        Mirror, ///< Mirrored and blurred image edges
        Extend, ///< Scaled up and blurred image
    };

    virtual ~Canvas() = default;

    /**
     * Get canvas size.
     * @return canvas size in pixels
     */
    [[nodiscard]] virtual Size size() const = 0;

    /**
     * Fill rectangle with color (replace pixels).
     * @param rect area to fill
     * @param color fill color
     */
    virtual void fill(const Rectangle& rect, const argb_t& color) = 0;

    /**
     * Fill rectangle with color using alpha blending.
     * @param rect area to fill
     * @param color fill color
     */
    virtual void fill_blend(const Rectangle& rect, const argb_t& color) = 0;

    /**
     * Fill everything outside the rectangle.
     * @param preserve area to keep untouched
     * @param color fill color
     */
    virtual void fill_inverse(const Rectangle& preserve,
                              const argb_t& color) = 0;

    /**
     * Fill rectangle with chessboard grid.
     * @param rect area to fill
     * @param cell grid cell size
     * @param clr0,clr1 grid colors
     */
    virtual void grid(const Rectangle& rect, const size_t cell,
                      const argb_t& clr0, const argb_t& clr1) = 0;

    /**
     * Draw rectangle outline.
     * @param rect rectangle geometry
     * @param color line color
     * @param thickness line thickness
     */
    virtual void rectangle(const Rectangle& rect, const argb_t& color,
                           const size_t thickness) = 0;

    /**
     * Draw grayscale mask (icon, glyph) with the specified color.
     * @param mask grayscale pixmap used as alpha mask
     * @param pos top-left coordinates
     * @param color color to apply
     */
    virtual void mask(const Pixmap& mask, const Point& pos,
                      const argb_t& color) = 0;

    /**
     * Draw ARGB pixmap without scaling using alpha blending.
     * @param pm pixmap to draw
     * @param pos top-left coordinates
     */
    virtual void blend(const Pixmap& pm, const Point& pos) = 0;

    /**
     * Draw scaled pixmap (image).
     * @param pm pixmap to draw
     * @param pos top-left coordinates of the scaled image
     * @param scale scale factor
     * @param clip clipping area, image is drawn only inside it
     * @param opacity image opacity in range [0,1] (may be ignored by
     *                software implementation)
     */
    virtual void draw(const Pixmap& pm, const Point& pos, const double scale,
                      const Rectangle& clip, const double opacity) = 0;

    /**
     * Draw scaled pixmap (image) with clipping.
     * @param pm pixmap to draw
     * @param pos top-left coordinates of the scaled image
     * @param scale scale factor
     * @param clip clipping area, image is drawn only inside it
     */
    void draw(const Pixmap& pm, const Point& pos, const double scale,
              const Rectangle& clip)
    {
        draw(pm, pos, scale, clip, 1.0);
    }

    /**
     * Draw scaled pixmap (image) without clipping.
     * @param pm pixmap to draw
     * @param pos top-left coordinates of the scaled image
     * @param scale scale factor
     */
    void draw(const Pixmap& pm, const Point& pos, const double scale)
    {
        const Size sz = size();
        draw(pm, pos, scale, { 0, 0, sz.width, sz.height }, 1.0);
    }

    /**
     * Fill area outside the image with effect based on image content.
     * Must be called after the image was drawn at `preserve` area.
     * @param mode background mode
     * @param preserve image area
     */
    virtual void background(const Background mode,
                            const Rectangle& preserve) = 0;

    /**
     * Dim everything outside the circle.
     * @param center circle center
     * @param radius circle radius
     * @param factor dim factor
     */
    virtual void dim_outside(const Point& center, const size_t radius,
                             const double factor) = 0;

    /**
     * Draw with a custom software renderer (e.g. vector images).
     * The callback receives an ARGB pixmap of canvas size; everything drawn
     * there is composited onto the canvas using alpha blending.
     * @param fn drawing callback
     */
    virtual void raster(const std::function<void(Pixmap&)>& fn) = 0;

    /** Callback to render GPU texture of requested size, returns texture id
     * (bottom-up orientation) or 0 if nothing to draw. */
    using TextureRenderer = std::function<unsigned(const Size&)>;

    /**
     * Draw externally rendered GPU texture (e.g. video frame).
     * @param fn callback to render the texture
     * @param src_size size of the content at scale 1.0
     * @param render_scale scale used to choose texture size (stable value
     *                     to avoid re-rendering during animations)
     * @param pos top-left coordinates of the scaled content
     * @param scale scale factor relative to native size
     * @param clip clipping area
     * @return false if not supported or nothing was drawn
     */
    virtual bool draw_texture(const TextureRenderer& /*fn*/,
                              const Size& /*src_size*/,
                              const double /*render_scale*/,
                              const Point& /*pos*/, const double /*scale*/,
                              const Rectangle& /*clip*/)
    {
        return false;
    }

    /**
     * Begin layer: following drawing goes to a separate transparent layer
     * which is composited onto the canvas by end_layer().
     * Software implementation draws directly on the canvas.
     */
    virtual void begin_layer() {}

    /**
     * Prepare pixmap for drawing in small steps (e.g. upload to GPU) to
     * avoid long frames when a large image is shown for the first time.
     * @param pm pixmap to prepare
     * @param owner object that owns the pixmap, kept alive while the pixmap
     *              is processed in background
     * @param budget max time to spend in this call (seconds)
     * @param scale scale the pixmap will be drawn with: the pixmap is ready
     *              when it can be drawn at this scale without quality loss,
     *              full resolution may follow later
     * @return true if the pixmap is ready to be drawn without delays
     */
    virtual bool preload(const Pixmap& /*pm*/,
                         const std::shared_ptr<const void>& /*owner*/,
                         const double /*budget*/, const double /*scale*/)
    {
        return true;
    }

    /**
     * Finish layer and composite it onto the canvas.
     * @param opacity layer opacity in range [0,1]
     */
    virtual void end_layer(const double /*opacity*/) {}
};
