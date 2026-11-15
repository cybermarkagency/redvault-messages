// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/sealed_route.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <iterator>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "redvault_e2ee.h"

namespace redvault::net {

namespace {

using Json = nlohmann::json;

/// Headers that never go inside a sealed request: transport details, or anything identifying.
bool skippedHeader(std::string_view name) {
    static constexpr std::array<std::string_view, 15> kSkipped = {
        "host", "connection", "keep-alive", "proxy-connection", "transfer-encoding", "te", "upgrade", "trailer",
        "content-length", "expect", "user-agent", "forwarded", "x-forwarded-for", "via", "accept-encoding"};
    return std::find(kSkipped.begin(), kSkipped.end(), name) != kSkipped.end();
}

struct Buffer {
    std::uint8_t* data = nullptr;
    std::size_t size = 0;
    ~Buffer() {
        if (data != nullptr) rv_e2ee_free(data, size);
    }
    std::string text() const { return std::string(reinterpret_cast<const char*>(data), size); }
};

bool isGet(const HttpRequest& request) {
    return request.method == "GET" || request.method == "HEAD";
}

}  // namespace

SealedFallbackTransport::SealedFallbackTransport(std::shared_ptr<HttpTransport> onion, std::shared_ptr<HttpTransport> website,
                                                 Config config, std::function<bool()> allowed,
                                                 std::function<void(bool)> onRouteChanged)
    : onion_(std::move(onion)),
      website_(std::move(website)),
      config_(std::move(config)),
      allowed_(std::move(allowed)),
      onRouteChanged_(std::move(onRouteChanged)) {
    prober_ = std::thread([this] { probeLoop(); });
}

SealedFallbackTransport::~SealedFallbackTransport() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (prober_.joinable()) prober_.join();
}

void SealedFallbackTransport::setBackup(bool backup) {
    if (backup_.exchange(backup) != backup) {
        if (onRouteChanged_) onRouteChanged_(backup);
        wake_.notify_all();
    }
}

void SealedFallbackTransport::useOnion() {
    {
        std::lock_guard lock(mutex_);
        failures_ = 0;
    }
    setBackup(false);
}

void SealedFallbackTransport::noteOnionSuccess() {
    std::lock_guard lock(mutex_);
    failures_ = 0;
}

void SealedFallbackTransport::noteOnionFailure() {
    bool switchNow = false;
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (failures_ == 0) firstFailure_ = now;
        ++failures_;
        switchNow = failures_ >= config_.switchAfterFailures || now - firstFailure_ >= config_.switchAfter;
    }
    if (switchNow && !config_.keyConfig.empty() && (!allowed_ || allowed_())) setBackup(true);
}

bool SealedFallbackTransport::probeOnionNow() {
    try {
        const auto response = onion_->send(HttpRequest{"GET", config_.probeUrl, {}, {}});
        if (response.status >= 200 && response.status < 500) {
            noteOnionSuccess();
            setBackup(false);
            return true;
        }
    } catch (const NetworkError&) {
        // Still unreachable: stay on the backup route.
    }
    return false;
}

void SealedFallbackTransport::probeLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (!backup_.load()) {
            wake_.wait(lock, [this] { return stopping_ || backup_.load(); });
            continue;
        }
        wake_.wait_for(lock, config_.probeEvery, [this] { return stopping_; });
        if (stopping_) break;
        lock.unlock();
        probeOnionNow();
        lock.lock();
    }
}

HttpResponse SealedFallbackTransport::send(const HttpRequest& request) {
    return sendStreaming(request, StreamOptions{});
}

HttpResponse SealedFallbackTransport::sendStreaming(const HttpRequest& request, const StreamOptions& streams) {
    // The user chose the onion only: never the backup route, and leave it if we're on it.
    if (allowed_ && !allowed_() && backup_.load()) setBackup(false);
    if (backup_.load()) return sendSealed(request, streams);
    try {
        auto response = onion_->sendStreaming(request, streams);
        noteOnionSuccess();
        return response;
    } catch (const TorUnavailableError&) {
        throw;  // Tor itself is down: nothing goes anywhere, on either route.
    } catch (const NetworkError&) {
        noteOnionFailure();
        // A GET is safe to send again; a body that was streamed can't be replayed.
        if (backup_.load() && isGet(request) && streams.upload == nullptr) return sendSealed(request, streams);
        throw;
    }
}

HttpResponse SealedFallbackTransport::sendSealed(const HttpRequest& request, const StreamOptions& streams) {
    const std::string prefix = config_.onionOrigin + config_.pathPrefix;
    if (request.url.rfind(prefix + "/api/", 0) != 0) {
        throw NetworkError("Only the API can use the backup route");
    }
    const std::string path = request.url.substr(prefix.size());

    std::string body = request.body;
    if (streams.upload != nullptr) {
        if (streams.uploadSize > kMaxSealedBody) {
            throw NetworkError("Large files wait for the onion address: the backup route carries up to 8 MB at a time");
        }
        body.assign(std::istreambuf_iterator<char>(*streams.upload), std::istreambuf_iterator<char>());
        if (body.size() != streams.uploadSize) throw NetworkError("Upload stopped");
    }

    Json headers = Json::array();
    for (const auto& [name, value] : request.headers) {
        if (!skippedHeader(name)) headers.push_back(Json::array({name, value}));
    }
    const std::string head = Json{{"method", request.method}, {"path", path}, {"headers", headers}}.dump();

    Buffer sealed;
    void* pending = nullptr;
    const int sealedStatus = rv_sealed_seal(config_.keyConfig.data(), config_.keyConfig.size(),
                                            reinterpret_cast<const std::uint8_t*>(head.data()), head.size(),
                                            reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), &sealed.data,
                                            &sealed.size, &pending);
    if (sealedStatus != 0) throw NetworkError("Couldn't seal the request for the backup route");

    HttpResponse outer;
    try {
        outer = website_->send(HttpRequest{"POST", config_.gatewayUrl, {{"content-type", "message/ohttp-req"}}, sealed.text()});
    } catch (...) {
        rv_sealed_discard(pending);
        throw;
    }
    if (outer.status != 200) {
        rv_sealed_discard(pending);
        throw NetworkError("The backup route refused the request (" + std::to_string(outer.status) + ")");
    }

    Buffer headOut;
    Buffer bodyOut;
    const int openedStatus =
        rv_sealed_open(pending, reinterpret_cast<const std::uint8_t*>(outer.body.data()), outer.body.size(), &headOut.data,
                       &headOut.size, &bodyOut.data, &bodyOut.size);
    if (openedStatus != 0) throw NetworkError("The backup route's answer couldn't be opened");

    HttpResponse response;
    const auto parsed = Json::parse(headOut.text(), nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("status")) throw NetworkError("The backup route's answer is malformed");
    response.status = parsed["status"].get<int>();
    for (const auto& pair : parsed.value("headers", Json::array())) {
        if (pair.is_array() && pair.size() == 2) {
            std::string name = pair[0].get<std::string>();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            response.headers.emplace_back(std::move(name), pair[1].get<std::string>());
        }
    }
    if (streams.download != nullptr && response.status >= 200 && response.status < 300) {
        streams.download->write(reinterpret_cast<const char*>(bodyOut.data), static_cast<std::streamsize>(bodyOut.size));
        if (!*streams.download) throw NetworkError("Couldn't save the download");
    } else {
        response.body = bodyOut.text();
    }
    return response;
}

}  // namespace redvault::net
