// SPDX-License-Identifier: MIT
// Lazy loading of heavy shared libraries.

#include "lazylib.hpp"

#include "log.hpp"

#include <dlfcn.h>

#include <cstdlib>

LazyLibrary::LazyLibrary(std::string soname)
    : soname(std::move(soname))
{
}

bool LazyLibrary::load()
{
    std::call_once(once, [this] {
        const Log::PerfTimer timer;
        handle = dlopen(soname.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            Log::error("Unable to load {}: {}", soname, dlerror());
        } else if (Log::verbose_enable()) {
            Log::verbose("Library {} loaded in {:.6f} sec", soname,
                         timer.time());
        }
    });
    return handle;
}

void* LazyLibrary::symbol(const char* name)
{
    void* addr = load() ? dlsym(handle, name) : nullptr;
    if (!addr) {
        Log::error("Function {} not found in {}", name, soname);
        std::abort();
    }
    return addr;
}
