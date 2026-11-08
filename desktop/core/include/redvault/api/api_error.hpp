// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace redvault::api {

/**
 * A failed API call, with the server's machine-readable error code where
 * there is one (the `{"error": "<code>", "message": "..."}` body). status 0
 * means no HTTP response: code "network_error", or "malformed_response" when
 * a 2xx body wasn't the expected shape.
 */
class ApiError : public std::runtime_error {
public:
    ApiError(int status, std::string code, const std::string& message)
        : std::runtime_error(message), status_(status), code_(std::move(code)) {}

    int status() const noexcept { return status_; }
    const std::string& code() const noexcept { return code_; }

private:
    int status_;
    std::string code_;
};

/// An error whose message is written for the person using the app, safe to show as-is.
class UserFacingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A transfer that never reached the server, or broke off (no connection, Tor down): worth queueing and retrying.
class TransferInterrupted : public UserFacingError {
public:
    using UserFacingError::UserFacingError;
};

}  // namespace redvault::api
