// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Development check (docs/tor-by-default.md, "Live check"): starts the embedded Tor
// gateway exactly as the app does, and fetches GET <onion API base>public/routing
// through the app's own transport (CurlTransport in Tor-only mode: socks5h to the
// gate, only the onion host, a socket guard). Passes when the server answers
// "viaOnion": true, which proves the onion API base path. One anonymous GET; it
// signs in to nothing and sends nothing else.
//
//   redvault_tor_routing_check [--api <onion API base>] [--state <dir>] [--wait <seconds>] [--path <relative path>]

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "redvault/net/curl_transport.hpp"
#include "redvault/util/text.hpp"
#include "redvault_tor.h"

#ifndef REDVAULT_ONION_API_BASE
#define REDVAULT_ONION_API_BASE ""
#endif

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace redvault;

namespace {

Json takeJson(char* text) {
    if (text == nullptr) return Json::object();
    Json out = Json::parse(text, nullptr, false);
    rv_tor_free_string(text);
    return out.is_object() ? out : Json::object();
}

}  // namespace

int main(int argc, char** argv) {
    std::string apiBase = REDVAULT_ONION_API_BASE;
    fs::path state = fs::temp_directory_path() / ("redvault-tor-check-" + util::randomUuid());
    int waitSeconds = 240;
    std::string relative = "public/routing";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--api") apiBase = argv[i + 1];
        else if (flag == "--state") state = argv[i + 1];
        else if (flag == "--wait") waitSeconds = std::atoi(argv[i + 1]);
        else if (flag == "--path") relative = argv[i + 1];
        else {
            std::cerr << "unknown flag " << flag << "\n";
            return 2;
        }
    }
    if (!apiBase.ends_with('/')) apiBase += '/';
    const auto url = net::parseUrl(apiBase);
    if (!url || url->scheme != "http" || !net::isOnionHost(url->host)) {
        std::cerr << "The API base must be http://<v3 onion>/...: " << apiBase << "\n";
        return 2;
    }
    // Relative to the API base, like ApiClient; or an absolute path on the same onion host.
    const std::string target = relative.starts_with('/') ? "http://" + url->host + relative : apiBase + relative;

    fs::create_directories(state / "state");
    fs::create_directories(state / "cache");
    fs::permissions(state, fs::perms::owner_all, fs::perm_options::replace);
    fs::permissions(state / "state", fs::perms::owner_all, fs::perm_options::replace);
    fs::permissions(state / "cache", fs::perms::owner_all, fs::perm_options::replace);
    const Json config = {{"stateDir", (state / "state").string()},
                         {"cacheDir", (state / "cache").string()},
                         {"allowedTargets", Json::array({url->host + ":80"})}};
    char* error = nullptr;
    RvTorGateway* gateway = rv_tor_start(config.dump().c_str(), &error);
    if (gateway == nullptr) {
        std::cerr << "Tor didn't start: " << (error != nullptr ? error : "") << "\n";
        if (error != nullptr) rv_tor_free_string(error);
        return 1;
    }
    const Json endpoint = takeJson(rv_tor_endpoint(gateway));
    const net::SocksEndpoint gate{endpoint.value("host", std::string()), endpoint.value("port", 0), endpoint.value("username", std::string()),
                                  endpoint.value("password", std::string())};

    const auto started = std::chrono::steady_clock::now();
    std::string last;
    std::string torState;
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(waitSeconds)) {
        const Json status = takeJson(rv_tor_status(gateway));
        torState = status.value("state", std::string("unavailable"));
        const std::string line = torState + " " + std::to_string(status.value("progress", 0)) + "% " + status.value("detail", std::string());
        if (line != last) {
            std::cerr << "tor: " << line << "\n";
            last = line;
        }
        if (torState == "connected") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    net::CurlTransport::Options options;
    options.allowedHost = url->host;
    options.connectTimeout = std::chrono::seconds(90);
    options.stallTimeout = std::chrono::seconds(60);
    options.torGate = [gate]() -> std::optional<net::SocksEndpoint> { return gate; };
    net::CurlTransport transport(options);

    int code = 1;
    try {
        const auto response = transport.send(net::HttpRequest{"GET", target, {{"accept", "application/json"}}, {}});
        const Json body = Json::parse(response.body, nullptr, false);
        const bool viaOnion = body.is_object() && body.value("viaOnion", false);
        std::cout << Json{{"url", target},
                          {"status", response.status},
                          {"viaOnion", viaOnion},
                          {"onion", body.is_object() ? body.value("onion", Json(nullptr)) : Json(nullptr)},
                          {"mode", body.is_object() ? body.value("mode", Json(nullptr)) : Json(nullptr)}}
                         .dump()
                  << "\n";
        if (response.status == 200 && viaOnion) code = 0;
    } catch (const std::exception& e) {
        std::cout << Json{{"url", target}, {"error", e.what()}}.dump() << "\n";
    }
    const auto sockets = transport.socketLog();
    std::cerr << "sockets: " << sockets.allowed << " to the Tor gate, " << sockets.refused.size() << " refused\n";
    rv_tor_stop(gateway);
    std::error_code ignored;
    fs::remove_all(state, ignored);
    return code;
}
