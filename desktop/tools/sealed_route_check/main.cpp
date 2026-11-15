// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Development only (docs/tor-sealed-fallback.md, "Live"): proves the sealed backup route works over the real Tor
// network with the app's own code. It starts the embedded Tor gateway with the same policy the app uses (the onion
// plus the one gateway website), points the app's onion transport at an onion address that can't answer (the real
// onion on an unused port, by default), and wraps it in the app's SealedFallbackTransport with the pinned key.
// The onion requests fail, the transport switches to the backup route, and GET public/routing must come back
// `"viaSealed": true`: sealed to the gateway's key, through Tor, to the website, and answered by the API's sealed
// listener. Anonymous, no account.
//
//   redvault_sealed_route_check --key <KeyConfig hex> [--onion-api <dead onion API base>] [--gateway <https URL>]
//                               [--wait <seconds>] [--tries <n>]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "redvault/e2ev3/sealed_route.hpp"
#include "redvault/net/curl_transport.hpp"
#include "redvault/util/text.hpp"
#include "redvault_tor.h"

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
    // The live onion, on a port its service doesn't serve: Tor connects, the onion never answers.
    std::string onionApi = "http://4cydjhdc3lmhg7l6ykjont7dms2alunglmnlznlmjjwkyc67m5h325yd.onion:81/redvaultmessages/api/";
    std::string gatewayUrl = "https://www.redvaultsystems.com/redvaultmessages/ohttp";
    std::string keyHex;
    int waitSeconds = 240;
    int tries = 6;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--key") keyHex = argv[i + 1];
        else if (flag == "--onion-api") onionApi = argv[i + 1];
        else if (flag == "--gateway") gatewayUrl = argv[i + 1];
        else if (flag == "--wait") waitSeconds = std::atoi(argv[i + 1]);
        else if (flag == "--tries") tries = std::atoi(argv[i + 1]);
        else {
            std::cerr << "unknown flag " << flag << "\n";
            return 2;
        }
    }
    const auto onion = net::parseUrl(onionApi);
    const auto gateway = net::parseUrl(gatewayUrl);
    if (keyHex.empty() || keyHex.size() % 2 != 0 || !onion || !net::isOnionHost(onion->host) || !gateway ||
        gateway->scheme != "https" || !onion->path.ends_with("/api/")) {
        std::cerr << "usage: redvault_sealed_route_check --key <KeyConfig hex> [--onion-api http://<onion>[:port]/.../api/]"
                     " [--gateway https://...]\n";
        return 2;
    }
    // "http://<onion>[:port]": the origin, and the port the gate must allow (80 when none is given).
    const std::string origin = onionApi.substr(0, onionApi.find('/', 7));
    const auto colon = origin.find(':', 7);
    const std::string onionPort = colon == std::string::npos ? "80" : origin.substr(colon + 1);

    const fs::path state = fs::temp_directory_path() / ("redvault-sealed-check-" + util::randomUuid());
    for (const fs::path& dir : {state, state / "state", state / "cache"}) {
        fs::create_directories(dir);
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
    }
    // The app's policy (app/src/tor_controller.cpp): the onion, plus the gateway website on 443.
    const Json config = {{"stateDir", (state / "state").string()},
                         {"cacheDir", (state / "cache").string()},
                         {"allowedTargets", Json::array({onion->host + ":" + onionPort})},
                         {"allowedExitTargets", Json::array({gateway->host + ":443"})}};
    char* error = nullptr;
    RvTorGateway* tor = rv_tor_start(config.dump().c_str(), &error);
    if (tor == nullptr) {
        std::cerr << "Tor didn't start: " << (error != nullptr ? error : "") << "\n";
        if (error != nullptr) rv_tor_free_string(error);
        return 1;
    }
    const Json endpoint = takeJson(rv_tor_endpoint(tor));
    const net::SocksEndpoint gate{endpoint.value("host", std::string()), endpoint.value("port", 0), endpoint.value("username", std::string()),
                                  endpoint.value("password", std::string())};

    const auto started = std::chrono::steady_clock::now();
    std::string last;
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(waitSeconds)) {
        const Json status = takeJson(rv_tor_status(tor));
        const std::string torState = status.value("state", std::string("unavailable"));
        const std::string line = torState + " " + std::to_string(status.value("progress", 0)) + "%";
        if (line != last) std::cerr << "tor: " << (last = line) << "\n";
        if (torState == "connected") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // The app's two transports (app/src/app_controller.cpp), with shorter connect timeouts so the check is quick.
    net::CurlTransport::Options onionOptions;
    onionOptions.allowedHost = onion->host;
    onionOptions.connectTimeout = std::chrono::seconds(30);
    onionOptions.stallTimeout = std::chrono::seconds(30);
    onionOptions.torGate = [gate]() -> std::optional<net::SocksEndpoint> { return gate; };
    auto onionTransport = std::make_shared<net::CurlTransport>(onionOptions);

    net::CurlTransport::Options website;
    website.allowedHost = gateway->host;
    website.connectTimeout = std::chrono::seconds(90);
    website.stallTimeout = std::chrono::seconds(60);
    website.torGate = [gate]() -> std::optional<net::SocksEndpoint> { return gate; };
    website.httpsOverTor = true;
    auto websiteTransport = std::make_shared<net::CurlTransport>(website);

    net::SealedFallbackTransport::Config routeConfig;
    routeConfig.onionOrigin = origin;
    routeConfig.pathPrefix = onion->path.substr(0, onion->path.size() - 5);
    routeConfig.gatewayUrl = gatewayUrl;
    routeConfig.probeUrl = onionApi + "public/routing";
    // The app switches after 3 onion failures or 30 s. The dead onion port now refuses within seconds, so a check
    // with fewer tries would never switch: switch on the last try at the latest (never later than the app would).
    routeConfig.switchAfterFailures = std::clamp(tries, 1, routeConfig.switchAfterFailures);
    for (std::size_t i = 0; i + 1 < keyHex.size(); i += 2) {
        routeConfig.keyConfig.push_back(static_cast<std::uint8_t>(std::stoi(keyHex.substr(i, 2), nullptr, 16)));
    }
    net::SealedFallbackTransport route(
        onionTransport, websiteTransport, std::move(routeConfig), [] { return true; },
        [](bool backup) { std::cerr << "route: " << (backup ? "backup (sealed)" : "onion") << "\n"; });

    int code = 1;
    for (int attempt = 1; attempt <= tries && code != 0; ++attempt) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            const auto response = route.send(net::HttpRequest{"GET", onionApi + "public/routing", {{"accept", "application/json"}}, {}});
            const Json body = Json::parse(response.body, nullptr, false);
            const bool viaSealed = body.is_object() && body.value("viaSealed", false);
            std::cout << Json{{"attempt", attempt},
                              {"status", response.status},
                              {"onBackup", route.onBackup()},
                              {"viaSealed", viaSealed},
                              {"viaOnion", body.is_object() ? body.value("viaOnion", Json(nullptr)) : Json(nullptr)},
                              {"mode", body.is_object() ? body.value("mode", Json(nullptr)) : Json(nullptr)},
                              {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()}}
                             .dump()
                      << "\n";
            if (response.status == 200 && viaSealed) code = 0;
        } catch (const std::exception& e) {
            std::cout << Json{{"attempt", attempt},
                              {"onBackup", route.onBackup()},
                              {"error", e.what()},
                              {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()}}
                             .dump()
                      << "\n";
        }
    }
    const auto onionSockets = onionTransport->socketLog();
    const auto websiteSockets = websiteTransport->socketLog();
    std::cerr << "sockets: " << onionSockets.allowed + websiteSockets.allowed << " to the Tor gate, "
              << onionSockets.refused.size() + websiteSockets.refused.size() << " refused\n";
    rv_tor_stop(tor);
    std::error_code ignored;
    fs::remove_all(state, ignored);
    return code;
}
