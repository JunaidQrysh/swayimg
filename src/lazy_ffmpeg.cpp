// SPDX-License-Identifier: MIT
// Lazy loaded FFmpeg: stubs for used functions, see lazylib.hpp.

#include "lazylib.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <format>

namespace {

LazyLibrary& avutil()
{
    static LazyLibrary lib(
        std::format("libavutil.so.{}", LIBAVUTIL_VERSION_MAJOR));
    return lib;
}

LazyLibrary& avcodec()
{
    static LazyLibrary lib(
        std::format("libavcodec.so.{}", LIBAVCODEC_VERSION_MAJOR));
    return lib;
}

LazyLibrary& avformat()
{
    static LazyLibrary lib(
        std::format("libavformat.so.{}", LIBAVFORMAT_VERSION_MAJOR));
    return lib;
}

LazyLibrary& swscale()
{
    static LazyLibrary lib(
        std::format("libswscale.so.{}", LIBSWSCALE_VERSION_MAJOR));
    return lib;
}

} // namespace

bool Lazy::ffmpeg()
{
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    return avutil().load() && avcodec().load() && avformat().load() &&
        swscale().load();
}

// clang-format off

// libavutil
LAZY_STUB(avutil, AVBufferRef*, av_buffer_ref, (const AVBufferRef* buf), (buf))
LAZY_STUB(avutil, void, av_buffer_unref, (AVBufferRef** buf), (buf))
LAZY_STUB(avutil, double, av_display_rotation_get, (const int32_t matrix[9]),
          (matrix))
LAZY_STUB(avutil, AVFrame*, av_frame_alloc, (void), ())
LAZY_STUB(avutil, AVFrame*, av_frame_clone, (const AVFrame* src), (src))
LAZY_STUB(avutil, void, av_frame_free, (AVFrame** frame), (frame))
LAZY_STUB(avutil, AVFrameSideData*, av_frame_get_side_data,
          (const AVFrame* frame, enum AVFrameSideDataType type),
          (frame, type))
LAZY_STUB(avutil, void, av_frame_unref, (AVFrame* frame), (frame))
LAZY_STUB(avutil, void, av_free, (void* ptr), (ptr))
LAZY_STUB(avutil, void, av_freep, (void* ptr), (ptr))
LAZY_STUB(avutil, void*, av_malloc, (size_t size), (size))
LAZY_STUB(avutil, int, av_hwdevice_ctx_create,
          (AVBufferRef** device_ctx, enum AVHWDeviceType type,
           const char* device, AVDictionary* opts, int flags),
          (device_ctx, type, device, opts, flags))
LAZY_STUB(avutil, int, av_hwframe_transfer_data,
          (AVFrame* dst, const AVFrame* src, int flags), (dst, src, flags))
LAZY_STUB(avutil, void, av_log_set_level, (int level), (level))
LAZY_STUB(avutil, const AVPixFmtDescriptor*, av_pix_fmt_desc_get,
          (enum AVPixelFormat pix_fmt), (pix_fmt))
LAZY_STUB(avutil, int64_t, av_rescale_q,
          (int64_t a, AVRational bq, AVRational cq), (a, bq, cq))

// libavcodec
LAZY_STUB(avcodec, AVCodecContext*, avcodec_alloc_context3,
          (const AVCodec* codec), (codec))
LAZY_STUB(avcodec, const AVCodec*, avcodec_find_decoder, (enum AVCodecID id),
          (id))
LAZY_STUB(avcodec, void, avcodec_flush_buffers, (AVCodecContext* avctx),
          (avctx))
LAZY_STUB(avcodec, void, avcodec_free_context, (AVCodecContext** avctx),
          (avctx))
LAZY_STUB(avcodec, int, avcodec_open2,
          (AVCodecContext* avctx, const AVCodec* codec,
           AVDictionary** options),
          (avctx, codec, options))
LAZY_STUB(avcodec, int, avcodec_parameters_to_context,
          (AVCodecContext* codec, const struct AVCodecParameters* par),
          (codec, par))
LAZY_STUB(avcodec, int, avcodec_receive_frame,
          (AVCodecContext* avctx, AVFrame* frame), (avctx, frame))
LAZY_STUB(avcodec, int, avcodec_send_packet,
          (AVCodecContext* avctx, const AVPacket* avpkt), (avctx, avpkt))
LAZY_STUB(avcodec, int, av_new_packet, (AVPacket* pkt, int size), (pkt, size))
LAZY_STUB(avcodec, AVPacket*, av_packet_alloc, (void), ())
LAZY_STUB(avcodec, void, av_packet_free, (AVPacket** pkt), (pkt))
LAZY_STUB(avcodec, const AVPacketSideData*, av_packet_side_data_get,
          (const AVPacketSideData* sd, int nb_sd,
           enum AVPacketSideDataType type),
          (sd, nb_sd, type))
LAZY_STUB(avcodec, void, av_packet_unref, (AVPacket* pkt), (pkt))

// libavformat
LAZY_STUB(avformat, int, av_find_best_stream,
          (AVFormatContext* ic, enum AVMediaType type, int wanted_stream_nb,
           int related_stream, const struct AVCodec** decoder_ret, int flags),
          (ic, type, wanted_stream_nb, related_stream, decoder_ret, flags))
LAZY_STUB(avformat, AVFormatContext*, avformat_alloc_context, (void), ())
LAZY_STUB(avformat, void, avformat_close_input, (AVFormatContext** s), (s))
LAZY_STUB(avformat, int, avformat_find_stream_info,
          (AVFormatContext* ic, AVDictionary** options), (ic, options))
LAZY_STUB(avformat, int, avformat_open_input,
          (AVFormatContext** ps, const char* url, const AVInputFormat* fmt,
           AVDictionary** options),
          (ps, url, fmt, options))
LAZY_STUB(avformat, AVIOContext*, avio_alloc_context,
          (unsigned char* buffer, int buffer_size, int write_flag,
           void* opaque,
           int (*read_packet)(void* opaque, uint8_t* buf, int buf_size),
           int (*write_packet)(void* opaque, const uint8_t* buf,
                               int buf_size),
           int64_t (*seek)(void* opaque, int64_t offset, int whence)),
          (buffer, buffer_size, write_flag, opaque, read_packet, write_packet,
           seek))
LAZY_STUB(avformat, void, avio_context_free, (AVIOContext** s), (s))
LAZY_STUB(avformat, const AVInputFormat*, av_probe_input_format,
          (const AVProbeData* pd, int is_opened), (pd, is_opened))
LAZY_STUB(avformat, int, av_read_frame, (AVFormatContext* s, AVPacket* pkt),
          (s, pkt))
LAZY_STUB(avformat, int, av_seek_frame,
          (AVFormatContext* s, int stream_index, int64_t timestamp,
           int flags),
          (s, stream_index, timestamp, flags))

// libswscale
LAZY_STUB(swscale, void, sws_freeContext, (SwsContext* swsContext),
          (swsContext))
LAZY_STUB(swscale, const int*, sws_getCoefficients, (int colorspace),
          (colorspace))
LAZY_STUB(swscale, SwsContext*, sws_getContext,
          (int srcW, int srcH, enum AVPixelFormat srcFormat, int dstW,
           int dstH, enum AVPixelFormat dstFormat, int flags,
           SwsFilter* srcFilter, SwsFilter* dstFilter, const double* param),
          (srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags, srcFilter,
           dstFilter, param))
LAZY_STUB(swscale, SwsContext*, sws_getCachedContext,
          (SwsContext* context, int srcW, int srcH,
           enum AVPixelFormat srcFormat, int dstW, int dstH,
           enum AVPixelFormat dstFormat, int flags, SwsFilter* srcFilter,
           SwsFilter* dstFilter, const double* param),
          (context, srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags,
           srcFilter, dstFilter, param))
LAZY_STUB(swscale, int, sws_scale,
          (SwsContext* c, const uint8_t* const srcSlice[],
           const int srcStride[], int srcSliceY, int srcSliceH,
           uint8_t* const dst[], const int dstStride[]),
          (c, srcSlice, srcStride, srcSliceY, srcSliceH, dst, dstStride))
LAZY_STUB(swscale, int, sws_setColorspaceDetails,
          (SwsContext* c, const int inv_table[4], int srcRange,
           const int table[4], int dstRange, int brightness, int contrast,
           int saturation),
          (c, inv_table, srcRange, table, dstRange, brightness, contrast,
           saturation))

// clang-format on
