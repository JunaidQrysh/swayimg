// SPDX-License-Identifier: MIT
// Freedesktop.org thumbnail cache reader.
// https://specifications.freedesktop.org/thumbnail-spec/latest/

#include "fdothumb.hpp"

#include "formatfactory.hpp"

#include <sys/stat.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/** Minimal MD5 implementation (RFC 1321), returns lowercase hex digest. */
std::string md5_hex(const std::string& msg)
{
    static constexpr std::array<uint32_t, 64> K = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf,
        0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e,
        0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
        0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
        0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static constexpr std::array<uint32_t, 64> S = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };

    // padding
    std::string data = msg;
    const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) {
        data.push_back(0);
    }
    for (size_t i = 0; i < 8; ++i) {
        data.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));
    }

    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe,
             h3 = 0x10325476;

    for (size_t off = 0; off < data.size(); off += 64) {
        std::array<uint32_t, 16> m;
        for (size_t i = 0; i < 16; ++i) {
            const auto* p =
                reinterpret_cast<const uint8_t*>(data.data() + off + i * 4);
            m[i] = p[0] | (p[1] << 8) | (p[2] << 16) |
                (static_cast<uint32_t>(p[3]) << 24);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t f;
            uint32_t g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const uint32_t tmp = d;
            d = c;
            c = b;
            const uint32_t x = a + f + K[i] + m[g];
            b = b + ((x << S[i]) | (x >> (32 - S[i])));
            a = tmp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
    }

    static constexpr const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const uint32_t h : { h0, h1, h2, h3 }) {
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t byte = (h >> (i * 8)) & 0xff;
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0xf]);
        }
    }
    return out;
}

/** Build canonical file:// URI for the path (percent-encoded). */
std::string file_uri(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path abs =
        std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) {
        return {};
    }

    static constexpr const char* hex = "0123456789ABCDEF";
    std::string uri = "file://";
    for (const char ch : abs.string()) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            uri.push_back(ch);
        } else {
            uri.push_back('%');
            uri.push_back(hex[c >> 4]);
            uri.push_back(hex[c & 0xf]);
        }
    }
    return uri;
}

/** Get base directory of the thumbnail cache. */
std::filesystem::path cache_dir()
{
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        return std::filesystem::path(xdg) / "thumbnails";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".cache" / "thumbnails";
    }
    return {};
}

} // namespace

namespace FdoThumb {

Pixmap load(const std::filesystem::path& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == -1 || !S_ISREG(st.st_mode)) {
        return {};
    }

    const std::string uri = file_uri(path);
    const std::filesystem::path base = cache_dir();
    if (uri.empty() || base.empty()) {
        return {};
    }

    // use non-throwing functions, this runs in a thread pool
    const std::filesystem::path thumb_path =
        base / "x-large" / (md5_hex(uri) + ".png");
    std::error_code ec;
    if (!std::filesystem::exists(thumb_path, ec)) {
        return {};
    }

    const ImageEntryPtr thumb_entry = std::make_shared<ImageEntry>();
    thumb_entry->path = thumb_path;
    const ImagePtr thumb = FormatFactory::self().load(thumb_entry);
    if (!thumb || thumb->frames.empty()) {
        return {};
    }

    // validate thumbnail against the origin file
    const auto it_uri = thumb->meta.find("Thumb::URI");
    const auto it_mtime = thumb->meta.find("Thumb::MTime");
    if (it_uri == thumb->meta.end() || it_mtime == thumb->meta.end() ||
        it_uri->second != uri) {
        return {};
    }
    char* end = nullptr;
    const long long mtime = std::strtoll(it_mtime->second.c_str(), &end, 10);
    if (end == it_mtime->second.c_str() || mtime != st.st_mtim.tv_sec) {
        return {};
    }

    return thumb->frames[0].pm;
}

} // namespace FdoThumb
