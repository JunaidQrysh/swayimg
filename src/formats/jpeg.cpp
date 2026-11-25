// SPDX-License-Identifier: MIT
// JPEG image format.
// Copyright (C) 2020 Artem Senichev <artemsen@gmail.com>

#include "../imageformat.hpp"
#include "../log.hpp"
#include "buildconf.hpp"

#ifdef HAVE_HWDECODE
#include "../formatfactory.hpp"
#include "../hwdecode.hpp"
#endif

#include <jpeglib.h>

#include <csetjmp>
#include <cstring>
#include <format>

namespace {

class ImageFormatJpeg : public ImageFormat {
public:
    ImageFormatJpeg() noexcept
        : ImageFormat(Priority::Highest, "jpg")
    {
    }

    [[nodiscard]] ImagePtr decode(const Data& data) const override
    {
        if (!check_signature(data, { 0xff, 0xd8 })) {
            return nullptr;
        }

        ImagePtr image = std::make_shared<Image>();

#ifdef HAVE_HWDECODE
        // large photos: decode on GPU
        int orientation = 0;
        if (hw_supported(data, orientation)) {
            // let GPU apply EXIF orientation as well
            if (!FormatFactory::self().fix_orientation) {
                orientation = 0;
            }
            image->frames.resize(1);
            if (HwDecode::jpeg(data, image->frames[0].pm, orientation)) {
                image->format = "JPEG 24bit";
                if (orientation > 1) {
                    image->meta[HW_ORIENTED] = "1";
                }
                return image;
            }
            image->frames.clear();
            Log::verbose("JPEG: hardware decoding failed, use libjpeg");
        }
#endif

        jpeg_decompress_struct jpg;

        // setup error handling
        Error err;
        jpg.err = jpeg_std_error(&err.manager);
        err.manager.error_exit = jpg_error_exit;
        if (setjmp(err.jump)) {
            jpeg_destroy_decompress(&jpg);
            return nullptr;
        }

        jpeg_create_decompress(&jpg);
        jpeg_mem_src(&jpg, data.data, data.size);
        jpeg_read_header(&jpg, TRUE);

#ifdef LIBJPEG_TURBO_VERSION
        switch (jpg.jpeg_color_space) {
            case JCS_CMYK:
            case JCS_YCCK:
                jpg.out_color_space = JCS_CMYK;
                break;
            case JCS_UNKNOWN:
                break;
            default:
                jpg.out_color_space = JCS_EXT_BGRA;
                break;
        }
#endif // LIBJPEG_TURBO_VERSION

        jpeg_start_decompress(&jpg);

        // allocate frame
        image->frames.resize(1);
        Pixmap& pm = image->frames[0].pm;
        pm.create(Pixmap::RGB, jpg.output_width, jpg.output_height, false);

        while (jpg.output_scanline < jpg.output_height) {
            argb_t* line = &pm.at(0, jpg.output_scanline);
            jpeg_read_scanlines(&jpg, reinterpret_cast<JSAMPARRAY>(&line), 1);

            // convert to argb
            if (jpg.out_color_components == 1) { // grayscale
                argb_t* pixel = line;
                for (int x = jpg.output_width - 1; x >= 0; --x) {
                    const uint8_t color =
                        *(reinterpret_cast<const uint8_t*>(line) + x);
                    pixel[x].a = argb_t::max;
                    pixel[x].r = color;
                    pixel[x].g = color;
                    pixel[x].b = color;
                }
            } else if (jpg.out_color_components == 3) { // rgb
                argb_t* pixel = line;
                for (int x = jpg.output_width - 1; x >= 0; --x) {
                    const uint8_t* color =
                        reinterpret_cast<const uint8_t*>(line) + x * 3UL;
                    pixel[x].a = argb_t::max;
                    pixel[x].r = color[0];
                    pixel[x].g = color[1];
                    pixel[x].b = color[2];
                }
            } else if (jpg.out_color_space == JCS_CMYK) {
                argb_t* pixel = line;
                for (size_t x = 0; x < pm.width(); ++x) {
                    const uint8_t* color =
                        reinterpret_cast<const uint8_t*>(line) + x * 4UL;
                    const double c = color[0];
                    const double m = color[1];
                    const double y = color[2];
                    const double k = color[3];
                    pixel[x].a = argb_t::max;
                    pixel[x].r = c * k / argb_t::max;
                    pixel[x].g = m * k / argb_t::max;
                    pixel[x].b = y * k / argb_t::max;
                }
            }
        }

        image->format = std::format("JPEG {}bit", jpg.num_components * 8);

        jpeg_finish_decompress(&jpg);
        jpeg_destroy_decompress(&jpg);

        return image;
    }

private:
#ifdef HAVE_HWDECODE
    /** Meta key: orientation was fixed by hardware decoder. */
    static constexpr const char* HW_ORIENTED = "swayimg.hw_oriented";

    void fix_orientation(ImagePtr& image, const int orientation) const override
    {
        const auto it = image->meta.find(HW_ORIENTED);
        if (it != image->meta.end()) {
            image->meta.erase(it); // already done
            return;
        }
        ImageFormat::fix_orientation(image, orientation);
    }

    /**
     * Read EXIF orientation from APP1 segment.
     * @param app1 segment data (after length field)
     * @param size segment size
     * @return orientation or 0 if not found
     */
    static int exif_orientation(const uint8_t* app1, const size_t size)
    {
        if (size < 14 || std::memcmp(app1, "Exif\0\0", 6) != 0) {
            return 0;
        }
        const uint8_t* tiff = app1 + 6;
        const size_t len = size - 6;
        const bool le = tiff[0] == 'I' && tiff[1] == 'I';
        if (!le && !(tiff[0] == 'M' && tiff[1] == 'M')) {
            return 0;
        }
        const auto u16 = [&](const size_t off) -> uint32_t {
            return le ? tiff[off] | (tiff[off + 1] << 8)
                      : (tiff[off] << 8) | tiff[off + 1];
        };
        const auto u32 = [&](const size_t off) -> uint32_t {
            return le ? u16(off) | (u16(off + 2) << 16)
                      : (u16(off) << 16) | u16(off + 2);
        };
        const size_t ifd = u32(4);
        if (ifd + 2 > len) {
            return 0;
        }
        const size_t entries = u16(ifd);
        for (size_t i = 0; i < entries; ++i) {
            const size_t entry = ifd + 2 + i * 12;
            if (entry + 12 > len) {
                break;
            }
            if (u16(entry) == 0x0112) { // orientation, SHORT
                const uint32_t val = u16(entry + 8);
                return val <= 8 ? static_cast<int>(val) : 0;
            }
        }
        return 0;
    }

    /**
     * Check if JPEG can be decoded on GPU: large baseline 8-bit YCbCr
     * or grayscale images (hardware doesn't support progressive mode).
     * @param data raw JPEG data
     * @param orientation output EXIF orientation (0 if not specified)
     * @return true if hardware decoding is worth trying
     */
    static bool hw_supported(const Data& data, int& orientation)
    {
        // smaller images are decoded fast enough by CPU
        constexpr size_t min_pixels = 4 * 1000 * 1000;

        size_t pos = 2; // skip SOI
        while (pos + 4 <= data.size) {
            if (data.data[pos] != 0xff) {
                return false;
            }
            const uint8_t marker = data.data[pos + 1];
            if (marker == 0xff) {
                ++pos; // fill byte
                continue;
            }
            const size_t len = (data.data[pos + 2] << 8) | data.data[pos + 3];
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
                marker != 0xc8 && marker != 0xcc) {
                // start of frame
                if (marker != 0xc0 && marker != 0xc1) {
                    return false; // progressive, lossless, arithmetic
                }
                if (pos + 10 > data.size) {
                    return false;
                }
                const uint8_t* sof = data.data + pos + 4;
                const size_t height = (sof[1] << 8) | sof[2];
                const size_t width = (sof[3] << 8) | sof[4];
                const uint8_t components = sof[5];
                return sof[0] == 8 && (components == 1 || components == 3) &&
                    width * height >= min_pixels;
            }
            if (marker == 0xda || len < 2) {
                return false; // start of scan before frame header
            }
            if (marker == 0xe1 && !orientation && pos + 2 + len <= data.size) {
                orientation = exif_orientation(data.data + pos + 4, len - 2);
            }
            pos += 2 + len;
        }
        return false;
    }
#endif

    /** JPG error description. */
    struct Error {
        jpeg_error_mgr manager;
        jmp_buf jump;
    };

    /** JPEG error callback. */
    static void jpg_error_exit(j_common_ptr jpg)
    {
        Error* err = reinterpret_cast<Error*>(jpg->err);
        char msg[JMSG_LENGTH_MAX] = { 0 };
        (*jpg->err->format_message)(jpg, msg);
        Log::error("JPEG: {}", msg);
        longjmp(err->jump, 1);
    }
};

// register format in factory
ImageFormatJpeg format_jpeg;

} // anonymous namespace
