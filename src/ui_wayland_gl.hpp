// SPDX-License-Identifier: MIT
// EGL/OpenGL ES rendering for Wayland window.

#pragma once

#include "glcanvas.hpp"

#include <EGL/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <memory>

class WaylandGl {
public:
    WaylandGl() = default;
    ~WaylandGl();

    WaylandGl(const WaylandGl&) = delete;
    WaylandGl& operator=(const WaylandGl&) = delete;

    /**
     * Initialize EGL display and context.
     * @param display wayland display
     * @return false if GPU rendering is not available
     */
    bool initialize(wl_display* display);

    /**
     * Begin frame: create/resize window surface and make context current.
     * Must be called from the rendering thread.
     * @param surface wayland surface
     * @param size window buffer size in pixels
     * @return canvas to draw on or nullptr on errors
     */
    GlCanvas* begin(wl_surface* surface, const Size& size);

    /**
     * Finish frame: present it (commits the wayland surface).
     */
    void end();

    /**
     * Warm up GPU renderer (compile shaders), call after the first frame.
     */
    void warmup();

private:
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLConfig egl_config = nullptr;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLContext upload_context = EGL_NO_CONTEXT; ///< Shared, for uploads
    EGLSurface egl_surface = EGL_NO_SURFACE;
    wl_egl_window* egl_window = nullptr;
    Size window_size;
    std::unique_ptr<GlCanvas> canvas;
    bool failed = false;
};
