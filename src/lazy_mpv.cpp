// SPDX-License-Identifier: MIT
// Lazy loaded libmpv: stubs for used functions, see lazylib.hpp.

#include "lazylib.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <format>

namespace {

LazyLibrary& libmpv()
{
    static LazyLibrary lib(
        std::format("libmpv.so.{}", MPV_CLIENT_API_VERSION >> 16));
    return lib;
}

} // namespace

bool Lazy::mpv()
{
    return libmpv().load();
}

// clang-format off
LAZY_STUB(libmpv, mpv_handle*, mpv_create, (void), ())
LAZY_STUB(libmpv, int, mpv_initialize, (mpv_handle* ctx), (ctx))
LAZY_STUB(libmpv, void, mpv_terminate_destroy, (mpv_handle* ctx), (ctx))
LAZY_STUB(libmpv, int, mpv_set_option_string,
          (mpv_handle* ctx, const char* name, const char* data),
          (ctx, name, data))
LAZY_STUB(libmpv, int, mpv_command_async,
          (mpv_handle* ctx, uint64_t reply_userdata, const char** args),
          (ctx, reply_userdata, args))
LAZY_STUB(libmpv, int, mpv_set_property_async,
          (mpv_handle* ctx, uint64_t reply_userdata, const char* name,
           mpv_format format, void* data),
          (ctx, reply_userdata, name, format, data))
LAZY_STUB(libmpv, int, mpv_observe_property,
          (mpv_handle* mpv, uint64_t reply_userdata, const char* name,
           mpv_format format),
          (mpv, reply_userdata, name, format))
LAZY_STUB(libmpv, mpv_event*, mpv_wait_event,
          (mpv_handle* ctx, double timeout), (ctx, timeout))
LAZY_STUB(libmpv, int, mpv_render_context_create,
          (mpv_render_context** res, mpv_handle* mpv,
           mpv_render_param* params),
          (res, mpv, params))
LAZY_STUB(libmpv, void, mpv_render_context_free, (mpv_render_context* ctx),
          (ctx))
LAZY_STUB(libmpv, int, mpv_render_context_render,
          (mpv_render_context* ctx, mpv_render_param* params), (ctx, params))
LAZY_STUB(libmpv, void, mpv_render_context_set_update_callback,
          (mpv_render_context* ctx, mpv_render_update_fn callback,
           void* callback_ctx),
          (ctx, callback, callback_ctx))
LAZY_STUB(libmpv, uint64_t, mpv_render_context_update,
          (mpv_render_context* ctx), (ctx))
// clang-format on
