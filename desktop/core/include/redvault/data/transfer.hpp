// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "redvault/api/api_client.hpp"
#include "redvault/api/dtos.hpp"
#include "redvault/net/http.hpp"

namespace redvault::data {

/**
 * Sends or fetches ciphertext through a transfer target the API handed out
 * (files in chats, Files storage). Only "proxy" targets on our own API under
 * `allowedPrefix` are ever used: a "direct" URL at the storage provider would
 * show it this computer's IP address. Adds the session cookie, retries
 * network errors, 5xx and 429 with backoff until the target expires, and maps
 * failures to UserFacingError (401 to ApiError, so the session can end).
 * `perform` sends the prepared request (streaming as it likes).
 */
net::HttpResponse transferTarget(api::ApiClient& api, const api::TransferTargetDto& target, const std::string& method,
                                 std::string_view allowedPrefix,
                                 const std::function<net::HttpResponse(const net::HttpRequest&)>& perform,
                                 const std::function<void(std::chrono::milliseconds)>& sleep);

}  // namespace redvault::data
