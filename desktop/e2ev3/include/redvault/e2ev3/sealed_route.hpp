// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "redvault/net/http.hpp"

namespace redvault::net {

/**
 * The sealed backup route (docs/tor-sealed-fallback.md).
 *
 * Wraps the onion transport. Requests keep their onion URL (so the session
 * cookie and everything else stay the same); this decides how they travel:
 * - normally straight through the onion transport;
 * - when the onion keeps failing while Tor itself works (30 s of failures, or
 *   3 in a row; or at once when a connection to it failed before anything was
 *   sent while Tor is connected), and the user hasn't chosen "Only use the onion address":
 *   sealed to the gateway's pinned key (crypto-core, Oblivious HTTP) and POSTed
 *   through Tor to the website's gateway address by `website`, a transport that
 *   only speaks https to that one host through the same Tor gate;
 * - a background check tries the onion every minute and moves back when it answers.
 *
 * Never a request without Tor (both transports refuse that), never an unsealed
 * request to the website. A request that failed on the onion is re-sent on the
 * backup route automatically when it's a GET, or when nothing of it reached
 * the onion (NotSentError: a sign-in, say); anything else fails as it would
 * have, and the app's own retries (the outbox) send it again.
 *
 * A chat file bigger than one sealed message goes up in Content-Range parts and
 * comes down in Range steps of kPartBytes, each part its own sealed request
 * (docs/tor-sealed-fallback.md, section 5).
 */
class SealedFallbackTransport final : public HttpTransport {
public:
    /// Seals a request and opens its answer. crypto-core's Oblivious HTTP unless a test passes its own.
    class Sealer {
    public:
        class Pending {
        public:
            virtual ~Pending() = default;
            /// Opens the gateway's answer into the inner response head (JSON) and body. Throws NetworkError.
            virtual void open(const std::string& reply, std::string& headJson, std::string& body) = 0;
        };
        virtual ~Sealer() = default;
        /// The message/ohttp-req body for this request head (JSON) and body. Throws NetworkError.
        virtual std::string seal(const std::string& headJson, const std::string& body, std::unique_ptr<Pending>& pending) = 0;
    };

    struct Config {
        /// The API base's origin and path prefix, e.g. "http://<onion>.onion" and "/redvaultmessages":
        /// "<origin><prefix>/api/..." becomes the sealed request's path "/api/...".
        std::string onionOrigin;
        std::string pathPrefix;
        /// Where sealed requests go, e.g. "https://www.redvaultsystems.com/redvaultmessages/ohttp".
        std::string gatewayUrl;
        /// The gateway's current public key configuration (RFC 9458 KeyConfig), pinned in the build.
        std::vector<std::uint8_t> keyConfig;
        std::chrono::seconds switchAfter{30};
        int switchAfterFailures = 3;
        std::chrono::seconds probeEvery{60};
        /// Start on the backup route (it was in use when the app last closed); the background check
        /// moves back to the onion as soon as it answers.
        bool startOnBackup = false;
        /// Whether Tor itself is connected now. A connection that failed before anything was sent
        /// switches at once only while it is (during Tor's start-up such failures say nothing about
        /// the onion). Unset: never switch on one failure.
        std::function<bool()> torConnected;
        /// The onion request the background check makes (a cheap public one).
        std::string probeUrl;
        /// Tests only: a stand-in for crypto-core's sealing, so a fake gateway can read requests.
        std::shared_ptr<Sealer> sealer;
    };

    /// `allowed`: false while the user has chosen "Only use the onion address".
    /// `onRouteChanged(backup)`: called (from any thread) when the route changes.
    SealedFallbackTransport(std::shared_ptr<HttpTransport> onion, std::shared_ptr<HttpTransport> website, Config config,
                            std::function<bool()> allowed, std::function<void(bool)> onRouteChanged);
    ~SealedFallbackTransport() override;

    HttpResponse send(const HttpRequest& request) override;
    HttpResponse sendStreaming(const HttpRequest& request, const StreamOptions& streams) override;

    /// True while requests go over the backup route.
    bool onBackup() const { return backup_.load(); }
    /// Back to the onion now (the user turned "Only use the onion address" on, say).
    void useOnion();
    /// One onion check, as the background one does (tests).
    bool probeOnionNow();

    /// Largest body sent or received on the backup route (one sealed message; Files parts fit).
    static constexpr std::uint64_t kMaxSealedBody = 8ull * 1024 * 1024 + 512 * 1024;
    /// A chat file bigger than one sealed message crosses the backup route in parts of this size:
    /// Content-Range uploads and Range downloads (server/src/services/fileService.ts, FILE_UPLOAD_PART_BYTES).
    static constexpr std::uint64_t kPartBytes = 8ull * 1024 * 1024;
    /// How often one part is tried before the transfer fails.
    static constexpr int kPartAttempts = 3;

private:
    struct Answer {
        int status = 0;
        Headers headers;
        std::string body;
    };

    HttpResponse sendSealed(const HttpRequest& request, const StreamOptions& streams);
    Answer sealOnce(const std::string& method, const std::string& path, const Headers& headers, const std::string& body);
    Answer sealPart(const std::string& method, const std::string& path, const Headers& headers, const std::string& body);
    static HttpResponse toResponse(Answer answer, const StreamOptions& streams);
    HttpResponse uploadInParts(const std::string& method, const std::string& path, const Headers& headers,
                               const StreamOptions& streams);
    HttpResponse downloadInRanges(const std::string& method, const std::string& path, const Headers& headers,
                                  const StreamOptions& streams);
    /// `notSent`: the connection failed before anything went out (a dead onion, typically).
    void noteOnionFailure(bool notSent = false);
    void noteOnionSuccess();
    void setBackup(bool backup);
    void probeLoop();

    std::shared_ptr<HttpTransport> onion_;
    std::shared_ptr<HttpTransport> website_;
    Config config_;
    std::function<bool()> allowed_;
    std::function<void(bool)> onRouteChanged_;
    std::shared_ptr<Sealer> sealer_;

    std::atomic<bool> backup_{false};
    std::mutex mutex_;
    int failures_ = 0;
    std::chrono::steady_clock::time_point firstFailure_{};
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread prober_;
};

}  // namespace redvault::net
