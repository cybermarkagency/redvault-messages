// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <functional>
#include <set>
#include <string>

#include "redvault/api/api_error.hpp"
#include "redvault/e2ev3/engine.hpp"

// Sending one message request at a time, at most 100 devices each, in order
// (docs/signal-integration/11-part-c2-communities.md, "Sending"). Internal to e2ev3.

namespace redvault::e2ev3::detail {

/// Accounts a refusal names to drop ("recipient_not_entitled" with details.userIds), or none.
std::set<std::string> refusedAccounts(const api::ApiError& error);

/**
 * Sends `items` (each an object with a "userId": recipients or per-device messages) in batches of at most 100, in
 * order, through `send`. A batch the server or the connection refused for a moment (no response, 5xx, 429; never
 * "tor_unavailable") is tried again, up to kBatchAttempts times. With `dropOnce`: the first refusal it turns into a
 * non-empty set of accounts (recipient_not_entitled) drops those accounts from this batch and every later one, tells
 * `dropped`, and tries once more; a second such refusal is thrown. Tor down inside a chat send (SendQueued): every
 * later batch is queued behind it, in order, and SendQueued is thrown once all are. Returns how many items went now.
 * `byAccount` (a send marked allDevices): items are grouped by account, first appearance first, and a batch never
 * cuts one account's devices across two requests (an account has at most 10 devices).
 */
int sendInBatches(const Json& items, const std::function<void(const Json& batch)>& send, bool dropOnce = false,
                  std::set<std::string>* dropped = nullptr, bool byAccount = false);

/// A key claim for any number of devices, at most 100 per request (the server's limit); the answers in order.
Json claimInBatches(const Json& devices, const std::function<Json(const Json& batch)>& claim);

}  // namespace redvault::e2ev3::detail
