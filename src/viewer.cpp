// SPDX-License-Identifier: MIT
// Image view mode.
// Copyright (C) 2020 Artem Senichev <artemsen@gmail.com>

#include "viewer.hpp"

#include "application.hpp"
#include "defaults.hpp"
#include "formatfactory.hpp"
#include "log.hpp"
#include "render.hpp"
#include "resources.hpp"
#include "text.hpp"
#include "embeddedvideo.hpp"
#include "fdothumb.hpp"
#include "lazylib.hpp"
#include "videoplayer.hpp"
#include "buildconf.hpp"
#ifdef HAVE_HWDECODE
#include "hwdecode.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include <sys/resource.h>
#include <unistd.h>

namespace {

/** Format name of the video placeholder taken from the thumbnail cache. */
constexpr const char* VIDEO_POSTER = "Video";

// Duration of zoom/pan transition in seconds
constexpr double VIEW_ANIM_TIME = 0.2;
// Duration of gallery<->viewer transition in seconds
constexpr double MODE_ANIM_TIME = 0.28;

// Touchpad panning speed (relative to finger movement) and wheel step
constexpr double FINGER_PAN_FACTOR = 16.0 * 1.5;
constexpr double WHEEL_PAN_STEP = 50.0;

// Kinetic panning
constexpr double PAN_FRICTION = 4.0;
constexpr double PAN_MIN_VELOCITY = 300.0;
constexpr double PAN_MAX_VELOCITY = 8000.0;
constexpr double PAN_STOP_VELOCITY = 15.0;

/** Ease out cubic. */
double ease_out(const double t)
{
    const double inv = 1.0 - std::clamp(t, 0.0, 1.0);
    return 1.0 - inv * inv * inv;
}

} // namespace

/** Max scale factor. */
constexpr double MAX_SCALE = 100.0;

/**
 * Lower CPU priority of the calling thread (and threads it creates), so
 * background loading doesn't delay drawing of animation frames.
 */
static void lower_thread_priority()
{
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
}

Viewer& Viewer::self()
{
    static Viewer singleton;
    return singleton;
}

Viewer::Viewer()
    : imagelist_loop(Defaults::viewer::imagelist_loop)
    , auto_pos(Defaults::viewer::auto_pos)
    , default_scale(Defaults::viewer::scale)
    , default_pos(Defaults::viewer::position)
    , window_bkg(Defaults::viewer::window_bkg)
    , tr_chessboard(Defaults::viewer::tr_chessboard)
    , tr_cbsize(Defaults::viewer::tr_cbsize)
    , tr_cbcolor { Defaults::viewer::tr_cbcolor0,
                   Defaults::viewer::tr_cbcolor1, }
    , tr_bgcolor(Defaults::viewer::tr_bgcolor)
    , animation(Defaults::viewer::animation)
    , focus_radius(Defaults::viewer::focus_radius)
    , focus_factor(Defaults::viewer::focus_factor)
{
    image_pool.preload.capacity = Defaults::viewer::preload;
    image_pool.history.capacity = Defaults::viewer::history;

    mark_color = Defaults::viewer::mark_color;
    pinch_factor = Defaults::viewer::pinch_factor;

    text_scheme[static_cast<size_t>(Text::TopLeft)].assign(
        Defaults::viewer::text_scheme_tl.begin(),
        Defaults::viewer::text_scheme_tl.end());
    text_scheme[static_cast<size_t>(Text::TopRight)].assign(
        Defaults::viewer::text_scheme_tr.begin(),
        Defaults::viewer::text_scheme_tr.end());
    text_scheme[static_cast<size_t>(Text::BottomLeft)].assign(
        Defaults::viewer::text_scheme_bl.begin(),
        Defaults::viewer::text_scheme_bl.end());

    Defaults::viewer::bind_inputs(this);
}

Viewer::~Viewer() = default;

bool Viewer::open(const ImageList::Dir dir)
{
    ImageList& il = ImageList::self();

    ImageEntryPtr next;
    if (image) {
        next = il.get(image->entry, dir);
    } else {
        next = il.get(nullptr, ImageList::Dir::First);
    }

    const bool forward = dir != ImageList::Dir::First &&
        dir != ImageList::Dir::Prev && dir != ImageList::Dir::PrevParent;

    while (true) {
        if (!next && imagelist_loop) {
            // reshuffle random on new loop
            if (il.get_order() == ImageList::Order::Random) {
                il.set_order(ImageList::Order::Random);
            }

            // start new loop
            next =
                il.get(nullptr,
                       forward ? ImageList::Dir::First : ImageList::Dir::Last);

            // avoid opening the same image in random mode
            if (next && image && next == image->entry &&
                il.get_order() == ImageList::Order::Random) {
                next = il.get(next, ImageList::Dir::Next);
            }
        }

        if (image && next == image->entry) {
            next = nullptr;
            break; // same image
        }
        if (!next || set_current(next)) {
            break;
        }

        const ImageEntryPtr remove = next;
        next =
            il.get(next, forward ? ImageList::Dir::Next : ImageList::Dir::Prev);
        il.remove(remove);
    }

    return !!next;
}

bool Viewer::reload()
{
    bool reloaded = false;
    const ImageEntryPtr entry = image ? image->entry : nullptr;

    if (entry && !entry->removed) {
        reloaded = set_current(entry);
    }
    if (!reloaded) {
        reloaded = open(ImageList::Dir::Next) || open(ImageList::Dir::Prev);
        if (entry && reloaded) {
            // remove entry from cache in reloading mode
            const std::scoped_lock lock(image_pool.mutex);
            image_pool.history.get(entry);
        }
    }
    if (!reloaded) {
        set_image(nullptr);
    }

    return reloaded;
}

void Viewer::export_frame(const std::filesystem::path& path) const
{
    if (image) {
        const Pixmap& pm = image->frames[frame_index].pm;
        if (!FormatFactory::save(pm, {}, path)) {
            Text::self().set_status("Failed to export image");
        }
    }
}

void Viewer::set_scale(const Scale sc)
{
    if (!image) {
        return;
    }

    const Pixmap& pm = image->frames[frame_index].pm;
    const double ratio_w = static_cast<double>(window_size.width) / pm.width();
    const double ratio_h =
        static_cast<double>(window_size.height) / pm.height();

    // 100% of the full image (placeholder thumbnail is smaller)
    const double real = pixel_ratio();

    double abs_sc = real;
    switch (sc) {
        case Scale::Keep:
        case Scale::Optimal:
            abs_sc = std::min({ real, ratio_w, ratio_h });
            break;
        case Scale::FitWindow:
            abs_sc = std::min(ratio_w, ratio_h);
            break;
        case Scale::FitWidth:
            abs_sc = ratio_w;
            break;
        case Scale::FitHeight:
            abs_sc = ratio_h;
            break;
        case Scale::FillWindow:
            abs_sc = std::max(ratio_w, ratio_h);
            break;
        case Scale::RealSize:
            abs_sc = real; // 100 %
            break;
    }

    set_scale(abs_sc);
}

void Viewer::set_scale(const double sc, const Point& preserve)
{
    if (!image) {
        return;
    }

    double new_scale = sc;

    // check scale limits
    const Pixmap& pm = image->frames[frame_index].pm;
    const Size scaled = static_cast<Size>(pm) * new_scale;
    if (!scaled) {
        return;
    }
    if (sc > MAX_SCALE * pixel_ratio()) {
        new_scale = MAX_SCALE * pixel_ratio();
    }

    // save base point of the image
    const Point bpt(preserve ? preserve.x : window_size.width / 2,
                    preserve ? preserve.y : window_size.height / 2);
    const double pr_x = (static_cast<double>(bpt.x) - position.x) / scale;
    const double pr_y = (static_cast<double>(bpt.y) - position.y) / scale;

    // set new scale
    scale = new_scale;

    // restore center
    position.x = bpt.x - pr_x * scale;
    position.y = bpt.y - pr_y * scale;

    update_text(TextUpdate::Scale);

    fixup_position();
}

void Viewer::reset()
{
    if (image) {
        if (std::holds_alternative<Scale>(default_scale)) {
            set_scale(std::get<Scale>(default_scale));
        } else {
            set_scale(std::get<double>(default_scale));
        }
        set_position(default_pos);
    }
}

void Viewer::set_position(const Position pos)
{
    if (!image) {
        return;
    }

    const Pixmap& pm = image->frames[frame_index].pm;
    const Size scaled = static_cast<Size>(pm) * scale;

    switch (pos) {
        case Position::Center:
            position.x = window_size.width / 2 - scaled.width / 2;
            position.y = window_size.height / 2 - scaled.height / 2;
            break;
        case Position::TopCenter:
            position.x = window_size.width / 2 - scaled.width / 2;
            position.y = 0;
            break;
        case Position::BottomCenter:
            position.x = window_size.width / 2 - scaled.width / 2;
            position.y = window_size.height - scaled.height;
            break;
        case Position::LeftCenter:
            position.x = 0;
            position.y = window_size.height / 2 - scaled.height / 2;
            break;
        case Position::RightCenter:
            position.x = window_size.width - scaled.width;
            position.y = window_size.height / 2 - scaled.height / 2;
            break;
        case Position::TopLeft:
            position.x = 0;
            position.y = 0;
            break;
        case Position::TopRight:
            position.x = window_size.width - scaled.width;
            position.y = 0;
            break;
        case Position::BottomLeft:
            position.x = 0;
            position.y = window_size.height - scaled.height;
            break;
        case Position::BottomRight:
            position.x = window_size.width - scaled.width;
            position.y = window_size.height - scaled.height;
            break;
    }

    Application::redraw();
}

void Viewer::set_position(const Point& pos)
{
    if (image && pos != position) {
        position = pos;
        fixup_position();
    }
}

void Viewer::flip_vertical()
{
    if (image) {
        image->flip_vertical();
        Application::redraw();
    }
}

void Viewer::flip_horizontal()
{
    if (image) {
        image->flip_horizontal();
        Application::redraw();
    }
}

void Viewer::rotate(const size_t angle)
{
    assert(angle == 90 || angle == 180 || angle == 270);

    if (image) {
        image->rotate(angle);
        snap_view();

        const Pixmap& pm = image->frames[frame_index].pm;
        const ssize_t diff = static_cast<ssize_t>(pm.width()) -
            static_cast<ssize_t>(pm.height());
        const ssize_t shift = (scale * diff) / 2;
        position.x -= shift;
        position.y += shift;

        update_text(TextUpdate::Frame);

        fixup_position();
    }
}

void Viewer::enable_animation(const bool enable)
{
    if (image) {
        const size_t duration = image->frames[frame_index].duration;
        animation = enable && image->frames.size() > 1 && duration;
        animation_timer.reset(enable ? duration : 0, 0);
    }
}

void Viewer::set_window_background(const argb_t& color)
{
    window_bkg = color;
    if (is_active()) {
        Application::redraw();
    }
}

void Viewer::set_window_background(const Background mode)
{
    window_bkg = mode;
    if (is_active()) {
        Application::redraw();
    }
}

void Viewer::set_image_background(const argb_t& color)
{
    tr_chessboard = false;
    tr_bgcolor = color;
    if (is_active()) {
        Application::redraw();
    }
}

void Viewer::set_image_chessboard(const size_t size, const argb_t& color1,
                                  const argb_t& color2)
{
    tr_chessboard = true;
    tr_cbsize = size;
    tr_cbcolor[0] = color1;
    tr_cbcolor[1] = color2;
    if (is_active()) {
        Application::redraw();
    }
}

void Viewer::set_focus_radius(const size_t radius)
{
    focus_radius =
        std::clamp(radius, static_cast<size_t>(0), static_cast<size_t>(1000));
    if (is_active()) {
        Application::redraw();
    }
}

void Viewer::set_focus_factor(const double factor)
{
    focus_factor = std::clamp(factor, 0.0, 1.0);
    if (is_active()) {
        Application::redraw();
    }
}

void Viewer::set_preload_limit(const size_t size)
{
    image_pool.preload.capacity = size;
}

void Viewer::set_history_limit(const size_t size)
{
    image_pool.history.capacity = size;
}

void Viewer::bind_image_drag(const InputMouse& input)
{
    drag = input;
    bind_input(drag, [] {
        Application::get_ui()->set_cursor(Ui::CursorShape::Drag);
    });
}

// void Viewer::set_focus(const bool enable)
// {
//     focus = enable;
//     if (is_active()) {
//         Application::redraw();
//     }
// }

void Viewer::initialize()
{
    Application::self().add_fdpoll(loaded_notify, [this] {
        handle_loaded();
    });

    Application::self().add_fdpoll(animation_timer, [this] {
        if (!image) {
            animation_timer.reset(0, 0); // the last image has gone, stop timer
            return;
        }
        size_t index = frame_index + 1;
        if (index >= image->frames.size()) {
            index = 0;
        }
        set_frame(index);
        enable_animation(true);
    });
}

void Viewer::activate(const ImageEntryPtr& entry, const Size& wnd)
{
    AppMode::activate(entry, wnd);

    window_size = wnd;

    if (image && image->format == VIDEO_POSTER) {
        // video poster is not reused: thumbnail may have been regenerated
        // while the viewer was inactive, it is cheap to get it again
        image = nullptr;
    }

    if (entry && image && image->entry == entry && !entry->removed) {
        set_image(image); // reinit state without reloading image
        if (this == &Viewer::self() && VideoPlayer::is_video(entry)) {
            start_video(entry);
        }
    } else if (!entry ||
               (!set_current(entry) && !open(ImageList::Dir::First))) {
        set_image(nullptr);
    }
}

void Viewer::deactivate()
{
    preloader_stop();
    stop_video();

    // restore cursor and content type
    Ui* ui = Application::get_ui();
    ui->set_ctype(Ui::ContentType::Static);
    ui->set_cursor(Ui::CursorShape::Default);
}

ImageEntryPtr Viewer::get_current()
{
    return image ? image->entry : nullptr;
}

bool Viewer::set_current(const ImageEntryPtr& entry)
{
    assert(entry && !entry->removed);

    ImagePtr new_image = nullptr;

    if (image && image->entry == entry) {
        // remove entry from cache in reloading mode
        const std::scoped_lock lock(image_pool.mutex);
        image_pool.history.get(entry);
        image_pool.preload.get(entry);
    } else {
        // get file from history/preload cache
        const std::scoped_lock lock(image_pool.mutex);
        new_image = image_pool.preload.get(entry);
        if (new_image) {
            Log::verbose("Got image {} from preloading cache",
                         entry->path.filename().string());
        } else {
            new_image = image_pool.history.get(entry);
            if (new_image) {
                Log::verbose("Got image {} from history cache",
                             entry->path.filename().string());
            }
        }
    }

    if (!new_image && this == &Viewer::self() &&
        VideoPlayer::is_video(entry) && EmbeddedVideo::available()) {
        // video placeholder from thumbnail cache: much faster than decoding
        // a frame, and the video replaces it as soon as playback starts
        Pixmap thumb = FdoThumb::load(entry->path);
        if (thumb) {
            new_image = std::make_shared<Image>();
            new_image->frames.resize(1);
            new_image->frames[0].pm = std::move(thumb);
            new_image->format = VIDEO_POSTER;
            new_image->entry = entry;
        }
    }

    if (!new_image && this == &Viewer::self() && !entry->is_special()) {
        // show thumbnail immediately, full image is decoded in background
        new_image = load_progressive(entry);
    }
    if (!new_image) {
        new_image = FormatFactory::self().load(entry);
    }
    if (new_image) {
        // play videos (not in slideshow mode)
        const bool changed = !image || image->entry != entry;
        set_image(new_image);
        if (changed && this == &Viewer::self() &&
            VideoPlayer::is_video(entry)) {
            start_video(entry);
        }
    }

    return !!new_image;
}

void Viewer::window_resize(const Size& wnd)
{
    window_size = wnd;
    snap_view();
    if (image) {
        if (std::holds_alternative<Scale>(default_scale) &&
            std::get<Scale>(default_scale) == Scale::Keep) {
            fixup_position();
        } else {
            reset();
        }
    }
}

void Viewer::window_redraw(Canvas& wnd)
{
    if (placeholder.full) {
        // max time per frame to spend on preparing the full image
        constexpr double preload_budget = 0.004;
        if (!is_placeholder() || placeholder.full->entry != image->entry) {
            placeholder.full = nullptr;
        } else if (wnd.preload(placeholder.full->frames[0].pm,
                               placeholder.full, preload_budget,
                               scale / placeholder.ratio)) {
            show_full();
        } else {
            Application::redraw(); // continue in the next frame
        }
    }

    if (!image) {
        const argb_t* bkg = std::get_if<argb_t>(&window_bkg);
        draw_empty(wnd, bkg ? *bkg : Defaults::viewer::window_bkg);
        return;
    }

    const Pixmap& pm = image->frames[frame_index].pm;
    const double vscale = view_scale > 0 ? view_scale : scale;
    const Point vpos = view_scale > 0
        ? Point { .x = static_cast<ssize_t>(std::lround(view_x)),
                  .y = static_cast<ssize_t>(std::lround(view_y)) }
        : position;
    const Rectangle imgr = { vpos, static_cast<Size>(pm) * vscale };

    // clear image background
    if (pm.format() == Pixmap::ARGB) {
        if (tr_chessboard) {
            wnd.grid(imgr, tr_cbsize, tr_cbcolor[0], tr_cbcolor[1]);
        } else {
            wnd.fill(imgr, tr_bgcolor);
        }
    }

    // put image on window surface
    image->draw(frame_index, wnd, vscale, vpos.x, vpos.y);

    // fill window background
    if (std::holds_alternative<argb_t>(window_bkg)) {
        wnd.fill_inverse(imgr, std::get<argb_t>(window_bkg));
    } else {
        switch (std::get<Background>(window_bkg)) {
            case Background::Mirror:
                wnd.background(Canvas::Background::Mirror, imgr);
                break;
            case Background::Extend:
                wnd.background(Canvas::Background::Extend, imgr);
                break;
            case Background::Auto:
                wnd.background(imgr.width > imgr.height
                                   ? Canvas::Background::Mirror
                                   : Canvas::Background::Extend,
                               imgr);
                break;
        }
    }

    // video player output (frame on top of the still preview frame and
    // on-screen controller) covers the whole window
    if (video_active()) {
        const double vx = static_cast<double>(vpos.x);
        const double vy = static_cast<double>(vpos.y);
        const double vw = pm.width() * vscale;
        const double vh = pm.height() * vscale;
        wnd.draw_texture([this, vx, vy, vw, vh](const Size& size) {
            return video->render(size, vx, vy, vw, vh);
        });
    }

    // mark icon
    if (image->entry->mark) {
        const ssize_t margin = 10;
        const ssize_t x = static_cast<ssize_t>(wnd.size().width) -
            static_cast<ssize_t>(Resource::mark.width()) - margin;
        const ssize_t y = static_cast<ssize_t>(wnd.size().height) -
            static_cast<ssize_t>(Resource::mark.height()) - margin;
        wnd.mask(Resource::mark, { .x = x, .y = y }, mark_color);
    }

    // focus mode: dim everything except a circle around the pointer
    if (focus_radius) {
        const Point pointer = Application::get_ui()->get_mouse();
        if (pointer) {
            wnd.dim_outside(pointer, focus_radius, focus_factor);
        }
    }
}

void Viewer::handle_mmove(const InputMouse& input, const Point& pos,
                          const Point& delta)
{
    Point new_pos = position;

    if (auto_pos == AutoPos::Follow) {
        const Pixmap& pm = image->frames[frame_index].pm;
        const Size scaled = static_cast<Size>(pm) * scale;

        // pointer on window (relative position)
        double pt_x = static_cast<double>(pos.x) / window_size.width;
        double pt_y = static_cast<double>(pos.y) / window_size.height;
        constexpr double margin = 0.05; // margin 5%
        pt_x = pt_x * (margin * 2 + 1) - margin;
        pt_y = pt_y * (margin * 2 + 1) - margin;

        // change position
        if (scaled.width > window_size.width) {
            new_pos.x = -pt_x * (scaled.width - window_size.width);
        }
        if (scaled.height > window_size.height) {
            new_pos.y = -pt_y * (scaled.height - window_size.height);
        }
    } else if (drag && drag == input) {
        new_pos += delta;
        stop_pan();
    }

    if (new_pos != position) {
        snap_view();
    }
    set_position(new_pos);

    // show video player's on-screen controller
    if (video_active() && pos) {
        video->mouse_move(pos.x, pos.y);
    }

    if (focus_radius) {
        // force redraw even if image position was not changed
        Application::redraw();
    }
}

void Viewer::handle_pinch(const double scale_delta, const double dx,
                          const double dy)
{
    if (!image) {
        return;
    }

    // the image follows fingers without limits until the gesture ends
    if (!pinching) {
        pinching = true;
        pinch_rx = 0;
        pinch_ry = 0;
    }
    stop_pan();
    snap_view();

    // two-finger movement during the pinch pans the image
    pinch_rx += dx;
    pinch_ry += dy;
    const ssize_t mx = static_cast<ssize_t>(pinch_rx);
    const ssize_t my = static_cast<ssize_t>(pinch_ry);
    pinch_rx -= mx;
    pinch_ry -= my;
    position.x += mx;
    position.y += my;

    set_scale(scale + scale_delta * scale * pinch_factor,
              Application::get_ui()->get_mouse());
}

void Viewer::handle_pinch_end()
{
    if (!pinching) {
        return;
    }
    pinching = false;
    if (image) {
        // settle into valid position with animation
        view_snap = false;
        fixup_position();
    }
}

bool Viewer::video_active() const
{
    return video && video_entry && image && image->entry == video_entry &&
        !clip_anim;
}

bool Viewer::handle_mbutton(const InputMouse::mouse_btn_t button,
                            const bool pressed, const Point& pos)
{
    // player output covers the whole window (on-screen controller is
    // placed relative to the window, not to the video)
    if (!video_active() || !pos) {
        return false;
    }

    const char* name = nullptr;
    switch (button) {
        case InputMouse::BUTTON_LEFT:
            name = "MBTN_LEFT";
            break;
        case InputMouse::BUTTON_RIGHT:
            name = "MBTN_RIGHT";
            break;
        case InputMouse::BUTTON_MIDDLE:
            name = "MBTN_MID";
            break;
        default:
            return false;
    }

    // mouse input goes to video player's on-screen controller
    video->mouse_move(pos.x, pos.y);
    video->mouse_button(name, pressed);
    return true;
}

bool Viewer::animate(const double dt)
{
    if (!image) {
        return false;
    }

    bool in_progress = false;

    // kinetic panning moves the target position directly
    if (pan_vx != 0 || pan_vy != 0) {
        pan_rx += pan_vx * dt;
        pan_ry += pan_vy * dt;
        const ssize_t dx = static_cast<ssize_t>(pan_rx);
        const ssize_t dy = static_cast<ssize_t>(pan_ry);
        pan_rx -= dx;
        pan_ry -= dy;
        const Point prev = position;
        set_position(Point { .x = position.x + dx, .y = position.y + dy });
        // stop at the edges
        if (dx && position.x == prev.x) {
            pan_vx = 0;
        }
        if (dy && position.y == prev.y) {
            pan_vy = 0;
        }
        const double decay = std::exp(-dt * PAN_FRICTION);
        pan_vx *= decay;
        pan_vy *= decay;
        if (std::hypot(pan_vx, pan_vy) < PAN_STOP_VELOCITY) {
            stop_pan();
        }
        view_snap = true;
        in_progress = pan_vx != 0 || pan_vy != 0;
    }

    // displayed transform follows the target one
    const double ts = scale;
    const double tx = static_cast<double>(position.x);
    const double ty = static_cast<double>(position.y);

    if (view_snap || view_scale <= 0) {
        view_snap = false;
        view_scale = ts;
        view_x = tx;
        view_y = ty;
        view_anim = { ts, tx, ty, ts, tx, ty, 1.0, VIEW_ANIM_TIME };
        clip_anim = false;
    } else if (!view_locked &&
               (ts != view_anim.s1 || tx != view_anim.x1 ||
                ty != view_anim.y1)) {
        // target changed: start new transition from the displayed state
        view_anim = { view_scale, view_x, view_y, ts,
                      tx,         ty,     0.0,    VIEW_ANIM_TIME };
        clip_anim = false;
    }

    if (view_anim.t < 1.0) {
        ViewAnim& va = view_anim;
        va.t = std::min(1.0, va.t + dt / va.duration);
        const double e = ease_out(va.t);

        // geometric interpolation of scale
        view_scale = va.s0 * std::pow(va.s1 / va.s0, e);

        // zoom around the fixed point of start and target transforms, so the
        // anchored point (e.g. under mouse pointer) doesn't drift
        const bool zoom = std::abs(va.s1 - va.s0) > va.s0 * 1e-6;
        const auto axis = [&](const double p0, const double p1) {
            if (!zoom) {
                return p0 + (p1 - p0) * e;
            }
            const double fixed = (p0 - p1) / (va.s1 - va.s0); // image coord
            return p0 + va.s0 * fixed - view_scale * fixed;
        };
        view_x = axis(va.x0, va.x1);
        view_y = axis(va.y0, va.y1);

        if (va.t >= 1.0) {
            view_scale = va.s1;
            view_x = va.x1;
            view_y = va.y1;
            if (!view_locked) {
                clip_anim = false;
            }
        } else {
            in_progress = true;
        }
    }

    // start video playback when opening transition is finished
    if (video_pending && !clip_anim && is_active()) {
        const ImageEntryPtr entry = video_pending;
        video_pending = nullptr;
        if (image->entry == entry) {
            if (!video) {
                video = std::make_unique<EmbeddedVideo>();
            }
            if (video->start(entry->path)) {
                video_entry = entry;
                in_progress = true;
            } else {
                release_video();
                VideoPlayer::open(entry->path);
            }
        }
    }

    // full image was shown during opening transition: finish postponed work
    if (after_transition && !clip_anim) {
        after_transition = false;
        if (image && !is_placeholder()) {
            if (placeholder.reset) {
                reset();
                in_progress = true;
            }
            preloader_start();
        }
        placeholder.reset = false;
    }

    return in_progress;
}

void Viewer::pan(const double delta_h, const double delta_v)
{
    if (!image) {
        return;
    }

    if (scroll_finger) {
        // touchpad: follow the finger
        if (pan_tracker.empty()) {
            stop_pan(); // touch stops kinetic scrolling
        }
        // inverted direction: content follows the fingers
        const double factor =
            -FINGER_PAN_FACTOR * Application::get_ui()->get_scale();
        const double dx = delta_h * factor;
        const double dy = delta_v * factor;
        pan_tracker.add(scroll_time, dx, dy);
        pan_rx += dx;
        pan_ry += dy;
        const ssize_t ix = static_cast<ssize_t>(pan_rx);
        const ssize_t iy = static_cast<ssize_t>(pan_ry);
        pan_rx -= ix;
        pan_ry -= iy;
        snap_view();
        set_position(Point { .x = position.x + ix, .y = position.y + iy });
    } else {
        // mouse wheel: animated step
        stop_pan();
        set_position(Point {
            .x = position.x + static_cast<ssize_t>(delta_h * WHEEL_PAN_STEP),
            .y = position.y + static_cast<ssize_t>(delta_v * WHEEL_PAN_STEP),
        });
    }
    Application::redraw();
}

void Viewer::transition_from(const Rectangle& tile, const bool cover)
{
    if (!image || !tile) {
        return;
    }

    const Pixmap& pm = image->frames[frame_index].pm;
    const double sw = static_cast<double>(tile.width) / pm.width();
    const double sh = static_cast<double>(tile.height) / pm.height();
    const double s = cover ? std::max(sw, sh) : std::min(sw, sh);
    const double w = pm.width() * s;
    const double h = pm.height() * s;
    const double x = tile.x + (tile.width - w) / 2.0;
    const double y = tile.y + (tile.height - h) / 2.0;

    // start from the thumbnail, finish at the current scale/position
    view_snap = false;
    view_locked = false;
    view_scale = s;
    view_x = x;
    view_y = y;
    view_anim = { s,
                  x,
                  y,
                  scale,
                  static_cast<double>(position.x),
                  static_cast<double>(position.y),
                  0.0,
                  MODE_ANIM_TIME };

    const Rectangle full { 0, 0, window_size.width, window_size.height };
    clip_from = cover ? tile
                      : Rectangle(static_cast<ssize_t>(x),
                                  static_cast<ssize_t>(y),
                                  static_cast<size_t>(w),
                                  static_cast<size_t>(h));
    clip_to = full.intersect({ position, static_cast<Size>(pm) * scale });
    clip_anim = true;
}

void Viewer::transition_to(const Rectangle& tile, const bool cover)
{
    if (!image || !tile) {
        return;
    }

    const Pixmap& pm = image->frames[frame_index].pm;
    const double sw = static_cast<double>(tile.width) / pm.width();
    const double sh = static_cast<double>(tile.height) / pm.height();
    const double s = cover ? std::max(sw, sh) : std::min(sw, sh);
    const double w = pm.width() * s;
    const double h = pm.height() * s;
    const double x = tile.x + (tile.width - w) / 2.0;
    const double y = tile.y + (tile.height - h) / 2.0;

    if (view_scale <= 0) {
        view_scale = scale;
        view_x = position.x;
        view_y = position.y;
    }

    // shrink from the currently displayed state to the thumbnail
    const Rectangle full { 0, 0, window_size.width, window_size.height };
    clip_from = full.intersect(
        { Point { .x = static_cast<ssize_t>(view_x),
                  .y = static_cast<ssize_t>(view_y) },
          static_cast<Size>(pm) * view_scale });
    clip_to = cover ? tile
                    : Rectangle(static_cast<ssize_t>(x),
                                static_cast<ssize_t>(y), static_cast<size_t>(w),
                                static_cast<size_t>(h));
    clip_anim = true;

    stop_pan();
    view_snap = false;
    view_locked = true;
    view_anim = { view_scale, view_x, view_y, s, x, y, 0.0, MODE_ANIM_TIME };
}

Rectangle Viewer::view_clip() const
{
    const Rectangle full { 0, 0, window_size.width, window_size.height };
    if (!clip_anim) {
        return full;
    }
    const double e = ease_out(view_anim.t);
    const auto lerp = [e](const double a, const double b) {
        return a + (b - a) * e;
    };
    return {
        static_cast<ssize_t>(std::lround(lerp(clip_from.x, clip_to.x))),
        static_cast<ssize_t>(std::lround(lerp(clip_from.y, clip_to.y))),
        static_cast<size_t>(std::lround(
            lerp(static_cast<double>(clip_from.width), clip_to.width))),
        static_cast<size_t>(std::lround(
            lerp(static_cast<double>(clip_from.height), clip_to.height))),
    };
}

void Viewer::draw_photo(Canvas& wnd)
{
    if (!image) {
        return;
    }

    const Pixmap& pm = image->frames[frame_index].pm;
    const double vscale = view_scale > 0 ? view_scale : scale;
    const Point vpos = view_scale > 0
        ? Point { .x = static_cast<ssize_t>(std::lround(view_x)),
                  .y = static_cast<ssize_t>(std::lround(view_y)) }
        : position;
    const Rectangle clip = view_clip();
    const Rectangle imgr =
        clip.intersect({ vpos, static_cast<Size>(pm) * vscale });
    if (!imgr) {
        return;
    }

    if (pm.format() == Pixmap::ARGB) {
        if (tr_chessboard) {
            wnd.grid(imgr, tr_cbsize, tr_cbcolor[0], tr_cbcolor[1]);
        } else {
            wnd.fill(imgr, tr_bgcolor);
        }
    }
    wnd.draw(pm, vpos, vscale, clip);
}

void Viewer::start_video(const ImageEntryPtr& entry)
{
    stop_video();
    if (EmbeddedVideo::available()) {
        if (video && video->ready() && video->start(entry->path)) {
            // renderer is ready: start loading now, frames are shown after
            // the opening transition
            video_entry = entry;
        } else {
            // first video: initialize player after the opening transition
            video_pending = entry;
        }
        Application::redraw();
    } else {
        VideoPlayer::open(entry->path); // fall back to external player
    }
}

void Viewer::stop_video()
{
    video_pending = nullptr;
    video_entry = nullptr;
    if (video) {
        video->stop();
    }
}

void Viewer::prewarm_video()
{
    // load libraries and open GPU decoder device in background, it takes
    // tens of milliseconds; the player itself needs GL context of the main
    // thread, so it is prepared there once libmpv is loaded
    loader.add([this] {
        lower_thread_priority();
#ifdef HAVE_HWDECODE
        HwDecode::warmup();
#endif
#ifdef HAVE_LIBMPV
        if (Lazy::mpv()) {
            Application::self().add_event(AppEvent::Callback { [this] {
                prepare_video();
            } });
        }
#endif
    });
}

void Viewer::prepare_video()
{
    if (!video && EmbeddedVideo::available()) {
        video = std::make_unique<EmbeddedVideo>();
        if (!video->prepare()) {
            video.reset();
        }
    }
}

void Viewer::release_video()
{
    stop_video();
    video.reset();
}

void Viewer::video_pause()
{
    if (video) {
        video->toggle_pause();
    }
}

void Viewer::video_seek(const double seconds)
{
    if (video) {
        video->seek(seconds);
    }
}

void Viewer::video_volume(const double delta)
{
    if (video) {
        video->change_volume(delta);
    }
}

void Viewer::handle_scroll_stop()
{
    const auto [vx, vy] = pan_tracker.release(scroll_time);
    const double speed = std::hypot(vx, vy);
    if (speed >= PAN_MIN_VELOCITY) {
        const double k = std::min(1.0, PAN_MAX_VELOCITY / speed);
        pan_vx = vx * k;
        pan_vy = vy * k;
        Application::redraw();
    }
}

void Viewer::stop_pan()
{
    pan_vx = 0;
    pan_vy = 0;
    pan_rx = 0;
    pan_ry = 0;
}

void Viewer::invalidate(const std::vector<ImageEntryPtr>& entries)
{
    {
        // remove entries from cache
        const std::scoped_lock lock(image_pool.mutex);
        for (const auto& entry : entries) {
            image_pool.history.get(entry);
            image_pool.preload.get(entry);
        }
    }

    // inactive mode keeps its last image to reopen it without reloading:
    // drop it if outdated (the active mode reloads it in handle_imagelist)
    if (!is_active() && image &&
        std::ranges::find(entries, image->entry) != entries.end()) {
        image = nullptr;
        placeholder.image = nullptr;
        placeholder.full = nullptr;
    }
}

void Viewer::handle_imagelist(const ImageListEvent event,
                              const std::vector<ImageEntryPtr>& entries)
{
    AppMode::handle_imagelist(event, entries);

    if (event == ImageListEvent::Modify || event == ImageListEvent::Remove) {
        invalidate(entries);
    }

    switch (event) {
        case ImageListEvent::Create:
            if (image) {
                preloader_start();
            } else {
                open(ImageList::Dir::First);
            }
            break;
        case ImageListEvent::Modify:
            if (image) {
                for (const auto& entry : entries) {
                    if (entry == image->entry) {
                        reload();
                        break;
                    }
                }
            }
            break;
        case ImageListEvent::Remove:
            if (image && image->entry->removed) {
                reload();
            }
            break;
    }
}

void Viewer::set_image(const ImagePtr& img)
{
    if (image && image != img && !is_placeholder() &&
        image->format != VIDEO_POSTER) {
        // put current image to history (video posters have own cache)
        const std::scoped_lock lock(image_pool.mutex);
        image_pool.history.put(image);
    }

    if ((video_entry || video_pending) &&
        (!img ||
         (img->entry != video_entry && img->entry != video_pending))) {
        stop_video(); // another image: stop playback
    }

    if (placeholder.full && (!img || img != placeholder.image)) {
        const std::scoped_lock lock(image_pool.mutex);
        image_pool.history.put(placeholder.full);
        placeholder.full = nullptr;
    }

    image = img;
    frame_index = 0;

    // new image is shown immediately at its place
    stop_pan();
    snap_view();
    view_locked = false;
    clip_anim = false;

    if (!image) {
        animation_timer.reset(0, 0); // no frames left to animate
        switch_current();
        return;
    }

    const Pixmap& pm = image->frames[0].pm;

    const bool is_animation =
        image->frames.size() > 1 && image->frames[0].duration;

    // set content type
    Ui* ui = Application::get_ui();
    ui->set_ctype(is_animation ? Ui::ContentType::Animation
                               : Ui::ContentType::Static);

    if (std::holds_alternative<Scale>(default_scale) &&
        std::get<Scale>(default_scale) == Scale::Keep && previmg) {
        // handle "keep scale" mode
        const ssize_t diff_w = static_cast<ssize_t>(previmg.width) -
            static_cast<ssize_t>(pm.width());
        const ssize_t diff_h = static_cast<ssize_t>(previmg.height) -
            static_cast<ssize_t>(pm.height());
        position.x += std::floor(scale * diff_w) / 2.0;
        position.y += std::floor(scale * diff_h) / 2.0;
        fixup_position();
    } else {
        if (std::holds_alternative<Scale>(default_scale)) {
            set_scale(std::get<Scale>(default_scale));
        } else {
            set_scale(std::get<double>(default_scale) * pixel_ratio());
        }

        set_position(default_pos);
    }

    previmg = pm;

    if (is_placeholder()) {
        // remember default view to detect user's zoom/pan while loading
        placeholder.scale = scale;
        placeholder.position = position;
    }

    enable_animation(is_animation);

    if (!is_placeholder()) {
        // otherwise started when the full image is shown, so preloading
        // doesn't slow down loading of the current image
        preloader_start();
    }

    switch_current();
    update_text(TextUpdate::All);
}

ImagePtr Viewer::load_progressive(const ImageEntryPtr& entry)
{
    // "keep" mode uses absolute scale between images, not worth handling
    if (std::holds_alternative<Scale>(default_scale) &&
        std::get<Scale>(default_scale) == Scale::Keep) {
        return nullptr;
    }

    Size orig;
    Pixmap thumb = FdoThumb::load(entry->path, &orig);
    if (!thumb) {
        return nullptr;
    }

    const double tw = thumb.width();
    const double th = thumb.height();
    bool exact = false;
    double ratio = 1.0;
    if (orig) {
        // metadata contains size before EXIF orientation fix
        const double aspect = tw / th;
        const double a1 = static_cast<double>(orig.width) / orig.height;
        const double a2 = static_cast<double>(orig.height) / orig.width;
        if (std::abs(a1 - aspect) / aspect < 0.02) {
            ratio = orig.width / tw;
            exact = true;
        } else if (std::abs(a2 - aspect) / aspect < 0.02) {
            ratio = orig.height / tw;
            exact = true;
        }
    }
    if (exact && ratio <= 1.0) {
        return nullptr; // thumbnail is not smaller than the image itself
    }

    ImagePtr img = std::make_shared<Image>();
    img->frames.resize(1);
    img->frames[0].pm = std::move(thumb);
    img->entry = entry;

    placeholder.image = img;
    placeholder.ratio = ratio;

    // decode the full image, drop the queued (not started) requests
    loader.cancel();
    loader.add([this, entry] {
        lower_thread_priority();
        ImagePtr full = FormatFactory::self().load(entry);
        {
            const std::scoped_lock lock(loaded_mutex);
            loaded.emplace_back(entry, std::move(full));
        }
        loaded_notify.set();
    });

    Log::verbose("Show thumbnail of {} while loading",
                 entry->path.filename().string());

    return img;
}

void Viewer::handle_loaded()
{
    std::vector<std::pair<ImageEntryPtr, ImagePtr>> results;
    {
        const std::scoped_lock lock(loaded_mutex);
        results.swap(loaded);
        loaded_notify.reset();
    }

    for (auto& [entry, full] : results) {
        const bool current = is_placeholder() && image->entry == entry;

        if (!full) {
            Log::error("Unable to load {}", entry->path.string());
            if (current) {
                placeholder.image = nullptr; // keep showing the thumbnail
            }
            continue;
        }

        if (!current) {
            // the user moved on: keep it for later
            const std::scoped_lock lock(image_pool.mutex);
            image_pool.history.put(full);
            continue;
        }

        // the thumbnail is replaced when the full image is ready to be drawn
        // (uploaded to GPU in small steps, see window_redraw)
        placeholder.full = full;
        Application::redraw();
    }
}

void Viewer::show_full()
{
    const ImagePtr full = std::move(placeholder.full);
    placeholder.full = nullptr;

    // replace thumbnail with the full image in place: convert all scale
    // factors to the new pixmap size, positions stay the same
    const Pixmap& thumb = image->frames[0].pm;
    const Pixmap& pm = full->frames[0].pm;
    const double k = static_cast<double>(thumb.width()) / pm.width();
    const bool untouched = scale == placeholder.scale &&
        position.x == placeholder.position.x &&
        position.y == placeholder.position.y;

    image = full;
    placeholder.image = nullptr;
    frame_index = 0;

    scale *= k;
    if (view_scale > 0) {
        view_scale *= k;
    }
    view_anim.s0 *= k;
    view_anim.s1 *= k;
    previmg = pm;

    if (untouched && !view_locked) {
        // land exactly on the default view of the full image (smoothly
        // if the thumbnail size didn't match the image); not during the
        // opening transition: it would restart the animation
        if (clip_anim) {
            placeholder.reset = true;
        } else {
            reset();
        }
    }

    if (is_active()) {
        const bool is_animation =
            image->frames.size() > 1 && image->frames[0].duration;
        Application::get_ui()->set_ctype(is_animation
                                             ? Ui::ContentType::Animation
                                             : Ui::ContentType::Static);
        enable_animation(is_animation);
        update_text(TextUpdate::All);
        if (clip_anim) {
            after_transition = true; // don't slow down the transition
        } else {
            preloader_start();
        }
    }
}

void Viewer::stop_loading()
{
    loader.cancel();
    loader.wait();
    const std::scoped_lock lock(loaded_mutex);
    loaded.clear();
}

void Viewer::set_frame(const size_t index)
{
    assert(is_active());

    if (image && index < image->frames.size()) {
        frame_index = index;
        update_text(TextUpdate::Frame);
        Application::redraw();
    }
}

void Viewer::update_text(const TextUpdate what) const
{
    assert(image);

    Text& text = Text::self();

    if (what == TextUpdate::All) {
        text.reset(image);
    }

    if (what == TextUpdate::All || what == TextUpdate::Frame) {
        const Pixmap& pm = image->frames[frame_index].pm;
        text.set_field(Text::FIELD_FRAME_INDEX,
                       std::to_wstring(frame_index + 1));
        text.set_field(Text::FIELD_FRAME_WIDTH, std::to_wstring(pm.width()));
        text.set_field(Text::FIELD_FRAME_HEIGHT, std::to_wstring(pm.height()));
    }

    if (what == TextUpdate::All || what == TextUpdate::Scale) {
        std::wstring tscale;
        const double real_scale = scale / pixel_ratio();
        if (real_scale >= 0.1) {
            tscale =
                std::format(L"{}%", static_cast<size_t>(real_scale * 100));
        } else {
            tscale = std::format(L"{:.02}%", real_scale * 100);
        }
        text.set_field(Text::FIELD_SCALE, tscale);
    }

    text.update();
}

ssize_t Viewer::fixup_position(const ssize_t pos, const size_t sz,
                               const size_t max_pos, const bool center)
{
    ssize_t fixed = pos;

    if (sz <= max_pos) {
        if (center) {
            // entire image fits the window, center it
            fixed = max_pos / 2 - sz / 2;
        } else {
            // keep it where it is (e.g. zoom anchor), but inside the window
            fixed = std::clamp<ssize_t>(pos, 0, max_pos - sz);
        }
    } else {
        // prevent going outside the max position
        const ssize_t out_pos = fixed + sz;
        if (fixed > 0 && std::cmp_greater(out_pos, max_pos)) {
            fixed = 0;
        }
        if (pos < 0 && std::cmp_less(out_pos, max_pos)) {
            fixed = max_pos - sz;
        }
    }

    return fixed;
}

void Viewer::fixup_position()
{
    assert(image);

    const Pixmap& pm = image->frames[frame_index].pm;
    const Size scaled = static_cast<Size>(pm) * scale;

    if (auto_pos == AutoPos::Free || pinching) {
        // don't let image to be far out of window
        if (position.x + static_cast<ssize_t>(scaled.width) < 0) {
            position.x = -static_cast<ssize_t>(scaled.width);
        }
        if (std::cmp_greater(position.x, window_size.width)) {
            position.x = window_size.width;
        }
        if (position.y + static_cast<ssize_t>(scaled.height) < 0) {
            position.y = -static_cast<ssize_t>(scaled.height);
        }
        if (std::cmp_greater(position.y, window_size.height)) {
            position.y = window_size.height;
        }
    } else {
        // center only if the whole image fits the window, otherwise the
        // smaller side may stay off-center (zooming at the image edge)
        const bool fits = scaled.width <= window_size.width &&
            scaled.height <= window_size.height;
        position.x = fixup_position(position.x, scaled.width,
                                    window_size.width, fits);
        position.y = fixup_position(position.y, scaled.height,
                                    window_size.height, fits);
    }

    Application::redraw();
}

void Viewer::preloader_work()
{
    assert(image);
    lower_thread_priority();

    ImageList& il = ImageList::self();
    ImagePtr current_image = image;
    ImageEntryPtr last_entry = image->entry;
    size_t counter = 0;

    while (!image_pool.stop && counter < image_pool.preload.capacity) {
        if (!image) {
            break; // no current image in viewer
        }
        if (current_image != image) {
            // current image has changed, restart preloading
            current_image = image;
            last_entry = image->entry;
            counter = 0;
        }

        ImageEntryPtr next_entry = il.get(last_entry, ImageList::Dir::Next);
        if (!next_entry && imagelist_loop) {
            next_entry = il.get(nullptr, ImageList::Dir::First);
        }
        if (!next_entry || next_entry == last_entry) {
            // no more images to preload
            const std::scoped_lock lock(image_pool.mutex);
            image_pool.preload.trim(counter);
            break;
        }

        if (this == &Viewer::self() && VideoPlayer::is_video(next_entry) &&
            EmbeddedVideo::available()) {
            // video is opened with a poster from the thumbnail cache and then
            // played, a decoded frame would be never shown
            last_entry = next_entry;
            ++counter;
            continue;
        }

        ImagePtr next_image = nullptr;

        // get existing image form history/preload cache
        {
            const std::scoped_lock lock(image_pool.mutex);
            next_image = image_pool.preload.get(next_entry);
            if (!next_image) {
                next_image = image_pool.history.get(next_entry);
            }
        }

        // load image
        if (!next_image) {
            next_image = FormatFactory::self().load(next_entry);
        }

        if (!next_image) {
            il.remove(next_entry);
        } else {
            Log::verbose("Put image {} to cache",
                         next_entry->path.filename().string());
            const std::scoped_lock lock(image_pool.mutex);
            image_pool.preload.put(next_image);
            last_entry = next_image->entry;
            ++counter;
        }
    }

    image_pool.stop = true;
}

void Viewer::preloader_start()
{
    if (image_pool.thread.joinable()) {
        if (image_pool.stop) {
            image_pool.thread.join();
        } else {
            return; // already in progress
        }
    }
    image_pool.stop = false;
    image_pool.thread = std::thread(&Viewer::preloader_work, this);
}

void Viewer::preloader_stop()
{
    if (image_pool.thread.joinable()) {
        image_pool.stop = true;
        image_pool.thread.join();
    }
}

void Viewer::Cache::trim(const size_t size)
{
    // Only shrink: the main thread can take images out of the cache while the
    // preloader works, and resize() would fill the gap with empty pointers.
    if (cache.size() > size) {
        cache.resize(size);
    }
}

void Viewer::Cache::put(const ImagePtr& image)
{
    if (capacity) {
        if (cache.size() == capacity) {
            cache.pop_back();
        }
        cache.push_front(image);
    }
}

ImagePtr Viewer::Cache::get(const ImageEntryPtr& entry)
{
    const auto it = std::find_if(cache.begin(), cache.end(),
                                 [&entry](const ImagePtr& image) {
                                     return image->entry == entry;
                                 });
    if (it == cache.end()) {
        return nullptr;
    }
    ImagePtr image = *it;
    cache.erase(it);
    return image;
}
