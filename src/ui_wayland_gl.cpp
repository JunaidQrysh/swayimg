// SPDX-License-Identifier: MIT
// EGL/OpenGL ES rendering for Wayland window.

#include "ui_wayland_gl.hpp"

#include "log.hpp"

#include <EGL/eglext.h>

#include <cstring>

WaylandGl::~WaylandGl()
{
    if (egl_display == EGL_NO_DISPLAY) {
        return;
    }
    if (egl_context != EGL_NO_CONTEXT && egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    }
    canvas.reset(); // free GL resources while context is current
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (upload_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, upload_context);
    }
    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display, egl_surface);
    }
    if (egl_window) {
        wl_egl_window_destroy(egl_window);
    }
    if (egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_context);
    }
    eglTerminate(egl_display);
}

bool WaylandGl::initialize(wl_display* display)
{
    egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display,
                                        nullptr);
    if (egl_display == EGL_NO_DISPLAY) {
        Log::verbose("EGL: unable to get display");
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(egl_display, &major, &minor)) {
        Log::verbose("EGL: unable to initialize");
        egl_display = EGL_NO_DISPLAY;
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        Log::verbose("EGL: OpenGL ES is not supported");
        return false;
    }

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    EGLint num = 0;
    if (!eglChooseConfig(egl_display, config_attrs, &egl_config, 1, &num) ||
        num == 0) {
        Log::verbose("EGL: no suitable config");
        return false;
    }

    const EGLint context_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION,
        3,
        EGL_CONTEXT_MINOR_VERSION,
        0,
        EGL_NONE,
    };
    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT,
                                   context_attrs);
    if (egl_context == EGL_NO_CONTEXT) {
        Log::verbose("EGL: unable to create OpenGL ES 3.0 context");
        return false;
    }

    // shared context for uploading textures in background thread
    const char* ext = eglQueryString(egl_display, EGL_EXTENSIONS);
    if (ext && strstr(ext, "EGL_KHR_surfaceless_context")) {
        // low priority: GPU runs frames of the main context first
        const bool prio = strstr(ext, "EGL_IMG_context_priority");
        const EGLint upload_attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION,
            3,
            EGL_CONTEXT_MINOR_VERSION,
            0,
            prio ? EGL_CONTEXT_PRIORITY_LEVEL_IMG : EGL_NONE,
            EGL_CONTEXT_PRIORITY_LOW_IMG,
            EGL_NONE,
        };
        upload_context = eglCreateContext(egl_display, egl_config,
                                          egl_context, upload_attrs);
        Log::verbose("EGL: background upload context (priority {})",
                     prio ? "low" : "default");
    }
    if (upload_context == EGL_NO_CONTEXT) {
        Log::verbose("EGL: background texture upload is not available");
    }

    return true;
}

GlCanvas* WaylandGl::begin(wl_surface* surface, const Size& size)
{
    if (failed || !size) {
        return nullptr;
    }

    if (!egl_window) {
        // first frame: create window surface and GL resources
        egl_window = wl_egl_window_create(surface, size.width, size.height);
        if (!egl_window) {
            Log::error("EGL: unable to create window");
            failed = true;
            return nullptr;
        }
        egl_surface = eglCreatePlatformWindowSurface(egl_display, egl_config,
                                                     egl_window, nullptr);
        if (egl_surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(egl_display, egl_surface, egl_surface,
                            egl_context)) {
            Log::error("EGL: unable to create window surface");
            failed = true;
            return nullptr;
        }
        // frame pacing is done by wayland frame callbacks
        eglSwapInterval(egl_display, 0);

        canvas = std::make_unique<GlCanvas>();
        if (!canvas->initialize()) {
            Log::error("Unable to initialize GPU renderer");
            canvas.reset();
            failed = true;
            return nullptr;
        }
        if (upload_context != EGL_NO_CONTEXT) {
            EGLDisplay dpy = egl_display;
            EGLContext ctx = upload_context;
            canvas->enable_async_upload(
                [dpy, ctx] {
                    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                          ctx) == EGL_TRUE;
                },
                [dpy] {
                    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                   EGL_NO_CONTEXT);
                    eglReleaseThread();
                });
        }
        window_size = size;
    } else if (size.width != window_size.width ||
               size.height != window_size.height) {
        wl_egl_window_resize(egl_window, size.width, size.height, 0, 0);
        window_size = size;
    }

    canvas->begin(size);
    return canvas.get();
}

void WaylandGl::warmup()
{
    if (canvas) {
        canvas->warmup();
    }
}

void WaylandGl::end()
{
    canvas->end();
    eglSwapBuffers(egl_display, egl_surface);
}
