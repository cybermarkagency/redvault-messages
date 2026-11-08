// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/groupcall/ffi.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "ffi.pb.h"
#include "ffi_library.hpp"

namespace redvault::lk {

namespace pb = ::livekit::proto;

namespace {

std::mutex g_mutex;
std::vector<std::filesystem::path> g_directories;
Ffi* g_instance = nullptr;
std::string g_error;
std::atomic<Ffi::Impl*> g_impl{nullptr};

/// The async request an event completes, if it completes one.
std::optional<std::uint64_t> completedAsyncId(const pb::FfiEvent& e) {
    switch (e.message_case()) {
        case pb::FfiEvent::kConnect: return e.connect().async_id();
        case pb::FfiEvent::kDisconnect: return e.disconnect().async_id();
        case pb::FfiEvent::kDispose: return e.dispose().async_id();
        case pb::FfiEvent::kPublishTrack: return e.publish_track().async_id();
        case pb::FfiEvent::kUnpublishTrack: return e.unpublish_track().async_id();
        case pb::FfiEvent::kCaptureAudioFrame: return e.capture_audio_frame().async_id();
        default: return std::nullopt;
    }
}

}  // namespace

struct Ffi::Impl {
    std::unique_ptr<detail::FfiLibrary> lib;
    std::thread dispatcher;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::vector<std::uint8_t>> queue;
    bool stopping = false;

    std::mutex listenersMutex;
    std::vector<std::pair<int, std::shared_ptr<Listener>>> listeners;
    int nextListenerId = 1;

    std::mutex waitersMutex;
    std::map<std::uint64_t, std::promise<std::shared_ptr<pb::FfiEvent>>> waiters;

    std::atomic<std::uint64_t> asyncCounter{1ull << 40};

    std::mutex logMutex;
    LogSink logSink;

    /// The library's callback: copy and queue, never block, never call back in.
    static void onEvent(const std::uint8_t* data, std::size_t len) {
        Impl* self = g_impl.load();
        if (!self || !data) return;
        std::vector<std::uint8_t> copy(data, data + len);
        {
            std::lock_guard lock(self->queueMutex);
            if (self->stopping) return;
            self->queue.push_back(std::move(copy));
        }
        self->queueCv.notify_one();
    }

    void run() {
        for (;;) {
            std::vector<std::uint8_t> bytes;
            {
                std::unique_lock lock(queueMutex);
                queueCv.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) return;  // stopping, and drained
                bytes = std::move(queue.front());
                queue.pop_front();
            }
            auto event = std::make_shared<pb::FfiEvent>();
            if (!event->ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) continue;
            dispatch(event);
        }
    }

    LogSink currentLogSink() {
        std::lock_guard lock(logMutex);
        return logSink;
    }

    void dispatch(const std::shared_ptr<pb::FfiEvent>& event) {
        if (event->message_case() == pb::FfiEvent::kLogs) {
            if (const LogSink sink = currentLogSink()) {
                static const char* levels[] = {"error", "warn", "info", "debug", "trace"};
                for (const auto& r : event->logs().records()) {
                    const int level = static_cast<int>(r.level());
                    sink(levels[level >= 0 && level < 5 ? level : 2], r.target(), r.message());
                }
            }
            return;
        }
        if (event->message_case() == pb::FfiEvent::kPanic) {
            if (const LogSink sink = currentLogSink()) sink("error", "livekit-ffi", "panic: " + event->panic().message());
        }
        if (const auto id = completedAsyncId(*event)) {
            std::promise<std::shared_ptr<pb::FfiEvent>> promise;
            bool found = false;
            {
                std::lock_guard lock(waitersMutex);
                const auto it = waiters.find(*id);
                if (it != waiters.end()) {
                    promise = std::move(it->second);
                    waiters.erase(it);
                    found = true;
                }
            }
            if (found) promise.set_value(event);
        }
        std::vector<std::shared_ptr<Listener>> snapshot;
        {
            std::lock_guard lock(listenersMutex);
            for (const auto& entry : listeners) snapshot.push_back(entry.second);
        }
        for (const auto& listener : snapshot) {
            if (*listener) (*listener)(*event);
        }
    }
};

Ffi::Ffi() : impl_(std::make_unique<Impl>()) {}
Ffi::~Ffi() = default;

void Ffi::setSearchDirectories(std::vector<std::filesystem::path> directories) {
    std::lock_guard lock(g_mutex);
    g_directories = std::move(directories);
    if (!g_instance) g_error.clear();
}

Ffi& Ffi::get() {
    std::lock_guard lock(g_mutex);
    if (g_instance) return *g_instance;

    std::string error;
    auto lib = detail::FfiLibrary::load(g_directories, error);
    if (!lib) {
        g_error = error;
        throw FfiError("Group calls need LiveKit's media library, which isn't available: " + error);
    }
    auto* instance = new Ffi();  // lives until the process ends: the library's threads may outlive any destructor
    instance->impl_->lib = std::move(lib);
    g_impl.store(instance->impl_.get());
    instance->impl_->dispatcher = std::thread([impl = instance->impl_.get()] { impl->run(); });
    // The names the library reports to the server as its SDK: nothing about this app or its version.
    instance->impl_->lib->initialize(&Impl::onEvent, true, "cpp", "");
    g_instance = instance;
    return *g_instance;
}

bool Ffi::libraryInstalled() {
    std::vector<std::filesystem::path> directories;
    {
        std::lock_guard lock(g_mutex);
        directories = g_directories;
    }
    std::error_code ec;
    for (const auto& dir : directories) {
        if (std::filesystem::exists(dir / detail::FfiLibrary::fileName(), ec)) return true;
    }
    return false;
}

bool Ffi::available(std::string* reason) {
    try {
        get();
        return true;
    } catch (const FfiError& e) {
        if (reason) *reason = e.what();
        return false;
    }
}

void Ffi::shutdown() {
    Ffi* instance = nullptr;
    {
        std::lock_guard lock(g_mutex);
        instance = g_instance;
    }
    if (!instance) return;
    try {
        pb::FfiRequest request;
        request.mutable_dispose()->set_async(false);
        instance->request(request);
    } catch (const std::exception&) {
    }
    Impl* impl = instance->impl_.get();
    {
        std::lock_guard lock(impl->queueMutex);
        impl->stopping = true;
    }
    impl->queueCv.notify_all();
    if (impl->dispatcher.joinable()) impl->dispatcher.join();
}

pb::FfiResponse Ffi::request(const pb::FfiRequest& request) {
    std::string bytes;
    if (!request.SerializeToString(&bytes)) throw FfiError("Couldn't encode a request for the media library");
    const std::uint8_t* resultPtr = nullptr;
    std::size_t resultLen = 0;
    const std::uint64_t handle =
        impl_->lib->request(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &resultPtr, &resultLen);
    if (handle == 0) throw FfiError("The media library rejected a request");
    pb::FfiResponse response;
    const bool ok = resultPtr && response.ParseFromArray(resultPtr, static_cast<int>(resultLen));
    impl_->lib->dropHandle(handle);
    if (!ok) throw FfiError("The media library sent an answer this app can't read");
    return response;
}

void Ffi::dropHandle(std::uint64_t handle) {
    if (handle != 0) impl_->lib->dropHandle(handle);
}

std::uint64_t Ffi::nextAsyncId() {
    return impl_->asyncCounter.fetch_add(1);
}

int Ffi::addListener(Listener listener) {
    std::lock_guard lock(impl_->listenersMutex);
    const int id = impl_->nextListenerId++;
    impl_->listeners.emplace_back(id, std::make_shared<Listener>(std::move(listener)));
    return id;
}

void Ffi::removeListener(int id) {
    std::lock_guard lock(impl_->listenersMutex);
    std::erase_if(impl_->listeners, [id](const auto& entry) { return entry.first == id; });
}

std::future<std::shared_ptr<pb::FfiEvent>> Ffi::expect(std::uint64_t asyncId) {
    std::promise<std::shared_ptr<pb::FfiEvent>> promise;
    auto future = promise.get_future();
    std::lock_guard lock(impl_->waitersMutex);
    impl_->waiters.emplace(asyncId, std::move(promise));
    return future;
}

void Ffi::cancel(std::uint64_t asyncId) {
    std::lock_guard lock(impl_->waitersMutex);
    impl_->waiters.erase(asyncId);
}

void Ffi::setLogSink(LogSink sink) {
    std::lock_guard lock(impl_->logMutex);
    impl_->logSink = std::move(sink);
}

}  // namespace redvault::lk
