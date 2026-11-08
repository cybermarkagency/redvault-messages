// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <nlohmann/json.hpp>

// The shared encryption engine (crypto-core: vodozemac Olm and Megolm, device
// lists, pairing) through its C ABI, include/redvault_e2ee.h. One JSON request
// in, one JSON response out; the engine keeps no state between calls. Requests
// carry the device's encrypted state and its storage key, responses the new
// state. Nothing here talks to the network.

namespace redvault::e2ev3 {

using Json = nlohmann::json;

/// Runs one request. The answer always has "ok"; on failure "error" (a stable code) and "message".
Json runEngine(const Json& request);

}  // namespace redvault::e2ev3
