#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The prebuilt livekit-ffi library, opened at run time (LoadLibrary / dlopen).
// There is no import library and nothing links against it: the app starts and
// runs without it, and only group calls need it. This file has no protobuf and
// no Windows headers in its interface, so the generated code can't clash with
// <windows.h>'s macros.
namespace redvault::lk::detail {

struct FfiLibrary {
    using EventCallback = void (*)(const std::uint8_t*, std::size_t);
    using InitializeFn = void (*)(EventCallback callback, bool captureLogs, const char* sdk, const char* sdkVersion);
    using RequestFn = std::uint64_t (*)(const std::uint8_t* data, std::size_t len, const std::uint8_t** resPtr, std::size_t* resLen);
    using DropHandleFn = bool (*)(std::uint64_t handle);

    InitializeFn initialize = nullptr;
    RequestFn request = nullptr;
    DropHandleFn dropHandle = nullptr;

    /// The file name for this platform: livekit_ffi.dll or liblivekit_ffi.so.
    static const char* fileName();
    /// Opens the library from the first directory that has it. Null (and `error` filled) if none does.
    static std::unique_ptr<FfiLibrary> load(const std::vector<std::filesystem::path>& directories, std::string& error);

    ~FfiLibrary();
    FfiLibrary(const FfiLibrary&) = delete;
    FfiLibrary& operator=(const FfiLibrary&) = delete;

private:
    FfiLibrary() = default;
    void* handle_ = nullptr;
};

}  // namespace redvault::lk::detail
