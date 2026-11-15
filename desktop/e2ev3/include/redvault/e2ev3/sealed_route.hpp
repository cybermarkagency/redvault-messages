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
 *   3 in a row), and the user hasn't chosen "Only use the onion address":
 *   sealed to the gateway's pinned key (crypto-core, Oblivious HTTP) and POSTed
 *   through Tor to the website's gateway address by `website`, a transport that
 *   only speaks https to that one host through the same Tor gate;
 * - a background check tries the onion every minute and moves back when it answers.
 *
 * Never a request without Tor (both transports refuse that), never an unsealed
 * request to the website. A request that failed on the onion is only re-sent
 * on the backup route automatically when it's a GET; anything else fails as it
 * would have, and the app's own retries (the outbox) send it again.
 */
class SealedFallbackTransport final : public HttpTransport {
public:
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
        /// The onion request the background check makes (a cheap public one).
        std::string probeUrl;
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

private:
    HttpResponse sendSealed(const HttpRequest& request, const StreamOptions& streams);
    void noteOnionFailure();
    void noteOnionSuccess();
    void setBackup(bool backup);
    void probeLoop();

    std::shared_ptr<HttpTransport> onion_;
    std::shared_ptr<HttpTransport> website_;
    Config config_;
    std::function<bool()> allowed_;
    std::function<void(bool)> onRouteChanged_;

    std::atomic<bool> backup_{false};
    std::mutex mutex_;
    int failures_ = 0;
    std::chrono::steady_clock::time_point firstFailure_{};
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread prober_;
};

}  // namespace redvault::net
