// SPDX-License-Identifier: MIT
// Hardware (VAAPI) decoding of still images.

#include "hwdecode.hpp"

#include "lazylib.hpp"
#include "log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <va/va.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Max number of threads used to convert decoded frames to RGB
constexpr size_t MAX_WORKERS = 4;

/** Get number of conversion threads. */
size_t workers_count()
{
    return std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 1,
                              MAX_WORKERS);
}

/**
 * Get VAAPI device shared by all threads (creation is expensive, libva is
 * thread safe).
 * @return device context or nullptr if hardware is not available
 */
AVBufferRef* device()
{
    struct Device {
        AVBufferRef* ref = nullptr;
        Device()
        {
            if (!Lazy::ffmpeg()) {
                return; // FFmpeg is not available
            }
            if (av_hwdevice_ctx_create(&ref, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                                       nullptr, 0) < 0) {
                ref = nullptr;
                Log::verbose("VAAPI is not available");
            }
        }
        ~Device()
        {
            if (ref) {
                av_buffer_unref(&ref);
            }
        }
    };
    static Device dev; // thread safe initialization
    return dev.ref;
}

/** Codec callback: select hardware pixel format. */
enum AVPixelFormat get_vaapi_format(AVCodecContext*,
                                    const enum AVPixelFormat* fmts)
{
    for (; *fmts != AV_PIX_FMT_NONE; ++fmts) {
        if (*fmts == AV_PIX_FMT_VAAPI) {
            return *fmts;
        }
    }
    return AV_PIX_FMT_NONE; // no hardware support: fail
}

/**
 * Create hardware decoder.
 * @param par codec parameters (optional)
 * @param id codec id
 * @param extra_frames number of extra hardware frames
 * @return decoder context or nullptr on errors
 */
AVCodecContext* open_decoder(const AVCodecParameters* par, AVCodecID id,
                             const int extra_frames)
{
    AVBufferRef* dev = device();
    if (!dev) {
        return nullptr;
    }
    const AVCodec* codec = avcodec_find_decoder(par ? par->codec_id : id);
    if (!codec) {
        return nullptr;
    }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!dec) {
        return nullptr;
    }
    if (par && avcodec_parameters_to_context(dec, par) < 0) {
        avcodec_free_context(&dec);
        return nullptr;
    }
    dec->get_format = &get_vaapi_format;
    dec->hw_device_ctx = av_buffer_ref(dev);
    dec->extra_hw_frames = extra_frames;
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        return nullptr;
    }
    return dec;
}

/** YUV to BGRA converter (one instance per thread). */
class Converter {
public:
    Converter(const bool full_range, const bool bt709)
        : full_range(full_range)
        , coefs(sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601))
    {
    }

    ~Converter() { sws_freeContext(sws); }

    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;

    /**
     * Convert rows of the frame and put them to the pixmap.
     * @param src source frame (in system memory)
     * @param y0,rows source rows to convert
     * @param pm target pixmap
     * @param x,y position of the frame on the pixmap
     * @return false on errors
     */
    bool convert(const AVFrame* src, const int y0, const int rows, Pixmap& pm,
                 const ssize_t x, const ssize_t y)
    {
        const ssize_t pw = static_cast<ssize_t>(pm.width());
        const ssize_t ph = static_cast<ssize_t>(pm.height());
        if (x < 0 || y < 0) {
            return false;
        }
        if (x >= pw || y + y0 >= ph) {
            return true; // out of visible area
        }
        const int cw = static_cast<int>(std::min<ssize_t>(src->width, pw - x));
        const int ch = static_cast<int>(std::min<ssize_t>(rows, ph - y - y0));

        // source planes at the first row
        const AVPixFmtDescriptor* desc =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src->format));
        if (!desc) {
            return false;
        }
        const uint8_t* planes[4] = {};
        for (int i = 0; i < 4 && src->data[i]; ++i) {
            const bool chroma = (i == 1 || i == 2) &&
                !(desc->flags & AV_PIX_FMT_FLAG_RGB);
            const int py = chroma ? (y0 >> desc->log2_chroma_h) : y0;
            planes[i] = src->data[i] + static_cast<ptrdiff_t>(py) *
                    src->linesize[i];
        }

        sws = sws_getCachedContext(sws, src->width, ch,
                                   static_cast<AVPixelFormat>(src->format),
                                   src->width, ch, AV_PIX_FMT_BGRA, SWS_POINT,
                                   nullptr, nullptr, nullptr);
        if (!sws) {
            return false;
        }
        sws_setColorspaceDetails(sws, coefs, full_range, coefs, 1, 0, 1 << 16,
                                 1 << 16);

        if (cw == src->width) {
            uint8_t* dst[4] = { static_cast<uint8_t*>(pm.ptr(x, y + y0)),
                                nullptr, nullptr, nullptr };
            const int dst_stride[4] = { static_cast<int>(pm.stride()), 0, 0,
                                        0 };
            return sws_scale(sws, planes, src->linesize, 0, ch, dst,
                             dst_stride) > 0;
        }

        // clipped by the right edge of the image
        const int tmp_stride = src->width * 4;
        tmp.resize(static_cast<size_t>(tmp_stride) * ch);
        uint8_t* dst[4] = { tmp.data(), nullptr, nullptr, nullptr };
        const int dst_stride[4] = { tmp_stride, 0, 0, 0 };
        if (sws_scale(sws, planes, src->linesize, 0, ch, dst, dst_stride) <=
            0) {
            return false;
        }
        for (int row = 0; row < ch; ++row) {
            std::memcpy(pm.ptr(x, y + y0 + row),
                        tmp.data() + static_cast<size_t>(row) * tmp_stride,
                        static_cast<size_t>(cw) * 4);
        }
        return true;
    }

private:
    int full_range;
    const int* coefs;
    SwsContext* sws = nullptr;
    std::vector<uint8_t> tmp;
};

/** Memory reader for libavformat. */
class MemIo {
public:
    explicit MemIo(const ImageFormat::Data& data)
        : data(data)
    {
    }

    ~MemIo()
    {
        if (io) {
            av_freep(static_cast<void*>(&io->buffer));
            avio_context_free(&io);
        }
    }

    MemIo(const MemIo&) = delete;
    MemIo& operator=(const MemIo&) = delete;

    /**
     * Create IO context.
     * @return IO context or nullptr on errors
     */
    AVIOContext* context()
    {
        constexpr size_t io_size = 64 * 1024;
        auto* buf = static_cast<unsigned char*>(av_malloc(io_size));
        if (!buf) {
            return nullptr;
        }
        io = avio_alloc_context(buf, io_size, 0, this, &read, nullptr, &seek);
        if (!io) {
            av_free(buf);
        }
        return io;
    }

private:
    static int read(void* opaque, uint8_t* buf, int buf_size)
    {
        auto* self = static_cast<MemIo*>(opaque);
        const size_t remain = self->data.size - self->pos;
        if (remain == 0) {
            return AVERROR_EOF;
        }
        const size_t sz = std::min(static_cast<size_t>(buf_size), remain);
        std::memcpy(buf, self->data.data + self->pos, sz);
        self->pos += sz;
        return static_cast<int>(sz);
    }

    static int64_t seek(void* opaque, int64_t offset, int whence)
    {
        auto* self = static_cast<MemIo*>(opaque);
        const auto size = static_cast<int64_t>(self->data.size);
        if (whence & AVSEEK_SIZE) {
            return size;
        }
        int64_t pos;
        switch (whence & ~AVSEEK_FORCE) {
            case SEEK_SET:
                pos = offset;
                break;
            case SEEK_CUR:
                pos = static_cast<int64_t>(self->pos) + offset;
                break;
            case SEEK_END:
                pos = size + offset;
                break;
            default:
                return AVERROR(EINVAL);
        }
        if (pos < 0 || pos > size) {
            return AVERROR(EINVAL);
        }
        self->pos = static_cast<size_t>(pos);
        return pos;
    }

    const ImageFormat::Data& data;
    size_t pos = 0;
    AVIOContext* io = nullptr;
};

/**
 * HEVC tile grid decoder: tiles are decoded on the GPU one by one and
 * converted to RGB by worker threads in parallel.
 */
class HeifGrid {
public:
    explicit HeifGrid(const ImageFormat::Data& data)
        : mem(data)
    {
    }

    ~HeifGrid()
    {
        if (dec) {
            avcodec_free_context(&dec);
        }
        if (fmt) {
            avformat_close_input(&fmt);
        }
    }

    HeifGrid(const HeifGrid&) = delete;
    HeifGrid& operator=(const HeifGrid&) = delete;

    /**
     * Open container and find tile grid.
     * @return true if image is a HEVC tile grid
     */
    bool open()
    {
        AVIOContext* io = mem.context();
        if (!io) {
            return false;
        }
        fmt = avformat_alloc_context();
        if (!fmt) {
            return false;
        }
        fmt->pb = io;
        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0 ||
            avformat_find_stream_info(fmt, nullptr) < 0) {
            return false;
        }

        for (unsigned i = 0; i < fmt->nb_stream_groups; ++i) {
            const AVStreamGroup* grp = fmt->stream_groups[i];
            if (grp->type == AV_STREAM_GROUP_PARAMS_TILE_GRID) {
                grid = grp->params.tile_grid;
                break;
            }
        }
        if (!grid || grid->nb_tiles == 0 || grid->width <= 0 ||
            grid->height <= 0) {
            return false;
        }
        for (unsigned t = 0; t < grid->nb_tiles; ++t) {
            if (grid->offsets[t].idx >= fmt->nb_streams) {
                return false;
            }
        }
        return fmt->streams[grid->offsets[0].idx]->codecpar->codec_id ==
            AV_CODEC_ID_HEVC;
    }

    /**
     * Decode all tiles.
     * @param pm output pixmap
     * @return true if all tiles were decoded
     */
    bool decode(Pixmap& pm)
    {
        const AVCodecParameters* par =
            fmt->streams[grid->offsets[0].idx]->codecpar;
        dec = open_decoder(par, AV_CODEC_ID_NONE, MAX_IN_FLIGHT + 2);
        if (!dec) {
            return false;
        }

        // map stream index to tile index
        std::vector<int> tile_of(fmt->nb_streams, -1);
        for (unsigned t = 0; t < grid->nb_tiles; ++t) {
            tile_of[grid->offsets[t].idx] = static_cast<int>(t);
        }

        pm.create(Pixmap::RGB, grid->width, grid->height, false);
        full_range = par->color_range == AVCOL_RANGE_JPEG;
        bt709 = par->color_space == AVCOL_SPC_BT709;

        // workers: download frames from GPU and convert to RGB
        std::vector<std::thread> workers;
        for (size_t i = 0; i < workers_count(); ++i) {
            workers.emplace_back(&HeifGrid::worker, this, &pm);
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        std::deque<int> sent; // tiles sent to decoder, in order

        const auto receive = [&](const bool flush) {
            while (!sent.empty() && !failed) {
                const int rc = avcodec_receive_frame(dec, frame);
                if (rc == AVERROR(EAGAIN) && !flush) {
                    return;
                }
                if (rc < 0) {
                    failed = true;
                    return;
                }
                std::unique_lock lock(mutex);
                cv_space.wait(lock, [this] {
                    return queue.size() < MAX_IN_FLIGHT || failed;
                });
                queue.emplace_back(sent.front(), av_frame_clone(frame));
                sent.pop_front();
                lock.unlock();
                cv_work.notify_one();
                av_frame_unref(frame);
            }
        };

        while (pkt && frame && !failed && av_read_frame(fmt, pkt) >= 0) {
            const int t =
                tile_of.size() > static_cast<size_t>(pkt->stream_index)
                ? tile_of[pkt->stream_index]
                : -1;
            if (t >= 0) {
                if (avcodec_send_packet(dec, pkt) < 0) {
                    failed = true;
                } else {
                    sent.push_back(t);
                    receive(false);
                }
            }
            av_packet_unref(pkt);
        }
        if (pkt && frame && !failed) {
            avcodec_send_packet(dec, nullptr);
            receive(true);
        }
        const bool complete = !failed && sent.empty() && pkt && frame;

        {
            const std::scoped_lock lock(mutex);
            finished = true;
        }
        cv_work.notify_all();
        for (auto& it : workers) {
            it.join();
        }
        for (auto& [t, fr] : queue) {
            av_frame_free(&fr);
        }

        av_packet_free(&pkt);
        av_frame_free(&frame);

        return complete && !failed && converted == grid->nb_tiles;
    }

private:
    /**
     * Worker thread: convert decoded tiles to RGB.
     * @param pm output pixmap
     */
    void worker(Pixmap* pm)
    {
        Converter conv(full_range, bt709);
        AVFrame* sw = av_frame_alloc();

        while (true) {
            std::pair<int, AVFrame*> job;
            {
                std::unique_lock lock(mutex);
                cv_work.wait(lock, [this] {
                    return !queue.empty() || finished || failed;
                });
                if (queue.empty() || failed) {
                    break;
                }
                job = queue.front();
                queue.pop_front();
            }
            cv_space.notify_one();

            AVFrame* src = job.second;
            bool ok = sw != nullptr;
            if (ok && src->format == AV_PIX_FMT_VAAPI) {
                ok = av_hwframe_transfer_data(sw, src, 0) >= 0;
                src = sw;
            }
            if (ok) {
                const auto& off = grid->offsets[job.first];
                ok = conv.convert(
                    src, 0, src->height, *pm,
                    off.horizontal -
                        static_cast<ssize_t>(grid->horizontal_offset),
                    off.vertical - static_cast<ssize_t>(grid->vertical_offset));
            }
            av_frame_free(&job.second);
            av_frame_unref(sw);

            const std::scoped_lock lock(mutex);
            if (ok) {
                ++converted;
            } else {
                failed = true;
                cv_space.notify_all();
                cv_work.notify_all();
            }
        }

        av_frame_free(&sw);
    }

private:
    static constexpr size_t MAX_IN_FLIGHT = 12; ///< Max queued frames

    MemIo mem;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    const AVStreamGroupTileGrid* grid = nullptr;

    bool full_range = false; ///< Full range YUV
    bool bt709 = false;      ///< BT.709 color matrix

    std::mutex mutex;                 ///< Queue sync
    std::condition_variable cv_work;  ///< Queue has jobs
    std::condition_variable cv_space; ///< Queue has free space
    std::deque<std::pair<int, AVFrame*>> queue; ///< Decoded tiles
    bool finished = false;            ///< No more tiles will be queued
    bool failed = false;              ///< Error occurred
    size_t converted = 0;             ///< Number of converted tiles
};

/**
 * Convert large frame to RGB using multiple threads.
 * @param src frame in system memory
 * @param full_range,bt709 color description
 * @param pm output pixmap
 * @return false on errors
 */
bool convert_parallel(const AVFrame* src, const bool full_range,
                      const bool bt709, Pixmap& pm)
{
    const size_t nthreads = workers_count();
    // band height must be aligned to the chroma subsampling
    const int band = static_cast<int>(
        ((src->height + nthreads - 1) / nthreads + 15) & ~15);

    std::vector<std::thread> workers;
    std::vector<char> results(nthreads, 0);
    for (size_t i = 0; i < nthreads; ++i) {
        workers.emplace_back([&, i] {
            const int y0 = static_cast<int>(i) * band;
            const int rows = std::min(band, src->height - y0);
            if (rows <= 0) {
                results[i] = 1;
                return;
            }
            Converter conv(full_range, bt709);
            results[i] = conv.convert(src, y0, rows, pm, 0, 0);
        });
    }
    for (auto& it : workers) {
        it.join();
    }
    return std::ranges::all_of(results, [](const char r) {
        return r != 0;
    });
}

/**
 * GPU video processing pipeline converting decoded surfaces to NV12.
 * Output surface is backed by system memory (user pointer) if supported,
 * so no copy from GPU memory is needed. The pipeline is created once per
 * thread and reused while the image size is the same (photos from one
 * camera), creation of large surfaces is expensive.
 */
struct VppPipeline {
    VADisplay dpy = nullptr;
    int width = 0;
    int height = 0;
    VAConfigID config = VA_INVALID_ID;
    VAContextID ctx = VA_INVALID_ID;
    VASurfaceID out = VA_INVALID_SURFACE;
    uint8_t* mem = nullptr; ///< User memory of the output surface
    size_t pitch = 0;       ///< Row size of the output surface
    size_t uv_offset = 0;   ///< Offset of the UV plane

    VppPipeline() = default;
    VppPipeline(const VppPipeline&) = delete;
    VppPipeline& operator=(const VppPipeline&) = delete;
    ~VppPipeline() { reset(); }

    void reset()
    {
        if (ctx != VA_INVALID_ID) {
            vaDestroyContext(dpy, ctx);
            ctx = VA_INVALID_ID;
        }
        if (out != VA_INVALID_SURFACE) {
            vaDestroySurfaces(dpy, &out, 1);
            out = VA_INVALID_SURFACE;
        }
        if (config != VA_INVALID_ID) {
            vaDestroyConfig(dpy, config);
            config = VA_INVALID_ID;
        }
        std::free(mem);
        mem = nullptr;
        width = height = 0;
    }

    bool init(VADisplay display, const int w, const int h)
    {
        if (dpy == display && width == w && height == h) {
            return true;
        }
        reset();
        dpy = display;
        if (vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, nullptr,
                           0, &config) != VA_STATUS_SUCCESS) {
            config = VA_INVALID_ID;
            return false;
        }
        if (!create_user_surface(w, h)) {
            // fallback: GPU memory, mapped after conversion
            VASurfaceAttrib attr {};
            attr.type = VASurfaceAttribPixelFormat;
            attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
            attr.value.type = VAGenericValueTypeInteger;
            attr.value.value.i = VA_FOURCC_NV12;
            if (vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, w, h, &out, 1,
                                 &attr, 1) != VA_STATUS_SUCCESS) {
                out = VA_INVALID_SURFACE;
                reset();
                return false;
            }
        }
        if (vaCreateContext(dpy, config, w, h, VA_PROGRESSIVE, &out, 1,
                            &ctx) != VA_STATUS_SUCCESS) {
            ctx = VA_INVALID_ID;
            reset();
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    bool create_user_surface(const int w, const int h)
    {
        constexpr size_t page = 4096;
        pitch = (static_cast<size_t>(w) + 127) & ~static_cast<size_t>(127);
        const size_t rows = (static_cast<size_t>(h) + 31) & ~31UL;
        uv_offset = pitch * rows;
        const size_t size = (uv_offset * 3 / 2 + page - 1) & ~(page - 1);
        mem = static_cast<uint8_t*>(std::aligned_alloc(page, size));
        if (!mem) {
            return false;
        }

        uintptr_t buffer = reinterpret_cast<uintptr_t>(mem);
        VASurfaceAttribExternalBuffers ext {};
        ext.pixel_format = VA_FOURCC_NV12;
        ext.width = w;
        ext.height = h;
        ext.data_size = size;
        ext.num_planes = 2;
        ext.pitches[0] = ext.pitches[1] = pitch;
        ext.offsets[0] = 0;
        ext.offsets[1] = uv_offset;
        ext.buffers = &buffer;
        ext.num_buffers = 1;

        VASurfaceAttrib attrs[3] {};
        attrs[0].type = VASurfaceAttribMemoryType;
        attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attrs[0].value.type = VAGenericValueTypeInteger;
        attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR;
        attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
        attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attrs[1].value.type = VAGenericValueTypePointer;
        attrs[1].value.value.p = &ext;
        attrs[2].type = VASurfaceAttribPixelFormat;
        attrs[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attrs[2].value.type = VAGenericValueTypeInteger;
        attrs[2].value.value.i = VA_FOURCC_NV12;

        if (vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, w, h, &out, 1, attrs,
                             3) != VA_STATUS_SUCCESS) {
            out = VA_INVALID_SURFACE;
            std::free(mem);
            mem = nullptr;
            return false;
        }
        return true;
    }
};

/**
 * Convert NV12 planes to the pixmap.
 * @return false on errors
 */
bool convert_nv12(uint8_t* y, uint8_t* uv, const size_t y_pitch,
                  const size_t uv_pitch, Pixmap& pm)
{
    AVFrame* view = av_frame_alloc();
    if (!view) {
        return false;
    }
    view->format = AV_PIX_FMT_NV12;
    view->width = static_cast<int>(pm.width());
    view->height = static_cast<int>(pm.height());
    view->data[0] = y;
    view->data[1] = uv;
    view->linesize[0] = static_cast<int>(y_pitch);
    view->linesize[1] = static_cast<int>(uv_pitch);
    const bool ok = convert_parallel(view, true, false, pm);
    av_frame_free(&view);
    return ok;
}

/**
 * Download decoded frame via GPU conversion to NV12: reading back the
 * decoder's native surfaces (e.g. 4:2:2 planar of JPEG) is very slow.
 * @param frame decoded VAAPI frame
 * @param pm output pixmap
 * @param orientation EXIF orientation to apply on GPU
 * @return false on errors
 */
bool download_nv12(const AVFrame* frame, Pixmap& pm, const int orientation)
{
    thread_local VppPipeline vpp;

    const auto* hwdev =
        reinterpret_cast<const AVHWDeviceContext*>(device()->data);
    VADisplay dpy = static_cast<AVVAAPIDeviceContext*>(hwdev->hwctx)->display;
    const auto in =
        static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
    // EXIF orientation: clockwise rotation, then mirroring (as in VA-API)
    uint32_t rotation = VA_ROTATION_NONE;
    uint32_t mirror = VA_MIRROR_NONE;
    switch (orientation) {
        case 2:
            mirror = VA_MIRROR_HORIZONTAL;
            break;
        case 3:
            rotation = VA_ROTATION_180;
            break;
        case 4:
            mirror = VA_MIRROR_VERTICAL;
            break;
        case 5:
            rotation = VA_ROTATION_90;
            mirror = VA_MIRROR_HORIZONTAL;
            break;
        case 6:
            rotation = VA_ROTATION_90;
            break;
        case 7:
            rotation = VA_ROTATION_90;
            mirror = VA_MIRROR_VERTICAL;
            break;
        case 8:
            rotation = VA_ROTATION_270;
            break;
        default:
            break;
    }
    const bool swap = rotation == VA_ROTATION_90 ||
        rotation == VA_ROTATION_270;
    const int width = swap ? frame->height : frame->width;
    const int height = swap ? frame->width : frame->height;

    if (!vpp.init(dpy, width, height)) {
        return false;
    }

    const bool full = frame->color_range != AVCOL_RANGE_MPEG;
    const VARectangle src_region { 0, 0, static_cast<uint16_t>(frame->width),
                                   static_cast<uint16_t>(frame->height) };
    const VARectangle dst_region { 0, 0, static_cast<uint16_t>(width),
                                   static_cast<uint16_t>(height) };
    VAProcPipelineParameterBuffer pp {};
    pp.surface = in;
    pp.surface_region = &src_region;
    pp.output_region = &dst_region;
    pp.rotation_state = rotation;
    pp.mirror_state = mirror;
    pp.output_background_color = 0xff000000;
    pp.surface_color_standard = VAProcColorStandardBT601;
    pp.output_color_standard = VAProcColorStandardBT601;
    pp.input_color_properties.color_range =
        full ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
    pp.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;

    VABufferID params = VA_INVALID_ID;
    if (vaCreateBuffer(dpy, vpp.ctx, VAProcPipelineParameterBufferType,
                       sizeof(pp), 1, &pp, &params) != VA_STATUS_SUCCESS) {
        return false;
    }
    const bool rendered = vaBeginPicture(dpy, vpp.ctx, vpp.out) ==
            VA_STATUS_SUCCESS &&
        vaRenderPicture(dpy, vpp.ctx, &params, 1) == VA_STATUS_SUCCESS &&
        vaEndPicture(dpy, vpp.ctx) == VA_STATUS_SUCCESS;
    vaDestroyBuffer(dpy, params);
    if (!rendered) {
        vpp.reset();
        return false;
    }

    // allocate output while GPU decodes and converts the image
    pm.create(Pixmap::RGB, width, height, false);

    if (vaSyncSurface(dpy, vpp.out) != VA_STATUS_SUCCESS) {
        vpp.reset();
        return false;
    }

    if (vpp.mem) {
        // output is already in system memory
        return convert_nv12(vpp.mem, vpp.mem + vpp.uv_offset, vpp.pitch,
                            vpp.pitch, pm);
    }

    VAImage image {};
    if (vaDeriveImage(dpy, vpp.out, &image) != VA_STATUS_SUCCESS) {
        return false;
    }
    bool ok = false;
    void* mapped = nullptr;
    if (image.format.fourcc == VA_FOURCC_NV12 &&
        vaMapBuffer(dpy, image.buf, &mapped) == VA_STATUS_SUCCESS) {
        auto* base = static_cast<uint8_t*>(mapped);
        ok = convert_nv12(base + image.offsets[0], base + image.offsets[1],
                          image.pitches[0], image.pitches[1], pm);
        vaUnmapBuffer(dpy, image.buf);
    }
    vaDestroyImage(dpy, image.image_id);

    return ok;
}

} // namespace

namespace HwDecode {

void warmup()
{
    device();
}

bool heif_grid(const ImageFormat::Data& data, Pixmap& pm)
{
    if (!device()) {
        return false;
    }
    HeifGrid grid(data);
    return grid.open() && grid.decode(pm);
}

bool jpeg(const ImageFormat::Data& data, Pixmap& pm, const int orientation)
{
    // decoder is reused: it keeps GPU surfaces allocated between images
    struct Decoder {
        AVCodecContext* ctx = nullptr;
        ~Decoder()
        {
            if (ctx) {
                avcodec_free_context(&ctx);
            }
        }
    };
    thread_local Decoder dec;
    if (!dec.ctx) {
        dec.ctx = open_decoder(nullptr, AV_CODEC_ID_MJPEG, 0);
        if (!dec.ctx) {
            return false;
        }
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = false;

    // decoder requires padded input buffer
    if (pkt && frame &&
        av_new_packet(pkt, static_cast<int>(data.size)) == 0) {
        std::memcpy(pkt->data, data.data, data.size);
        if (avcodec_send_packet(dec.ctx, pkt) == 0) {
            int rc = avcodec_receive_frame(dec.ctx, frame);
            if (rc == AVERROR(EAGAIN)) {
                avcodec_send_packet(dec.ctx, nullptr); // drain
                rc = avcodec_receive_frame(dec.ctx, frame);
                avcodec_flush_buffers(dec.ctx);
            }
            if (rc == 0 && frame->format == AV_PIX_FMT_VAAPI) {
                ok = download_nv12(frame, pm, orientation);
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (!ok) {
        // start from scratch next time
        avcodec_free_context(&dec.ctx);
    }

    return ok;
}

} // namespace HwDecode
