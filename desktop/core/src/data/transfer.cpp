// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/transfer.hpp"

#include <algorithm>
#include <string>

#include "redvault/api/api_error.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

using api::ApiError;
using api::TransferInterrupted;
using api::UserFacingError;

namespace {
constexpr int kMaxAttempts = 4;
}  // namespace

net::HttpResponse transferTarget(api::ApiClient& api, const api::TransferTargetDto& target, const std::string& method,
                                 std::string_view allowedPrefix,
                                 const std::function<net::HttpResponse(const net::HttpRequest&)>& perform,
                                 const std::function<void(std::chrono::milliseconds)>& sleep) {
    net::HttpRequest request;
    request.method = method;
    // Only our own API, never a "direct" URL at the storage provider: it
    // would see this computer's IP address. The web app's CSP refuses those too.
    if (target.mode != "proxy" || !target.url.starts_with(allowedPrefix)) throw UserFacingError("Transfer failed. Try again.");
    request.url = api.resolve(target.url);
    const auto url = net::parseUrl(request.url);
    if (!url) throw UserFacingError("Transfer failed. Try again.");

    bool hasContentType = false;
    for (const auto& [name, value] : target.headers) {
        const std::string lower = net::toLower(name);
        hasContentType = hasContentType || lower == "content-type";
        request.headers.emplace_back(lower, value);
    }
    if (method == "PUT" && !hasContentType) request.headers.emplace_back("content-type", "application/octet-stream");

    const auto expiresAt = util::parseIsoMillis(target.expiresAt);
    // Everything retried here (no connection, server busy) is TransferInterrupted once the
    // retries run out: the app pauses and resumes instead of giving up.
    UserFacingError lastError("Network interrupted. Try again.");
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) sleep(std::chrono::milliseconds(std::min(8000LL, 500LL << (attempt - 1))));
        if (expiresAt && *expiresAt < util::nowMillis()) throw UserFacingError("The transfer link expired. Try again.");

        net::HttpRequest attemptRequest = request;
        if (const auto cookie = api.cookies().headerFor(*url); !cookie.empty()) {
            attemptRequest.headers.emplace_back("cookie", cookie);
        }
        net::HttpResponse response;
        try {
            response = perform(attemptRequest);
        } catch (const net::TorUnavailableError&) {
            // Tor routing and Tor is down: retrying now can't help, and nothing may go another way.
            throw TransferInterrupted("Tor isn't connected. The transfer resumes when it is.");
        } catch (const net::NetworkError&) {
            lastError = UserFacingError("Network interrupted. Try again.");
            continue;
        }
        api.cookies().saveFromResponse(*url, response.headersNamed("set-cookie"));
        const int code = response.status;
        if (code >= 200 && code <= 299) return response;

        UserFacingError error("Transfer failed (" + std::to_string(code) + "). Try again.");
        if (code == 410) {
            error = UserFacingError("This file is no longer available.");
        } else if (code == 404) {
            error = UserFacingError("File not found.");
        } else if (code == 401) {
            // Session over: surface it like any API call so the app can sign out.
            throw ApiError(401, "unauthorized", "Your session has ended. Sign in again.");
        }
        if (code >= 500 || code == 429) {
            lastError = error;
            continue;
        }
        throw error;
    }
    throw TransferInterrupted(lastError.what());
}

}  // namespace redvault::data
