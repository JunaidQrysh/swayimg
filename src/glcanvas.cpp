// SPDX-License-Identifier: MIT
// GPU canvas: draws using OpenGL ES 3.0.

#include "glcanvas.hpp"

#include "log.hpp"
#include "render.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <sys/resource.h>
#include <unistd.h>

namespace {

// Max size of texture cache in GPU memory
constexpr size_t TEXTURE_CACHE_BYTES = 512UL * 1024 * 1024;

// Background blur: downscale factor and gaussian sigma (in window pixels)
constexpr size_t BLUR_DOWNSCALE = 4;
constexpr float BLUR_SIGMA = 16.0f;

constexpr const char* VERTEX_SHADER = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
uniform vec2 u_viewport;
out vec2 v_uv;
out vec2 v_pos;
void main() {
    v_uv = a_uv;
    v_pos = a_pos;
    vec2 ndc = a_pos / u_viewport * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

constexpr const char* FRAG_SOLID = R"(#version 300 es
precision highp float;
uniform vec4 u_color;
out vec4 o_color;
void main() {
    o_color = u_color;
}
)";

// u_param: alpha multiplier, color multiplier (for premultiplied alpha)
constexpr const char* FRAG_TEXTURE = R"(#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform vec4 u_param;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec4 color = texture(u_tex, v_uv);
    o_color = vec4(color.rgb * u_param.y, color.a * u_param.x);
}
)";

constexpr const char* FRAG_MASK = R"(#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform vec4 u_color;
in vec2 v_uv;
out vec4 o_color;
void main() {
    o_color = vec4(u_color.rgb, min(texture(u_tex, v_uv).r, u_color.a));
}
)";

// u_param: origin.xy, cell size
constexpr const char* FRAG_GRID = R"(#version 300 es
precision highp float;
uniform vec4 u_color;
uniform vec4 u_color2;
uniform vec4 u_param;
in vec2 v_pos;
out vec4 o_color;
void main() {
    vec2 cell = floor((v_pos - u_param.xy) / u_param.z);
    o_color = mod(cell.x + cell.y, 2.0) >= 1.0 ? u_color : u_color2;
}
)";

// u_param: center.xy, radius^2, dim factor
constexpr const char* FRAG_DIM = R"(#version 300 es
precision highp float;
uniform vec4 u_param;
in vec2 v_pos;
out vec4 o_color;
void main() {
    vec2 d = v_pos - u_param.xy;
    if (dot(d, d) <= u_param.z) {
        discard;
    }
    o_color = vec4(u_param.w, u_param.w, u_param.w, 1.0);
}
)";

// Separable gaussian blur with fixed kernel (radius 12, weights from CPU);
// u_param: texel step (direction)
constexpr int BLUR_RADIUS = 12;
constexpr const char* FRAG_BLUR = R"(#version 300 es
precision highp float;
uniform sampler2D u_tex;
uniform vec4 u_param;
uniform float u_weights[13];
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec4 acc = texture(u_tex, v_uv) * u_weights[0];
    for (int i = 1; i <= 12; ++i) {
        vec2 offset = u_param.xy * float(i);
        acc += (texture(u_tex, v_uv + offset) +
                texture(u_tex, v_uv - offset)) * u_weights[i];
    }
    o_color = acc;
}
)";

/** Convert color to normalized components. */
struct Color {
    explicit Color(const argb_t& c)
        : r(c.r / 255.0f)
        , g(c.g / 255.0f)
        , b(c.b / 255.0f)
        , a(c.a / 255.0f)
    {
    }
    float r, g, b, a;
};

void set_uniform(const GLint loc, const argb_t& color)
{
    const Color c(color);
    glUniform4f(loc, c.r, c.g, c.b, c.a);
}

void set_filter(const GLint min, const GLint mag)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
}

void set_wrap(const GLint mode)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mode);
}

} // namespace

/**
 * Downscale pixmap by 2^level with box filter (multiple threads).
 * @param src source pixmap
 * @param level downscale level
 * @return downscaled pixmap
 */
static Pixmap downscale(const Pixmap& src, const size_t level)
{
    const size_t f = static_cast<size_t>(1) << level;
    const size_t sw = src.width();
    const size_t sh = src.height();
    const size_t dw = (sw + f - 1) / f;
    const size_t dh = (sh + f - 1) / f;

    Pixmap dst;
    dst.create(src.format(), dw, dh, false);

    const auto worker = [&](const size_t first, const size_t step) {
        for (size_t y = first; y < dh; y += step) {
            const size_t y0 = y * f;
            const size_t y1 = std::min(y0 + f, sh);
            for (size_t x = 0; x < dw; ++x) {
                const size_t x0 = x * f;
                const size_t x1 = std::min(x0 + f, sw);
                uint32_t a = 0, r = 0, g = 0, b = 0;
                for (size_t sy = y0; sy < y1; ++sy) {
                    for (size_t sx = x0; sx < x1; ++sx) {
                        const argb_t& c = src.at(sx, sy);
                        a += c.a;
                        r += c.r;
                        g += c.g;
                        b += c.b;
                    }
                }
                const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
                argb_t& d = dst.at(x, y);
                d.a = static_cast<uint8_t>((a + n / 2) / n);
                d.r = static_cast<uint8_t>((r + n / 2) / n);
                d.g = static_cast<uint8_t>((g + n / 2) / n);
                d.b = static_cast<uint8_t>((b + n / 2) / n);
            }
        }
    };

    // few threads: it runs during animations, limit memory bandwidth use
    const size_t nthreads =
        std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 1, 2);
    std::vector<std::thread> threads;
    for (size_t i = 1; i < nthreads; ++i) {
        threads.emplace_back(worker, i, nthreads);
    }
    worker(0, nthreads);
    for (auto& it : threads) {
        it.join();
    }

    return dst;
}

GlCanvas::~GlCanvas()
{
    if (up_thread.joinable()) {
        {
            const std::scoped_lock lock(up_mutex);
            up_stop = true;
        }
        up_cv.notify_all();
        up_thread.join();
    }
    for (auto& [_, job] : up_jobs) {
        if (job->reduced_done && !job->reduced_taken) {
            free_texture(job->reduced);
            glDeleteSync(job->reduced_fence);
        }
        if (job->done) {
            free_texture(job->tex);
            glDeleteSync(job->fence);
        }
    }

    for (auto& [_, tex] : textures) {
        free_texture(tex);
    }
    for (Target& it : blur) {
        free_target(it);
    }
    free_target(layer);
    for (const Program* it : { &prog_solid, &prog_tex, &prog_mask, &prog_grid,
                               &prog_dim, &prog_blur }) {
        if (it->id) {
            glDeleteProgram(it->id);
        }
    }
    const GLuint texs[] = { capture_tex, raster_tex };
    glDeleteTextures(2, texs);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

bool GlCanvas::initialize()
{
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);
    if (max_tex_size <= 0) {
        max_tex_size = 2048;
    }

    prog_solid = make_program(FRAG_SOLID);
    prog_tex = make_program(FRAG_TEXTURE);
    prog_mask = make_program(FRAG_MASK);
    prog_grid = make_program(FRAG_GRID);
    prog_dim = make_program(FRAG_DIM);
    prog_blur = make_program(FRAG_BLUR);
    if (!prog_solid.id || !prog_tex.id || !prog_mask.id || !prog_grid.id ||
        !prog_dim.id || !prog_blur.id) {
        return false;
    }

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), nullptr,
                 GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    glGenTextures(1, &capture_tex);
    glGenTextures(1, &raster_tex);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    blend_mode = Blend::None;

    const char* renderer =
        reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    Log::verbose("GPU rendering enabled: {}", renderer ? renderer : "?");

    return glGetError() == GL_NO_ERROR;
}

void GlCanvas::begin(const Size& size)
{
    if (up_thread.joinable()) {
        last_frame_time.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
        collect_uploads();
    }

    // window resized: resize offscreen targets now (e.g. the window becomes
    // fullscreen at startup), not in the middle of the next animation
    if (layer.fbo && (layer.size.width != size.width ||
                      layer.size.height != size.height)) {
        resize_target(layer, size);
        const Size blur_size = {
            std::max(size.width / BLUR_DOWNSCALE, static_cast<size_t>(1)),
            std::max(size.height / BLUR_DOWNSCALE, static_cast<size_t>(1)),
        };
        for (Target& it : blur) {
            if (it.fbo) {
                resize_target(it, blur_size);
            }
        }
    }

    fb_size = size;
    layer_active = false;
    bind_target(nullptr);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void GlCanvas::end()
{
    // evict least recently used textures
    if (textures_bytes > TEXTURE_CACHE_BYTES) {
        std::vector<std::pair<uint64_t, uint64_t>> lru; // last use, id
        lru.reserve(textures.size());
        for (const auto& [id, tex] : textures) {
            if (tex.last_use != frame) {
                lru.emplace_back(tex.last_use, id);
            }
        }
        std::ranges::sort(lru);
        for (const auto& [_, id] : lru) {
            if (textures_bytes <= TEXTURE_CACHE_BYTES) {
                break;
            }
            auto it = textures.find(id);
            textures_bytes -= it->second.bytes;
            free_texture(it->second);
            textures.erase(it);
        }
    }
    ++frame;
}

void GlCanvas::fill(const Rectangle& rect, const argb_t& color)
{
    solid(rect, color.premultiplied(), Blend::None);
}

void GlCanvas::fill_blend(const Rectangle& rect, const argb_t& color)
{
    if (color.a == argb_t::min) {
        return;
    }
    solid(rect, color,
          color.a == argb_t::max ? Blend::None : Blend::Straight);
}

void GlCanvas::fill_inverse(const Rectangle& preserve, const argb_t& color)
{
    const Rectangle full { 0, 0, fb_size.width, fb_size.height };
    const auto [top, bottom, left, right] = full.cutout(preserve);
    for (const Rectangle* it : { &top, &bottom, &left, &right }) {
        if (*it) {
            fill(*it, color);
        }
    }
}

void GlCanvas::grid(const Rectangle& rect, const size_t cell,
                    const argb_t& clr0, const argb_t& clr1)
{
    use(prog_grid, fb_size);
    set_blend(Blend::None);
    set_uniform(prog_grid.color, clr0.premultiplied());
    set_uniform(prog_grid.color2, clr1.premultiplied());
    glUniform4f(prog_grid.param, static_cast<float>(rect.x),
                static_cast<float>(rect.y),
                static_cast<float>(std::max(cell, static_cast<size_t>(1))),
                0.0f);
    quad(rect.x, rect.y, rect.width, rect.height);
}

void GlCanvas::rectangle(const Rectangle& rect, const argb_t& color,
                         const size_t thickness)
{
    Rectangle line = rect;
    line.height = thickness;
    fill(line, color); // top
    line.y = rect.y + rect.height - thickness;
    fill(line, color); // bottom

    const ssize_t fill_v = static_cast<ssize_t>(rect.height) - thickness * 2;
    if (fill_v > 0) {
        line.y = rect.y + thickness;
        line.width = thickness;
        line.height = fill_v;
        line.x = rect.x;
        fill(line, color); // left
        line.x = rect.x + rect.width - thickness;
        fill(line, color); // right
    }
}

void GlCanvas::mask(const Pixmap& mask, const Point& pos, const argb_t& color)
{
    if (!mask) {
        return;
    }
    const Texture& tex = get_texture(mask, false);
    use(prog_mask, fb_size);
    set_blend(Blend::Straight);
    set_uniform(prog_mask.color, color);
    for (const Tile& tile : tex.tiles) {
        glBindTexture(GL_TEXTURE_2D, tile.id);
        set_filter(GL_NEAREST, GL_NEAREST);
        quad(pos.x + tile.area.x, pos.y + tile.area.y, tile.area.width,
             tile.area.height);
    }
}

void GlCanvas::blend(const Pixmap& src, const Point& pos)
{
    if (!src) {
        return;
    }
    const Texture& tex = get_texture(src, false);
    use(prog_tex, fb_size);
    set_blend(src.format() == Pixmap::ARGB ? Blend::Straight : Blend::None);
    for (const Tile& tile : tex.tiles) {
        glBindTexture(GL_TEXTURE_2D, tile.id);
        set_filter(GL_NEAREST, GL_NEAREST);
        quad(pos.x + tile.area.x, pos.y + tile.area.y, tile.area.width,
             tile.area.height);
    }
}

void GlCanvas::draw(const Pixmap& src, const Point& pos, const double scale,
                    const Rectangle& clip, const double opacity)
{
    if (opacity <= 0.0) {
        return;
    }
    if (!src || scale <= 0.0) {
        return;
    }

    const Rectangle full { 0, 0, fb_size.width, fb_size.height };
    const Rectangle visible = full.intersect(clip);
    if (!visible) {
        return;
    }

    const bool one2one = scale == 1.0;
    const bool aa = Render::self().antialiasing;
    const Texture& tex = get_texture(src, !one2one && aa && scale < 1.0);

    GLint min_filter = GL_NEAREST;
    GLint mag_filter = GL_NEAREST;
    if (!one2one && aa) {
        min_filter = tex.mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
        mag_filter = GL_LINEAR;
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(visible.x,
              static_cast<GLint>(fb_size.height) - visible.y -
                  static_cast<GLint>(visible.height),
              visible.width, visible.height);

    use(prog_tex, fb_size);
    const bool translucent = opacity < 1.0;
    if (translucent) {
        glUniform4f(prog_tex.param, static_cast<float>(opacity), 1.0f, 0.0f,
                    0.0f);
    }
    set_blend(src.format() == Pixmap::ARGB || translucent ? Blend::Straight
                                                          : Blend::None);
    for (const Tile& tile : tex.tiles) {
        glBindTexture(GL_TEXTURE_2D, tile.id);
        set_filter(min_filter, mag_filter);
        quad(static_cast<float>(pos.x + tile.area.x * scale),
             static_cast<float>(pos.y + tile.area.y * scale),
             static_cast<float>(tile.area.width * scale),
             static_cast<float>(tile.area.height * scale));
    }

    glDisable(GL_SCISSOR_TEST);
}

void GlCanvas::background(const Background mode, const Rectangle& preserve)
{
    const Rectangle full { 0, 0, fb_size.width, fb_size.height };
    const Rectangle ex = full.intersect(preserve);
    if (!ex) {
        fill(full, { argb_t::max, 0, 0, 0 });
        return;
    }

    const float fw = static_cast<float>(fb_size.width);
    const float fh = static_cast<float>(fb_size.height);
    const float ex_x = static_cast<float>(ex.x);
    const float ex_y = static_cast<float>(ex.y);
    const float ex_w = static_cast<float>(ex.width);
    const float ex_h = static_cast<float>(ex.height);

    // capture already drawn image from framebuffer (bottom-up texture)
    glBindTexture(GL_TEXTURE_2D, capture_tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ex.x,
                     static_cast<GLint>(fb_size.height) - ex.y -
                         static_cast<GLint>(ex.height),
                     ex.width, ex.height, 0);
    glGenerateMipmap(GL_TEXTURE_2D);
    set_filter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
    set_wrap(mode == Background::Mirror ? GL_MIRRORED_REPEAT
                                        : GL_CLAMP_TO_EDGE);

    // render background to downscaled target
    const Size blur_size = {
        std::max(fb_size.width / BLUR_DOWNSCALE, static_cast<size_t>(1)),
        std::max(fb_size.height / BLUR_DOWNSCALE, static_cast<size_t>(1)),
    };
    if (!resize_target(blur[0], blur_size) ||
        !resize_target(blur[1], blur_size)) {
        fill_inverse(ex, { argb_t::max, 0, 0, 0 });
        return;
    }

    bind_target(&blur[0]);
    use(prog_tex, fb_size); // window coordinates, scaled by viewport
    set_blend(Blend::None);
    glBindTexture(GL_TEXTURE_2D, capture_tex);
    if (mode == Background::Mirror) {
        quad(0, 0, fw, fh,
             { -ex_x / ex_w, 1.0f + ex_y / ex_h, (fw - ex_x) / ex_w,
               1.0f - (fh - ex_y) / ex_h });
    } else {
        const float scale = std::max(fw / ex_w, fh / ex_h);
        const float w = ex_w * scale;
        const float h = ex_h * scale;
        quad((fw - w) / 2, (fh - h) / 2, w, h, { 0.0f, 1.0f, 1.0f, 0.0f });
    }

    // two-pass gaussian blur
    const float sigma = BLUR_SIGMA / BLUR_DOWNSCALE;
    // normalized gaussian weights
    float weights[BLUR_RADIUS + 1];
    float wsum = 0.0f;
    for (int i = 0; i <= BLUR_RADIUS; ++i) {
        weights[i] = std::exp(-static_cast<float>(i * i) /
                              (2.0f * sigma * sigma));
        wsum += i ? weights[i] * 2.0f : weights[i];
    }
    for (float& w : weights) {
        w /= wsum;
    }
    const float bw = static_cast<float>(blur_size.width);
    const float bh = static_cast<float>(blur_size.height);
    for (size_t pass = 0; pass < 2; ++pass) {
        const Target& src = blur[pass];
        const Target& dst = blur[1 - pass];
        bind_target(&dst);
        use(prog_blur, dst.size);
        glUniform4f(prog_blur.param, pass == 0 ? 1.0f / bw : 0.0f,
                    pass == 0 ? 0.0f : 1.0f / bh, 0.0f, 0.0f);
        glUniform1fv(prog_blur.param2, BLUR_RADIUS + 1, weights);
        glBindTexture(GL_TEXTURE_2D, src.tex);
        quad(0, 0, bw, bh, { 0.0f, 1.0f, 1.0f, 0.0f });
    }

    // put blurred background around the image
    bind_output();
    use(prog_tex, fb_size);
    set_blend(Blend::None);
    glBindTexture(GL_TEXTURE_2D, blur[0].tex);
    const auto [top, bottom, left, right] = full.cutout(ex);
    for (const Rectangle* it : { &top, &bottom, &left, &right }) {
        if (!*it) {
            continue;
        }
        const float x = static_cast<float>(it->x);
        const float y = static_cast<float>(it->y);
        const float w = static_cast<float>(it->width);
        const float h = static_cast<float>(it->height);
        quad(x, y, w, h,
             { x / fw, 1.0f - y / fh, (x + w) / fw, 1.0f - (y + h) / fh });
    }
}

void GlCanvas::dim_outside(const Point& center, const size_t radius,
                           const double factor)
{
    use(prog_dim, fb_size);
    set_blend(Blend::Multiply);
    const float r = static_cast<float>(radius);
    glUniform4f(prog_dim.param, static_cast<float>(center.x) + 0.5f,
                static_cast<float>(center.y) + 0.5f, r * r,
                static_cast<float>(std::clamp(factor, 0.0, 1.0)));
    quad(0, 0, fb_size.width, fb_size.height);
}

void GlCanvas::raster(const std::function<void(Pixmap&)>& fn)
{
    if (raster_pm.width() != fb_size.width ||
        raster_pm.height() != fb_size.height) {
        raster_pm.create(Pixmap::ARGB, fb_size.width, fb_size.height);
    }
    std::memset(raster_pm.ptr(0, 0), 0,
                raster_pm.stride() * raster_pm.height());

    fn(raster_pm);

    // software renderers (cairo) produce premultiplied alpha
    glBindTexture(GL_TEXTURE_2D, raster_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,
                  static_cast<GLint>(raster_pm.stride() / sizeof(argb_t)));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, raster_pm.width(),
                 raster_pm.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 raster_pm.ptr(0, 0));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    set_filter(GL_NEAREST, GL_NEAREST);
    set_wrap(GL_CLAMP_TO_EDGE);

    use(prog_tex, fb_size);
    set_blend(Blend::Premult);
    quad(0, 0, fb_size.width, fb_size.height);
}

bool GlCanvas::draw_texture(const TextureRenderer& fn, const Size& src_size,
                            const double render_scale, const Point& pos,
                            const double scale, const Rectangle& clip)
{
    if (!src_size || scale <= 0.0) {
        return false;
    }

    const Rectangle full { 0, 0, fb_size.width, fb_size.height };
    const Rectangle visible = full.intersect(clip);
    if (!visible) {
        return false;
    }

    const double dst_w = src_size.width * scale;
    const double dst_h = src_size.height * scale;

    // render at the displayed size (external renderer scales the content
    // itself), limited to avoid huge textures at high zoom levels
    double tex_w = src_size.width * (render_scale > 0 ? render_scale : scale);
    double tex_h = src_size.height * (render_scale > 0 ? render_scale : scale);
    const double limit = std::min(
        static_cast<double>(max_tex_size),
        1.5 * std::max(fb_size.width, fb_size.height));
    const double over = std::max(tex_w, tex_h) / limit;
    if (over > 1.0) {
        tex_w /= over;
        tex_h /= over;
    }
    const Size tex_size {
        std::max(static_cast<size_t>(std::lround(tex_w)), static_cast<size_t>(1)),
        std::max(static_cast<size_t>(std::lround(tex_h)), static_cast<size_t>(1)),
    };

    // external renderers (libmpv) expect OpenGL state in standard defaults
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    blend_mode = Blend::None;

    const GLuint tex = fn(tex_size);
    reset_state();
    if (!tex) {
        return false;
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(visible.x,
              static_cast<GLint>(fb_size.height) - visible.y -
                  static_cast<GLint>(visible.height),
              visible.width, visible.height);
    use(prog_tex, fb_size);
    set_blend(Blend::None);
    glBindTexture(GL_TEXTURE_2D, tex);
    set_filter(GL_LINEAR, GL_LINEAR);
    quad(static_cast<float>(pos.x), static_cast<float>(pos.y),
         static_cast<float>(dst_w), static_cast<float>(dst_h),
         { 0.0f, 1.0f, 1.0f, 0.0f });
    glDisable(GL_SCISSOR_TEST);

    return true;
}

void GlCanvas::reset_state()
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    blend_mode = Blend::None;
    bind_output();
}

void GlCanvas::warmup()
{
    // GPU drivers finish compiling shaders on the first draw (with specific
    // texture formats), which may take hundreds of milliseconds: do it now,
    // not in the middle of an animation
    const Size size { 64, 64 };
    Pixmap argb;
    argb.create(Pixmap::ARGB, 32, 32);
    argb.fill({ 0, 0, 32, 32 }, { 0x80, 0x40, 0x80, 0xc0 });
    Pixmap rgb;
    rgb.create(Pixmap::RGB, 32, 32);
    rgb.fill({ 0, 0, 32, 32 }, { 0xff, 0x40, 0x80, 0xc0 });
    Pixmap gs;
    gs.create(Pixmap::GS, 16, 16);

    // draw to offscreen target only: touching the window buffer between
    // frames would make the driver allocate it with an outdated size
    Target warm;
    if (!resize_target(warm, size)) {
        return;
    }
    const Size prev_size = fb_size;
    fb_size = size;
    output = &warm;
    layer_active = false;
    bind_output();

    fill({ 0, 0, 64, 64 }, { 0xff, 0, 0, 0 });
    begin_layer();
    fill_blend({ 0, 0, 10, 10 }, { 0x80, 0xff, 0xff, 0xff });
    grid({ 0, 0, 32, 32 }, 4, { 0xff, 0x80, 0x80, 0x80 },
         { 0xff, 0x40, 0x40, 0x40 });
    draw(rgb, { 0, 0 }, 1.0);
    draw(argb, { 16, 16 }, 0.5);
    draw(rgb, { 8, 8 }, 0.7, { 0, 0, 64, 64 }, 0.5);
    blend(argb, { 4, 4 });
    mask(gs, { 2, 2 }, { 0xff, 0xff, 0xff, 0xff });
    background(Background::Mirror, { 16, 16, 32, 32 });
    background(Background::Extend, { 16, 16, 32, 32 });
    dim_outside({ 32, 32 }, 10, 0.5);
    end_layer(0.5);
    glFinish();
    free_target(warm);

    // repeat transition-like drawing at the window size: allocates layer
    // and blur targets, otherwise it happens on the first mode transition
    Target full;
    if (prev_size && resize_target(full, prev_size)) {
        const Rectangle win { 0, 0, prev_size.width, prev_size.height };
        const Rectangle center { static_cast<ssize_t>(prev_size.width / 4),
                                 static_cast<ssize_t>(prev_size.height / 4),
                                 prev_size.width / 2, prev_size.height / 2 };
        fb_size = prev_size;
        output = &full;
        layer_active = false;
        bind_output();
        fill(win, { 0xff, 0, 0, 0 });
        begin_layer();
        draw(rgb, { center.x, center.y }, 8.0);
        background(Background::Mirror, center);
        background(Background::Extend, center);
        end_layer(0.5);
        draw(rgb, { center.x, center.y }, 8.0, win, 0.5);
        glFinish();
        free_target(full);
    }

    output = nullptr;
    fb_size = prev_size;

    // drop temporary textures
    for (const Pixmap* pm : { &argb, &rgb, &gs }) {
        const auto it = textures.find(pm->id());
        if (it != textures.end()) {
            textures_bytes -= it->second.bytes;
            free_texture(it->second);
            textures.erase(it);
        }
    }
}

void GlCanvas::begin_layer()
{
    if (layer_active || !resize_target(layer, fb_size)) {
        return;
    }
    layer_active = true;
    bind_output();
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void GlCanvas::end_layer(const double opacity)
{
    if (!layer_active) {
        return;
    }
    layer_active = false;
    bind_output();

    const float alpha = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (alpha <= 0.0f) {
        return;
    }

    // layer has premultiplied alpha
    use(prog_tex, fb_size);
    glUniform4f(prog_tex.param, alpha, alpha, 0.0f, 0.0f);
    set_blend(Blend::Premult);
    glBindTexture(GL_TEXTURE_2D, layer.tex);
    set_filter(GL_NEAREST, GL_NEAREST);
    quad(0, 0, fb_size.width, fb_size.height, { 0.0f, 1.0f, 1.0f, 0.0f });
}

GLuint GlCanvas::compile(const char* fragment)
{
    const auto shader = [](const GLenum type, const char* src) -> GLuint {
        const GLuint id = glCreateShader(type);
        glShaderSource(id, 1, &src, nullptr);
        glCompileShader(id);
        GLint ok = 0;
        glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = { 0 };
            glGetShaderInfoLog(id, sizeof(log), nullptr, log);
            Log::error("Unable to compile shader: {}", log);
            glDeleteShader(id);
            return 0;
        }
        return id;
    };

    const GLuint vs = shader(GL_VERTEX_SHADER, VERTEX_SHADER);
    const GLuint fs = shader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        Log::error("Unable to link shader program: {}", log);
        glDeleteProgram(prog);
        return 0;
    }

    return prog;
}

GlCanvas::Program GlCanvas::make_program(const char* fragment)
{
    Program prog;
    prog.id = compile(fragment);
    if (prog.id) {
        prog.viewport = glGetUniformLocation(prog.id, "u_viewport");
        prog.color = glGetUniformLocation(prog.id, "u_color");
        prog.color2 = glGetUniformLocation(prog.id, "u_color2");
        prog.param = glGetUniformLocation(prog.id, "u_param");
        prog.param2 = glGetUniformLocation(prog.id, "u_param2");
        if (prog.param2 < 0) {
            prog.param2 = glGetUniformLocation(prog.id, "u_weights");
        }
    }
    return prog;
}

const GlCanvas::Texture& GlCanvas::get_texture(const Pixmap& pm,
                                               const bool mipmap)
{
    auto it = textures.find(pm.id());
    if (it == textures.end()) {
        Texture tex = create_texture(pm, true);
        tex.last_use = frame;
        textures_bytes += tex.bytes;
        it = textures.insert_or_assign(pm.id(), std::move(tex)).first;
    }

    Texture& tex = it->second;
    tex.last_use = frame;
    if (tex.pending) {
        upload_rows(pm, tex, 0); // not preloaded yet: finish now
        tex.pending = false;
    }
    if (mipmap && !tex.mipmap) {
        for (const Tile& tile : tex.tiles) {
            glBindTexture(GL_TEXTURE_2D, tile.id);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        tex.mipmap = true;
        textures_bytes += tex.bytes / 3;
        tex.bytes += tex.bytes / 3;
    }
    return tex;
}

void GlCanvas::enable_async_upload(const std::function<bool()>& bind,
                                   const std::function<void()>& unbind)
{
    up_bind = bind;
    up_unbind = unbind;
    up_thread = std::thread(&GlCanvas::upload_worker, this);
}

void GlCanvas::upload_worker()
{
    // background work: don't compete with the render thread
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);

    if (!up_bind()) {
        Log::verbose("GL: unable to bind upload context");
        up_failed = true;
        return;
    }

    while (true) {
        std::shared_ptr<UploadJob> job;
        {
            std::unique_lock lock(up_mutex);
            up_cv.wait(lock, [this] {
                return up_stop || !up_queue.empty();
            });
            if (up_stop) {
                break;
            }
            job = up_queue.front();
            up_queue.pop_front();
        }

        if (job->level) {
            // first stage: resolution needed for the current view
            const Pixmap small = downscale(*job->pm, job->level);
            auto [tex, fence] = upload_async(small, true);
            // texture covers the area of the origin pixmap
            for (Tile& tile : tex.tiles) {
                tile.area = Rectangle(0, 0, job->pm->width(),
                                      job->pm->height());
            }
            tex.reduced = true;
            const std::scoped_lock lock(up_mutex);
            job->reduced = std::move(tex);
            job->reduced_fence = fence;
            job->reduced_done = true;
        }

        // full resolution (e.g. for zoom), not urgent if the reduced
        // texture is already uploaded
        auto [tex, fence] = upload_async(*job->pm, job->level == 0);
        const std::scoped_lock lock(up_mutex);
        job->tex = std::move(tex);
        job->fence = fence;
        job->owner.reset(); // pixel data has been copied by GL
        job->done = true;
    }

    up_unbind();
}

std::pair<GlCanvas::Texture, GLsync>
GlCanvas::upload_async(const Pixmap& pm, const bool urgent) const
{
    // allocate, upload and generate mipmaps: all the slow parts
    Texture tex = create_texture(pm, false);
    // upload band by band: GL holds a lock shared with the main context
    // during each call, so pause to let the main thread draw frames;
    // non-urgent uploads pause longer while animation frames are drawn
    while (!upload_rows(pm, tex, 1e-9)) {
        bool animating = false;
        if (!urgent) {
            const auto now =
                std::chrono::steady_clock::now().time_since_epoch().count();
            const auto idle = std::chrono::steady_clock::duration(
                now - last_frame_time.load(std::memory_order_relaxed));
            animating = idle < std::chrono::milliseconds(50);
        }
        std::this_thread::sleep_for(
            std::chrono::microseconds(animating ? 3000 : (urgent ? 1000 : 300)));
    }
    for (const Tile& tile : tex.tiles) {
        glBindTexture(GL_TEXTURE_2D, tile.id);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    tex.mipmap = true;
    tex.bytes += tex.bytes / 3;
    glBindTexture(GL_TEXTURE_2D, 0);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    return { std::move(tex), fence };
}

bool GlCanvas::collect_uploads()
{
    if (up_failed) {
        // drop jobs that will never be processed
        const std::scoped_lock lock(up_mutex);
        up_queue.clear();
        up_jobs.clear();
        return false;
    }

    // GPU must finish the upload before the texture is used here
    const auto signaled = [](GLsync fence) {
        const GLenum rc = glClientWaitSync(fence, 0, 0);
        return rc == GL_ALREADY_SIGNALED || rc == GL_CONDITION_SATISFIED;
    };

    const std::scoped_lock lock(up_mutex);
    for (auto it = up_jobs.begin(); it != up_jobs.end();) {
        UploadJob& job = *it->second;
        const uint64_t id = it->first;

        if (job.reduced_done && !job.reduced_taken &&
            signaled(job.reduced_fence)) {
            glDeleteSync(job.reduced_fence);
            job.reduced_taken = true;
            if (textures.contains(id)) {
                free_texture(job.reduced); // already created on demand
            } else {
                job.reduced.last_use = frame;
                textures_bytes += job.reduced.bytes;
                textures.insert_or_assign(id, std::move(job.reduced));
            }
        }

        if (!job.done || (job.level && !job.reduced_taken) ||
            !signaled(job.fence)) {
            ++it;
            continue;
        }
        glDeleteSync(job.fence);
        const auto cached = textures.find(id);
        if (cached != textures.end() && !cached->second.reduced) {
            free_texture(job.tex); // already created on demand
        } else {
            if (cached != textures.end()) {
                // replace reduced texture with the full one
                job.tex.last_use = cached->second.last_use;
                textures_bytes -= cached->second.bytes;
                free_texture(cached->second);
            } else {
                job.tex.last_use = frame;
            }
            textures_bytes += job.tex.bytes;
            textures.insert_or_assign(id, std::move(job.tex));
        }
        it = up_jobs.erase(it);
    }
    return true;
}

bool GlCanvas::preload(const Pixmap& pm,
                       const std::shared_ptr<const void>& owner,
                       const double budget, const double scale)
{
    if (up_thread.joinable() && collect_uploads()) {
        const auto it = textures.find(pm.id());
        if (it != textures.end() && !it->second.pending) {
            it->second.last_use = frame;
            if (!it->second.mipmap) {
                get_texture(pm, true);
            }
            return true;
        }
        if (!up_jobs.contains(pm.id())) {
            auto job = std::make_shared<UploadJob>();
            job->pm = &pm;
            job->owner = owner;
            // first upload the largest downscaled level that is still not
            // smaller than the drawn image: much less data to transfer
            const size_t max_size = static_cast<size_t>(max_tex_size);
            if (scale > 0 && pm.width() <= max_size &&
                pm.height() <= max_size && pm.format() != Pixmap::GS) {
                while (job->level < 4 &&
                       1.0 / static_cast<double>(1 << (job->level + 1)) >=
                           scale) {
                    ++job->level;
                }
            }
            {
                const std::scoped_lock lock(up_mutex);
                up_queue.push_back(job);
            }
            up_jobs.emplace(pm.id(), job);
            up_cv.notify_one();
        }
        return false;
    }

    // fallback: upload in small steps within the time budget
    auto it = textures.find(pm.id());
    if (it == textures.end()) {
        // allocate textures now, fill them in the following steps
        Texture tex = create_texture(pm, false);
        tex.last_use = frame;
        tex.pending = true;
        textures_bytes += tex.bytes;
        it = textures.insert_or_assign(pm.id(), std::move(tex)).first;
        return false; // allocation is a step itself
    }

    Texture& tex = it->second;
    tex.last_use = frame; // protect from eviction

    if (tex.pending) {
        if (upload_rows(pm, tex, budget)) {
            tex.pending = false;
        }
        return false; // mipmaps are generated in a separate step
    }

    if (!tex.mipmap) {
        // the viewer shows large images downscaled: generate mipmaps
        // in advance to avoid it on the first draw
        get_texture(pm, true);
    }

    return true;
}

GlCanvas::Texture GlCanvas::create_texture(const Pixmap& pm,
                                           const bool upload) const
{
    Texture tex;

    const bool gs = pm.format() == Pixmap::GS;
    const size_t bpp = gs ? 1 : sizeof(argb_t);
    const size_t max_size = static_cast<size_t>(max_tex_size);

    glPixelStorei(GL_UNPACK_ALIGNMENT, gs ? 1 : 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pm.stride() / bpp));

    for (size_t y = 0; y < pm.height(); y += max_size) {
        for (size_t x = 0; x < pm.width(); x += max_size) {
            const size_t w = std::min(max_size, pm.width() - x);
            const size_t h = std::min(max_size, pm.height() - y);

            Tile tile { 0, Rectangle(x, y, w, h) };
            glGenTextures(1, &tile.id);
            glBindTexture(GL_TEXTURE_2D, tile.id);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, static_cast<GLint>(x));
            glPixelStorei(GL_UNPACK_SKIP_ROWS, static_cast<GLint>(y));

            const GLsizei tw = static_cast<GLsizei>(w);
            const GLsizei th = static_cast<GLsizei>(h);
            if (!upload) {
                // immutable storage with all mip levels, filled later: cheaper
                // to allocate and mipmaps are generated without reallocation
                GLsizei levels = 1;
                for (size_t sz = std::max(w, h); sz > 1; sz >>= 1) {
                    ++levels;
                }
                glTexStorage2D(GL_TEXTURE_2D, levels, gs ? GL_R8 : GL_RGBA8,
                               tw, th);
            } else if (gs) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, tw, th, 0, GL_RED,
                             GL_UNSIGNED_BYTE, pm.ptr(0, 0));
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, pm.ptr(0, 0));
            }
            if (!gs) {
                // pixmap is BGRA in memory, swap channels on sampling
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
                if (pm.format() == Pixmap::RGB) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
                                    GL_ONE);
                }
            }
            set_wrap(GL_CLAMP_TO_EDGE);

            tex.bytes += w * h * bpp;
            tex.tiles.push_back(tile);
        }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    return tex;
}

bool GlCanvas::upload_rows(const Pixmap& pm, Texture& tex, const double budget)
{
    // rows uploaded at once: small enough to keep the time budget
    constexpr size_t band_bytes = 2 * 1024 * 1024;

    const auto start = std::chrono::steady_clock::now();
    const bool gs = pm.format() == Pixmap::GS;
    const size_t bpp = gs ? 1 : sizeof(argb_t);

    glPixelStorei(GL_UNPACK_ALIGNMENT, gs ? 1 : 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pm.stride() / bpp));

    bool complete = false;
    while (true) {
        if (tex.up_tile >= tex.tiles.size()) {
            complete = true;
            break;
        }
        const Tile& tile = tex.tiles[tex.up_tile];
        const size_t band = std::max<size_t>(
            1, band_bytes / (static_cast<size_t>(tile.area.width) * bpp));
        const size_t rows = std::min(band, tile.area.height - tex.up_row);

        glBindTexture(GL_TEXTURE_2D, tile.id);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, static_cast<GLint>(tile.area.x));
        glPixelStorei(GL_UNPACK_SKIP_ROWS,
                      static_cast<GLint>(tile.area.y + tex.up_row));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(tex.up_row),
                        static_cast<GLsizei>(tile.area.width),
                        static_cast<GLsizei>(rows), gs ? GL_RED : GL_RGBA,
                        GL_UNSIGNED_BYTE, pm.ptr(0, 0));

        tex.up_row += rows;
        if (tex.up_row >= tile.area.height) {
            ++tex.up_tile;
            tex.up_row = 0;
        }

        if (budget > 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          start)
                    .count() >= budget) {
            complete = tex.up_tile >= tex.tiles.size();
            break;
        }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    return complete;
}

void GlCanvas::free_texture(Texture& tex)
{
    for (const Tile& tile : tex.tiles) {
        glDeleteTextures(1, &tile.id);
    }
    tex.tiles.clear();
}

void GlCanvas::set_blend(const Blend mode)
{
    if (mode == blend_mode) {
        return;
    }
    blend_mode = mode;

    switch (mode) {
        case Blend::None:
            glDisable(GL_BLEND);
            break;
        case Blend::Straight:
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                GL_ONE_MINUS_SRC_ALPHA);
            break;
        case Blend::Premult:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case Blend::Multiply:
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
            break;
    }
}

void GlCanvas::use(const Program& prog, const Size& viewport)
{
    glUseProgram(prog.id);
    glUniform2f(prog.viewport, static_cast<float>(viewport.width),
                static_cast<float>(viewport.height));
    if (prog.id == prog_tex.id) {
        glUniform4f(prog_tex.param, 1.0f, 1.0f, 0.0f, 0.0f); // opaque
    }
}

void GlCanvas::quad(const float x, const float y, const float w,
                    const float h, const UvRect& uv)
{
    const float vertices[] = {
        x,     y,     uv.u0, uv.v0, // top-left
        x,     y + h, uv.u0, uv.v1, // bottom-left
        x + w, y,     uv.u1, uv.v0, // top-right
        x + w, y + h, uv.u1, uv.v1, // bottom-right
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GlCanvas::solid(const Rectangle& rect, const argb_t& color,
                     const Blend mode)
{
    if (!rect) {
        return;
    }
    use(prog_solid, fb_size);
    set_blend(mode);
    set_uniform(prog_solid.color, color);
    quad(rect.x, rect.y, rect.width, rect.height);
}

void GlCanvas::bind_output()
{
    bind_target(layer_active ? &layer : output);
}

void GlCanvas::bind_target(const Target* target)
{
    if (target) {
        glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
        glViewport(0, 0, target->size.width, target->size.height);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fb_size.width, fb_size.height);
    }
}

bool GlCanvas::resize_target(Target& target, const Size& size)
{
    if (target.fbo && target.size.width == size.width &&
        target.size.height == size.height) {
        return true;
    }

    if (!target.fbo) {
        glGenFramebuffers(1, &target.fbo);
        glGenTextures(1, &target.tex);
    }

    glBindTexture(GL_TEXTURE_2D, target.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width, size.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    set_filter(GL_LINEAR, GL_LINEAR);
    set_wrap(GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target.tex, 0);
    const bool ok =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    target.size = size;
    if (!ok) {
        Log::error("Unable to create GL framebuffer");
    }
    return ok;
}

void GlCanvas::free_target(Target& target)
{
    if (target.fbo) {
        glDeleteFramebuffers(1, &target.fbo);
        glDeleteTextures(1, &target.tex);
        target.fbo = 0;
        target.tex = 0;
    }
}
