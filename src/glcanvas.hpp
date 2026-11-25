// SPDX-License-Identifier: MIT
// GPU canvas: draws using OpenGL ES 3.0.

#pragma once

#include "canvas.hpp"

#include <GLES3/gl3.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class GlCanvas : public Canvas {
public:
    GlCanvas() = default;
    ~GlCanvas() override;

    GlCanvas(const GlCanvas&) = delete;
    GlCanvas& operator=(const GlCanvas&) = delete;

    /**
     * Initialize GL resources, GL context must be current.
     * @return false on error
     */
    bool initialize();

    /**
     * Enable uploading of large textures in a background thread.
     * @param bind function to make a shared GL context current on the
     *             calling thread
     * @param unbind function to release the context
     */
    void enable_async_upload(const std::function<bool()>& bind,
                             const std::function<void()>& unbind);

    /**
     * Begin frame.
     * @param size framebuffer size in pixels
     */
    void begin(const Size& size);

    /**
     * Finish frame (free unused resources).
     */
    void end();

    /**
     * Compile shaders in advance by drawing offscreen (call between frames).
     */
    void warmup();

    [[nodiscard]] Size size() const override { return fb_size; }

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
    bool draw_texture(const TextureRenderer& fn) override;
    void begin_layer() override;
    bool preload(const Pixmap& pm, const std::shared_ptr<const void>& owner,
                 const double budget, const double scale) override;
    void end_layer(const double opacity) override;

private:
    /** Texture tile (large images are split to multiple textures). */
    struct Tile {
        GLuint id;      ///< GL texture
        Rectangle area; ///< Area of the source pixmap
    };

    /** Cached texture created from pixmap. */
    struct Texture {
        std::vector<Tile> tiles; ///< Texture tiles
        size_t bytes = 0;        ///< Size in GPU memory
        uint64_t last_use = 0;   ///< Number of the frame where used last time
        bool mipmap = false;     ///< Mipmaps generated
        bool pending = false;    ///< Upload is in progress (see preload)
        bool reduced = false;    ///< Reduced resolution, full one follows
        size_t up_tile = 0;      ///< Pending upload: current tile
        size_t up_row = 0;       ///< Pending upload: next row in the tile
    };

    /** Shader program with uniform locations. */
    struct Program {
        GLuint id = 0;
        GLint viewport = -1;
        GLint color = -1;
        GLint color2 = -1;
        GLint param = -1;
        GLint param2 = -1;
    };

    /** Offscreen render target. */
    struct Target {
        GLuint fbo = 0;
        GLuint tex = 0;
        Size size;
    };

    /** Blending modes. */
    enum class Blend : uint8_t {
        None,     ///< Replace pixels
        Straight, ///< Source with straight alpha
        Premult,  ///< Source with premultiplied alpha
        Multiply, ///< Multiply destination color by source color
    };

    /** Quad vertex: position and texture coordinates. */
    struct UvRect {
        float u0, v0, u1, v1;
    };

    static GLuint compile(const char* fragment);
    static Program make_program(const char* fragment);

    const Texture& get_texture(const Pixmap& pm, const bool mipmap);
    Texture create_texture(const Pixmap& pm, const bool upload) const;
    static bool upload_rows(const Pixmap& pm, Texture& tex,
                            const double budget);
    static void free_texture(Texture& tex);

    /** Background upload job. */
    struct UploadJob {
        const Pixmap* pm;                  ///< Pixmap to upload
        std::shared_ptr<const void> owner; ///< Owner of the pixmap
        size_t level = 0;                  ///< Downscale level of first stage
        Texture reduced;                   ///< First stage: reduced texture
        GLsync reduced_fence = nullptr;    ///< First stage complete fence
        bool reduced_done = false;         ///< First stage is finished
        bool reduced_taken = false;        ///< First stage is in the cache
        Texture tex;                       ///< Full resolution texture
        GLsync fence = nullptr;            ///< Upload complete fence
        bool done = false;                 ///< Job is finished
    };

    /**
     * Upload texture in background thread (allocate, fill, mipmaps).
     * @param pm pixmap to upload
     * @return texture and fence of the upload
     */
    std::pair<Texture, GLsync> upload_async(const Pixmap& pm,
                                            const bool urgent) const;

    /** Time of the last frame (steady clock ticks), used to slow down
     * non-urgent uploads while animation frames are drawn. */
    std::atomic<int64_t> last_frame_time = 0;

    void upload_worker();
    bool collect_uploads(); ///< Take finished uploads, false if failed

    std::thread up_thread;                        ///< Upload thread
    std::mutex up_mutex;                          ///< Upload queue sync
    std::condition_variable up_cv;                ///< Upload queue signal
    std::deque<std::shared_ptr<UploadJob>> up_queue; ///< Waiting jobs
    std::unordered_map<uint64_t, std::shared_ptr<UploadJob>> up_jobs; ///< Jobs
    bool up_stop = false;                 ///< Stop upload thread
    std::atomic<bool> up_failed = false;  ///< Upload context is unusable
    std::function<bool()> up_bind;        ///< Bind upload context
    std::function<void()> up_unbind;      ///< Unbind upload context

    void set_blend(const Blend mode);
    void use(const Program& prog, const Size& viewport);
    void quad(const float x, const float y, const float w, const float h,
              const UvRect& uv = { 0.0f, 0.0f, 1.0f, 1.0f });
    void solid(const Rectangle& rect, const argb_t& color, const Blend mode);

    void bind_target(const Target* target);
    void bind_output(); ///< Bind current output: layer or window
    void reset_state(); ///< Restore GL state after external renderer
    static bool resize_target(Target& target, const Size& size);
    static void free_target(Target& target);

    Size fb_size;            ///< Current framebuffer size
    uint64_t frame = 0;      ///< Frame counter
    GLint max_tex_size = 0;  ///< Max texture size supported by GPU
    GLuint vao = 0;          ///< Vertex array
    GLuint vbo = 0;          ///< Vertex buffer
    Blend blend_mode = Blend::None;

    Program prog_solid;  ///< Solid color
    Program prog_tex;    ///< Textured quad
    Program prog_mask;   ///< Grayscale mask with color
    Program prog_grid;   ///< Chessboard
    Program prog_dim;    ///< Dim outside circle
    Program prog_blur;   ///< Separable gaussian blur

    std::unordered_map<uint64_t, Texture> textures; ///< Texture cache
    size_t textures_bytes = 0;                      ///< Total size of cache

    GLuint capture_tex = 0; ///< Captured framebuffer area (backgrounds)
    Target blur[2];         ///< Blur render targets
    Target layer;              ///< Offscreen layer
    bool layer_active = false; ///< Drawing to the layer
    const Target* output = nullptr; ///< Offscreen output (nullptr = window)
    GLuint raster_tex = 0;  ///< Software rasterized layer
    Pixmap raster_pm;       ///< Software rasterized layer pixmap
};
