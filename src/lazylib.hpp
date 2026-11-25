// SPDX-License-Identifier: MIT
// Lazy loading of heavy shared libraries.
//
// libmpv and FFmpeg pull in ~100 shared libraries (Vulkan, shader compilers,
// audio servers, codecs...), loading and relocating them takes a significant
// part of the application startup time. These libraries are not linked to
// the executable: the functions used by swayimg are defined as stubs (see
// lazy_*.cpp) which load the library on the first call and forward the call
// to the real function.

#pragma once

#include <mutex>
#include <string>

/** Shared library loaded on demand. */
class LazyLibrary {
public:
    /**
     * Constructor.
     * @param soname library file name
     */
    explicit LazyLibrary(std::string soname);

    LazyLibrary(const LazyLibrary&) = delete;
    LazyLibrary& operator=(const LazyLibrary&) = delete;

    /**
     * Load library (thread-safe, the library is loaded once).
     * @return false if library is not available
     */
    bool load();

    /**
     * Get address of the function.
     * The process is aborted if the library or the function is not
     * available: callers must check load() before using the library.
     * @param name function name
     * @return function address
     */
    void* symbol(const char* name);

private:
    const std::string soname;   ///< Library file name
    std::once_flag once;        ///< Load once flag
    void* handle = nullptr;     ///< Library handle
};

/**
 * Define stub for the function of lazy loaded library.
 * Signature is checked by the compiler against the library header.
 * @param lib function returning LazyLibrary instance
 * @param ret return type
 * @param name function name
 * @param params parameters list in parentheses
 * @param args arguments list in parentheses
 */
#define LAZY_STUB(lib, ret, name, params, args)                            \
    extern "C" ret name params                                             \
    {                                                                      \
        using Fn = ret(*) params;                                          \
        static const Fn fn = reinterpret_cast<Fn>(lib().symbol(#name));    \
        return fn args;                                                    \
    }

namespace Lazy {

/**
 * Load libmpv.
 * @return false if libmpv is not available
 */
bool mpv();

/**
 * Load FFmpeg libraries.
 * @return false if FFmpeg is not available
 */
bool ffmpeg();

} // namespace Lazy
