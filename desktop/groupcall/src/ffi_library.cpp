// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "ffi_library.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace redvault::lk::detail {

namespace fs = std::filesystem;

const char* FfiLibrary::fileName() {
#ifdef _WIN32
    return "livekit_ffi.dll";
#else
    return "liblivekit_ffi.so";
#endif
}

namespace {

#ifdef _WIN32
std::string lastError() {
    return "Windows error " + std::to_string(GetLastError());
}
#endif

}  // namespace

FfiLibrary::~FfiLibrary() {
    // Never unloaded while the process runs: the library owns threads that may still be winding down.
}

std::unique_ptr<FfiLibrary> FfiLibrary::load(const std::vector<fs::path>& directories, std::string& error) {
    std::error_code ec;
    for (const auto& dir : directories) {
        const fs::path file = dir / fileName();
        if (!fs::exists(file, ec)) continue;
        std::unique_ptr<FfiLibrary> lib(new FfiLibrary());
#ifdef _WIN32
        // The file's full path, with its own folder searched for anything it needs.
        HMODULE module = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) {
            error = "Couldn't load " + file.string() + " (" + lastError() + ")";
            continue;
        }
        lib->handle_ = module;
        auto symbol = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(module, name)); };
#else
        void* module = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            const char* why = dlerror();
            error = "Couldn't load " + file.string() + " (" + (why ? why : "unknown error") + ")";
            continue;
        }
        lib->handle_ = module;
        auto symbol = [&](const char* name) { return dlsym(module, name); };
#endif
        lib->initialize = reinterpret_cast<InitializeFn>(symbol("livekit_ffi_initialize"));
        lib->request = reinterpret_cast<RequestFn>(symbol("livekit_ffi_request"));
        lib->dropHandle = reinterpret_cast<DropHandleFn>(symbol("livekit_ffi_drop_handle"));
        if (!lib->initialize || !lib->request || !lib->dropHandle) {
            error = file.string() + " isn't a livekit-ffi library this build understands";
            lib->handle_ = nullptr;  // leave it loaded: it may have started threads
            continue;
        }
        error.clear();
        return lib;
    }
    if (error.empty()) error = std::string(fileName()) + " isn't installed with this app";
    return nullptr;
}

}  // namespace redvault::lk::detail
