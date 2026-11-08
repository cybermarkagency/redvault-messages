// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "redvault/net/http.hpp"

namespace redvault::test {

/// Records every request and answers from a queue (or a handler), so API tests never touch the network.
class FakeTransport final : public net::HttpTransport {
public:
    using Handler = std::function<net::HttpResponse(const net::HttpRequest&)>;

    std::vector<net::HttpRequest> requests;
    std::deque<net::HttpResponse> responses;
    Handler handler;
    bool failWithNetworkError = false;

    void enqueue(int status, std::string body, net::Headers headers = {}) {
        responses.push_back(net::HttpResponse{status, std::move(headers), std::move(body)});
    }

    net::HttpResponse send(const net::HttpRequest& request) override {
        requests.push_back(request);
        if (failWithNetworkError) throw net::NetworkError("connection refused");
        if (handler) return handler(request);
        if (responses.empty()) throw std::logic_error("FakeTransport: no response queued for " + request.url);
        auto response = std::move(responses.front());
        responses.pop_front();
        return response;
    }

    const net::HttpRequest& last() const { return requests.back(); }

    static std::string headerOf(const net::HttpRequest& request, const std::string& name) {
        for (const auto& [key, value] : request.headers) {
            if (key == name) return value;
        }
        return {};
    }
};

}  // namespace redvault::test
