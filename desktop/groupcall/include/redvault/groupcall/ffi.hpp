// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// LiveKit's prebuilt native client library (livekit-ffi), driven with its
// protobuf protocol (desktop/third_party/livekit-ffi/protocol, pinned to the
// same release as the binary). One instance per process: the library is loaded
// the first time it's needed, so an app without it still runs, and only group
// calls report it missing.
//
// The library calls back from threads of its own. Everything it sends is
// copied into a queue and handled, in order, on one dispatcher thread, so a
// listener never runs inside the library and may call back into it.

namespace livekit::proto {
class FfiEvent;
class FfiRequest;
class FfiResponse;
}  // namespace livekit::proto

namespace redvault::lk {

class FfiError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Ffi {
public:
    /// Folders to look for the library in, in order (usually the program's own folder). Call before first use.
    static void setSearchDirectories(std::vector<std::filesystem::path> directories);
    /// The library file is in one of the search folders (it isn't opened or started).
    static bool libraryInstalled();
    /// True if the library can be loaded; otherwise says why in `reason`.
    static bool available(std::string* reason = nullptr);
    /// The loaded library, initialized on first use. Throws FfiError if it isn't there.
    static Ffi& get();
    /// Winds the library down at program exit. A no-op if it was never used.
    static void shutdown();

    /// One synchronous request. Throws FfiError if the library rejects it.
    ::livekit::proto::FfiResponse request(const ::livekit::proto::FfiRequest& request);
    /// Releases something the library handed out (a track, a video frame, a stream...). Zero is ignored.
    void dropHandle(std::uint64_t handle);
    /// A number for an asynchronous request, so its completion can be expected before it is sent.
    std::uint64_t nextAsyncId();

    using Listener = std::function<void(const ::livekit::proto::FfiEvent&)>;
    /// Called for every event, on the dispatcher thread. Returns an id for removeListener.
    int addListener(Listener listener);
    void removeListener(int id);

    /// The event that completes asynchronous request `asyncId` (a connect, a publish...). Register before sending.
    std::future<std::shared_ptr<::livekit::proto::FfiEvent>> expect(std::uint64_t asyncId);
    /// Gives up on an expectation that will never be met (the request failed to send).
    void cancel(std::uint64_t asyncId);

    /// What the library logs (warnings and errors only), for diagnostics; never written anywhere by default.
    using LogSink = std::function<void(const std::string& level, const std::string& target, const std::string& message)>;
    void setLogSink(LogSink sink);

    struct Impl;

private:
    Ffi();
    ~Ffi();
    std::unique_ptr<Impl> impl_;
};

}  // namespace redvault::lk
