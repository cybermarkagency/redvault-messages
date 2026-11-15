// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/sealed_route.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <iterator>
#include <regex>
#include <utility>
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

namespace {

/// What crypto-core seals with: Oblivious HTTP to the gateway's pinned key.
class CryptoCoreSealer final : public SealedFallbackTransport::Sealer {
public:
    explicit CryptoCoreSealer(std::vector<std::uint8_t> keyConfig) : keyConfig_(std::move(keyConfig)) {}

    class Pending final : public SealedFallbackTransport::Sealer::Pending {
    public:
        explicit Pending(void* handle) : handle_(handle) {}
        ~Pending() override {
            if (handle_ != nullptr) rv_sealed_discard(handle_);
        }
        Pending(const Pending&) = delete;
        Pending& operator=(const Pending&) = delete;

        void open(const std::string& reply, std::string& headJson, std::string& body) override {
            void* handle = std::exchange(handle_, nullptr);
            if (handle == nullptr) throw NetworkError("The backup route's answer was already opened");
            Buffer headOut;
            Buffer bodyOut;
            const int status = rv_sealed_open(handle, reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size(),
                                              &headOut.data, &headOut.size, &bodyOut.data, &bodyOut.size);
            if (status != 0) throw NetworkError("The backup route's answer couldn't be opened");
            headJson = headOut.text();
            body = bodyOut.text();
        }

    private:
        void* handle_;
    };

    std::string seal(const std::string& headJson, const std::string& body, std::unique_ptr<Sealer::Pending>& pending) override {
        Buffer sealed;
        void* handle = nullptr;
        const int status = rv_sealed_seal(keyConfig_.data(), keyConfig_.size(),
                                          reinterpret_cast<const std::uint8_t*>(headJson.data()), headJson.size(),
                                          reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), &sealed.data,
                                          &sealed.size, &handle);
        if (status != 0) throw NetworkError("Couldn't seal the request for the backup route");
        pending = std::make_unique<Pending>(handle);
        return sealed.text();
    }

private:
    std::vector<std::uint8_t> keyConfig_;
};

/// `PUT /api/v1/files/<id>/content`: the chat file upload the server takes in parts (Content-Range).
bool takesUploadInParts(const std::string& method, const std::string& path) {
    static const std::regex kPattern("^/api/v1/files/[0-9A-Fa-f-]{36}/content$");
    return method == "PUT" && std::regex_match(path, kPattern);
}

/// `bytes start-end/total` from a 206's Content-Range.
bool parseContentRange(const std::string& value, std::uint64_t& start, std::uint64_t& end, std::uint64_t& total) {
    static const std::regex kPattern("^bytes ([0-9]{1,15})-([0-9]{1,15})/([0-9]{1,15})$");
    std::smatch match;
    if (!std::regex_match(value, match, kPattern)) return false;
    start = std::stoull(match[1].str());
    end = std::stoull(match[2].str());
    total = std::stoull(match[3].str());
    return start <= end && end < total;
}

std::string rangeText(std::uint64_t start, std::uint64_t end) {
    return std::to_string(start) + "-" + std::to_string(end);
}

}  // namespace

SealedFallbackTransport::SealedFallbackTransport(std::shared_ptr<HttpTransport> onion, std::shared_ptr<HttpTransport> website,
                                                 Config config, std::function<bool()> allowed,
                                                 std::function<void(bool)> onRouteChanged)
    : onion_(std::move(onion)),
      website_(std::move(website)),
      config_(std::move(config)),
      allowed_(std::move(allowed)),
      onRouteChanged_(std::move(onRouteChanged)),
      sealer_(config_.sealer ? config_.sealer : std::make_shared<CryptoCoreSealer>(config_.keyConfig)) {
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
    // Where a download started, so a resend on the backup route doesn't follow half a file from the onion.
    const auto downloadStart = streams.download != nullptr ? streams.download->tellp() : std::ostream::pos_type(-1);
    try {
        auto response = onion_->sendStreaming(request, streams);
        noteOnionSuccess();
        return response;
    } catch (const TorUnavailableError&) {
        throw;  // Tor itself is down: nothing goes anywhere, on either route.
    } catch (const NetworkError&) {
        noteOnionFailure();
        // A GET is safe to send again; a body that was streamed can't be replayed.
        if (backup_.load() && isGet(request) && streams.upload == nullptr) {
            if (streams.download != nullptr) {
                if (downloadStart == std::ostream::pos_type(-1)) throw;
                streams.download->clear();
                streams.download->seekp(downloadStart);
                if (!*streams.download) throw;
            }
            return sendSealed(request, streams);
        }
        throw;
    }
}

SealedFallbackTransport::Answer SealedFallbackTransport::sealOnce(const std::string& method, const std::string& path,
                                                                  const Headers& headers, const std::string& body) {
    Json headerList = Json::array();
    for (const auto& [name, value] : headers) headerList.push_back(Json::array({name, value}));
    const std::string head = Json{{"method", method}, {"path", path}, {"headers", headerList}}.dump();

    std::unique_ptr<Sealer::Pending> pending;
    const std::string sealed = sealer_->seal(head, body, pending);

    // A throw from here on drops `pending`, which discards the reply's key.
    const HttpResponse outer = website_->send(HttpRequest{"POST", config_.gatewayUrl, {{"content-type", "message/ohttp-req"}}, sealed});
    if (outer.status != 200) throw NetworkError("The backup route refused the request (" + std::to_string(outer.status) + ")");

    std::string headOut;
    Answer answer;
    pending->open(outer.body, headOut, answer.body);
    const auto parsed = Json::parse(headOut, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("status")) throw NetworkError("The backup route's answer is malformed");
    answer.status = parsed["status"].get<int>();
    for (const auto& pair : parsed.value("headers", Json::array())) {
        if (pair.is_array() && pair.size() == 2) {
            std::string name = pair[0].get<std::string>();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            answer.headers.emplace_back(std::move(name), pair[1].get<std::string>());
        }
    }
    return answer;
}

SealedFallbackTransport::Answer SealedFallbackTransport::sealPart(const std::string& method, const std::string& path,
                                                                  const Headers& headers, const std::string& body) {
    // One part of a file: sending it again is harmless (the server replaces a part), so a part that
    // got no answer is tried again before the whole transfer fails.
    for (int attempt = 1;; ++attempt) {
        try {
            return sealOnce(method, path, headers, body);
        } catch (const TorUnavailableError&) {
            throw;
        } catch (const NetworkError&) {
            if (attempt >= kPartAttempts) throw;
        }
    }
}

HttpResponse SealedFallbackTransport::sendSealed(const HttpRequest& request, const StreamOptions& streams) {
    const std::string prefix = config_.onionOrigin + config_.pathPrefix;
    if (request.url.rfind(prefix + "/api/", 0) != 0) {
        throw NetworkError("Only the API can use the backup route");
    }
    const std::string path = request.url.substr(prefix.size());

    Headers headers;
    bool hasRange = false;
    for (const auto& [name, value] : request.headers) {
        if (skippedHeader(name)) continue;
        if (name == "range") hasRange = true;
        headers.emplace_back(name, value);
    }

    if (streams.upload != nullptr && streams.uploadSize > kMaxSealedBody) {
        if (!takesUploadInParts(request.method, path)) {
            throw NetworkError("Large files wait for the onion address: the backup route carries up to 8 MB at a time");
        }
        return uploadInParts(request.method, path, headers, streams);
    }
    if (streams.download != nullptr && !hasRange && isGet(request)) {
        return downloadInRanges(request.method, path, headers, streams);
    }

    std::string body = request.body;
    if (streams.upload != nullptr) {
        body.assign(std::istreambuf_iterator<char>(*streams.upload), std::istreambuf_iterator<char>());
        if (body.size() != streams.uploadSize) throw NetworkError("Upload stopped");
    }
    return toResponse(sealOnce(request.method, path, headers, body), streams);
}

HttpResponse SealedFallbackTransport::toResponse(Answer answer, const StreamOptions& streams) {
    HttpResponse response;
    response.status = answer.status;
    response.headers = std::move(answer.headers);
    if (streams.download != nullptr && response.status >= 200 && response.status < 300) {
        streams.download->write(answer.body.data(), static_cast<std::streamsize>(answer.body.size()));
        if (!*streams.download) throw NetworkError("Couldn't save the download");
    } else {
        response.body = std::move(answer.body);
    }
    return response;
}

HttpResponse SealedFallbackTransport::uploadInParts(const std::string& method, const std::string& path, const Headers& headers,
                                                    const StreamOptions& streams) {
    const std::uint64_t total = streams.uploadSize;
    std::string part;
    for (std::uint64_t start = 0; start < total; start += kPartBytes) {
        const std::uint64_t length = std::min<std::uint64_t>(kPartBytes, total - start);
        part.resize(static_cast<std::size_t>(length));
        streams.upload->read(part.data(), static_cast<std::streamsize>(length));
        if (static_cast<std::uint64_t>(streams.upload->gcount()) != length) throw NetworkError("Upload stopped");

        Headers partHeaders = headers;
        partHeaders.emplace_back("content-range", "bytes " + rangeText(start, start + length - 1) + "/" + std::to_string(total));
        Answer answer = sealPart(method, path, partHeaders, part);
        const bool last = start + length == total;
        // 202: the part is in, more to come. Anything else ends the upload with that answer.
        if (!last && answer.status == 202) continue;
        return toResponse(std::move(answer), StreamOptions{});
    }
    throw NetworkError("Upload stopped");  // total was 0: never reached for a body above the limit
}

HttpResponse SealedFallbackTransport::downloadInRanges(const std::string& method, const std::string& path, const Headers& headers,
                                                       const StreamOptions& streams) {
    std::uint64_t next = 0;
    std::uint64_t total = 0;
    Headers lastHeaders;
    do {
        Headers rangeHeaders = headers;
        rangeHeaders.emplace_back("range", "bytes=" + rangeText(next, next + kPartBytes - 1));
        Answer answer = next == 0 ? sealOnce(method, path, rangeHeaders, {}) : sealPart(method, path, rangeHeaders, {});
        if (answer.status != 206) {
            // A route without ranges answers 200 with the whole (small) body; 416 is an empty file.
            if (next == 0 && answer.status == 416) return toResponse(sealOnce(method, path, headers, {}), streams);
            if (next == 0) return toResponse(std::move(answer), streams);
            throw NetworkError("Download stopped (" + std::to_string(answer.status) + ")");
        }
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        std::uint64_t whole = 0;
        const auto contentRange = std::find_if(answer.headers.begin(), answer.headers.end(),
                                               [](const auto& header) { return header.first == "content-range"; });
        if (contentRange == answer.headers.end() || !parseContentRange(contentRange->second, start, end, whole) ||
            start != next || (next != 0 && whole != total) || answer.body.size() != end - start + 1) {
            throw NetworkError("The backup route's download is malformed");
        }
        total = whole;
        streams.download->write(answer.body.data(), static_cast<std::streamsize>(answer.body.size()));
        if (!*streams.download) throw NetworkError("Couldn't save the download");
        next = end + 1;
        lastHeaders = std::move(answer.headers);
    } while (next < total);

    // To the caller it's the whole file, as the onion would have sent it.
    HttpResponse response;
    response.status = 200;
    for (auto& header : lastHeaders) {
        if (header.first == "content-range") continue;
        if (header.first == "content-length") header.second = std::to_string(total);
        response.headers.push_back(std::move(header));
    }
    return response;
}

}  // namespace redvault::net
