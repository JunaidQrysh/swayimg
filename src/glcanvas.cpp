// SPDX-License-Identifier: MIT
// GPU canvas: draws using OpenGL ES 3.0.

#include "glcanvas.hpp"

#include "log.hpp"
#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

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

GlCanvas::~GlCanvas()
{
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

    output = nullptr;
    fb_size = prev_size;
    free_target(warm);

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
    const auto it = textures.find(pm.id());
    if (it != textures.end()) {
        Texture& tex = it->second;
        tex.last_use = frame;
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

    Texture tex;
    tex.last_use = frame;
    tex.mipmap = mipmap;

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

            if (gs) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED,
                             GL_UNSIGNED_BYTE, pm.ptr(0, 0));
            } else {
                // pixmap is BGRA in memory, swap channels on sampling
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, pm.ptr(0, 0));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
                if (pm.format() == Pixmap::RGB) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
                                    GL_ONE);
                }
            }
            set_wrap(GL_CLAMP_TO_EDGE);
            if (mipmap) {
                glGenerateMipmap(GL_TEXTURE_2D);
            }

            tex.bytes += w * h * bpp;
            tex.tiles.push_back(tile);
        }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    if (mipmap) {
        tex.bytes += tex.bytes / 3;
    }
    textures_bytes += tex.bytes;

    return textures.insert_or_assign(pm.id(), std::move(tex)).first->second;
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
