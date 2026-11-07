// End-to-end proof of concept: the desktop core's own repositories (real
// end-to-end encryption, real server code) talking to the scratch API only
// through the embedded Tor gateway and a v3 onion service.
//
// tor_e2e_driver --onion HOST.onion --state DIR --control http://127.0.0.1:18082
//
// Prints one JSON line per step on stdout; exit code 0 only if every step passed.
// The control URL is the harness's test hook (setting a plan, reading the
// server's request log): it is not app traffic and doesn't go through Tor.

#include <curl/curl.h>
#include <sodium.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>

#include "redvault/api/api_client.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/file_repository.hpp"
#include "redvault/data/friend_repository.hpp"
#include "redvault/net/cookie_jar.hpp"
#include "redvault_tor.h"
#include "tor_curl_transport.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace redvault;

namespace {

struct Args {
    std::string onion;
    fs::path state;
    std::string control;
};

int failures = 0;

void report(const std::string& step, bool ok, const json& detail, std::chrono::steady_clock::time_point began) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    json line{{"step", step}, {"ok", ok}, {"ms", ms.count()}, {"detail", detail}};
    std::cout << line.dump() << std::endl;
    if (!ok) ++failures;
}

/// Runs one step; an exception fails it with the message.
void step(const std::string& name, const std::function<json()>& body) {
    const auto began = std::chrono::steady_clock::now();
    try {
        json detail = body();
        const bool ok = !detail.contains("ok") || detail["ok"].get<bool>();
        detail.erase("ok");
        report(name, ok, detail, began);
    } catch (const std::exception& e) {
        report(name, false, json{{"error", e.what()}}, began);
    }
}

std::string takeString(char* text) {
    std::string out = text != nullptr ? text : "";
    rv_tor_free_string(text);
    return out;
}

std::size_t collect(char* data, std::size_t size, std::size_t count, void* out) {
    static_cast<std::string*>(out)->append(data, size * count);
    return size * count;
}

/// The harness's control port (loopback test hook, not app traffic).
std::string controlRequest(const std::string& url, const std::string& method, const std::string& body = {}) {
    CURL* handle = curl_easy_init();
    std::string out;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &collect);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &out);
    if (method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
    }
    const CURLcode code = curl_easy_perform(handle);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    if (code != CURLE_OK) throw std::runtime_error(std::string("control request failed: ") + curl_easy_strerror(code));
    return out;
}

std::string sha256Hex(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    char buffer[65536];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(buffer),
                                  static_cast<unsigned long long>(in.gcount()));
    }
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&state, digest);
    char hex[crypto_hash_sha256_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
    return hex;
}

/// A file of `size` random bytes behind a real-looking header (the server only ever sees ciphertext).
fs::path makeFile(const fs::path& dir, const std::string& name, const std::string& header, std::size_t size) {
    fs::create_directories(dir);
    const fs::path path = dir / name;
    std::string bytes(size, '\0');
    randombytes_buf(bytes.data(), bytes.size());
    bytes.replace(0, header.size(), header);
    std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::string randomSuffix() {
    std::random_device rd;
    std::uniform_int_distribution<int> digit(0, 9);
    std::string out;
    for (int i = 0; i < 6; ++i) out.push_back(static_cast<char>('0' + digit(rd)));
    return out;
}

Args parseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        const std::string value = argv[i + 1];
        if (flag == "--onion") args.onion = value;
        else if (flag == "--state") args.state = value;
        else if (flag == "--control") args.control = value;
    }
    if (args.onion.empty() || args.state.empty() || args.control.empty()) {
        std::cerr << "usage: tor_e2e_driver --onion HOST.onion --state DIR --control URL\n";
        std::exit(2);
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (sodium_init() < 0) return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    fs::create_directories(args.state);
    const std::string target = args.onion + ":80";
    const std::string apiBase = "http://" + args.onion + "/api/";

    auto transport = std::make_shared<net::TorCurlTransport>(net::TorCurlTransport::Options{args.onion});
    RvTorGateway* gateway = nullptr;
    net::TorSocksEndpoint endpoint;

    // Before Tor is up nothing may leave: the transport has no endpoint yet.
    step("refuse_before_tor_starts", [&] {
        try {
            transport->send({"GET", apiBase + "auth/me", {}, {}});
            return json{{"ok", false}, {"error", "a request went out without Tor"}};
        } catch (const net::NetworkError& e) {
            return json{{"refused", e.what()}, {"sockets", transport->socketLog().allowed.size()}};
        }
    });

    step("tor_start", [&] {
        const json config{{"stateDir", (args.state / "tor-state").string()},
                          {"cacheDir", (args.state / "tor-cache").string()},
                          {"allowedTargets", json::array({target})},
                          {"log", "warn"}};
        char* error = nullptr;
        gateway = rv_tor_start(config.dump().c_str(), &error);
        if (gateway == nullptr) return json{{"ok", false}, {"error", takeString(error)}};
        const json ep = json::parse(takeString(rv_tor_endpoint(gateway)));
        endpoint = {ep["host"], ep["port"], ep["username"], ep["password"]};
        transport->setEndpoint(endpoint);
        return json{{"socksPort", endpoint.port}};
    });
    if (gateway == nullptr) return 1;

    step("tor_connected", [&] {
        json transitions = json::array();
        std::string last;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(4);
        while (std::chrono::steady_clock::now() < deadline) {
            const json status = json::parse(takeString(rv_tor_status(gateway)));
            const std::string key = status["state"].get<std::string>() + " " + std::to_string(status["progress"].get<int>());
            if (key != last) {
                transitions.push_back(key);
                last = key;
            }
            if (status["state"] == "connected") return json{{"transitions", transitions}};
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return json{{"ok", false}, {"transitions", transitions}};
    });

    controlRequest(args.control + "/log/clear", "POST", "{}");

    auto newApi = [&] { return std::make_shared<api::ApiClient>(apiBase, transport, std::make_shared<net::SessionCookieJar>()); };
    auto aliceApi = newApi();
    auto bobApi = newApi();
    const std::string suffix = randomSuffix();
    const std::string aliceHandle = "toralice" + suffix;
    const std::string bobHandle = "torbob" + suffix;
    const std::string password = "correct horse battery staple " + suffix;
    std::optional<data::SelfIdentity> alice;
    std::optional<data::SelfIdentity> bob;

    step("register_accounts", [&] {
        data::AuthRepository aliceAuth(aliceApi);
        data::AuthRepository bobAuth(bobApi);
        alice.emplace(std::move(aliceAuth.registerAccount(aliceHandle, password, {true, true, true}).identity));
        bob.emplace(std::move(bobAuth.registerAccount(bobHandle, password, {true, true, true}).identity));
        // Files need a paid plan (harness hook).
        controlRequest(args.control + "/tier", "POST", json{{"userId", alice->userId}, {"tier", "pro"}}.dump());
        controlRequest(args.control + "/tier", "POST", json{{"userId", bob->userId}, {"tier", "pro"}}.dump());
        return json{{"alice", aliceHandle}, {"bob", bobHandle}};
    });
    if (!alice || !bob) return 1;

    auto aliceConversations = std::make_shared<data::ConversationRepository>(aliceApi);
    auto bobConversations = std::make_shared<data::ConversationRepository>(bobApi);
    std::optional<data::ConversationMeta> aliceDm;
    std::optional<data::ConversationMeta> bobDm;

    step("friend_request_and_accept", [&] {
        data::FriendRepository(aliceApi).sendRequest(bobHandle);
        data::FriendRepository bobFriends(bobApi);
        int accepted = 0;
        for (const auto& request : bobFriends.pending().incoming) {
            bobFriends.accept(*bob, request);
            ++accepted;
        }
        return json{{"ok", accepted == 1}, {"accepted", accepted}};
    });

    auto findDm = [](data::ConversationRepository& repo, const data::SelfIdentity& me, const std::string& otherId)
        -> std::optional<data::ConversationMeta> {
        for (const auto& dto : repo.listConversations()) {
            auto meta = repo.hydrateMeta(dto, me);
            if (meta && !meta->isGroup && meta->otherUserId == otherId) return meta;
        }
        return std::nullopt;
    };

    step("key_exchange_and_chat", [&] {
        aliceDm = findDm(*aliceConversations, *alice, bob->userId);
        bobDm = findDm(*bobConversations, *bob, alice->userId);
        const bool sameSafetyNumber = aliceDm && bobDm && aliceDm->safetyNumber == bobDm->safetyNumber;
        return json{{"ok", aliceDm.has_value() && bobDm.has_value() && sameSafetyNumber},
                    {"safetyNumbersMatch", sameSafetyNumber}};
    });
    if (!aliceDm || !bobDm) return 1;

    const std::string text = "Hello over Tor \xF0\x9F\xA7\x85 " + suffix;
    std::string sentId;
    step("text_message_send", [&] {
        const auto sent = aliceConversations->send(*aliceDm, text);
        sentId = sent.id;
        const bool verified = sent.security && sent.security->verification == data::VerificationStatus::Verified;
        return json{{"ok", verified}, {"commitmentVerified", verified}};
    });

    step("text_message_receive_and_decrypt", [&] {
        const auto result = bobConversations->fetchMessages(*bobDm, bob->userId);
        for (const auto& message : result.messages) {
            if (message.text == text) return json{{"decrypted", true}, {"unread", result.unread}};
        }
        return json{{"ok", false}, {"messages", result.messages.size()}};
    });

    step("read_receipt", [&] {
        bobConversations->markRead(bobDm->id, true);
        const auto result = aliceConversations->fetchMessages(*aliceDm, alice->userId);
        for (const auto& message : result.messages) {
            if (message.id == sentId) return json{{"ok", message.seen}, {"seen", message.seen}, {"delivered", message.delivered}};
        }
        return json{{"ok", false}, {"error", "sent message not found"}};
    });

    const fs::path files = args.state / "files";
    auto roundTrip = [&](const std::string& name, const std::string& header, std::size_t size) {
        const fs::path original = makeFile(files / "out", name, header, size);
        data::FileRepository aliceFiles(aliceApi, aliceConversations, files / "alice-cache");
        data::FileRepository bobFiles(bobApi, bobConversations, files / "bob-cache");
        aliceFiles.send(*aliceDm, original);
        const auto result = bobConversations->fetchMessages(*bobDm, bob->userId);
        for (auto it = result.messages.rbegin(); it != result.messages.rend(); ++it) {
            if (it->kind == data::MessageKind::File && it->file && it->text == name) {
                const fs::path decrypted = bobFiles.download(*it->file);
                const bool same = sha256Hex(decrypted) == sha256Hex(original);
                const auto mime = data::FileRepository::guessMimeType(original);
                aliceFiles.clearCache();
                bobFiles.clearCache();
                return json{{"ok", same}, {"bytes", size}, {"mime", mime}, {"sha256Match", same}};
            }
        }
        return json{{"ok", false}, {"error", "file message not found"}};
    };

    step("encrypted_photo", [&] { return roundTrip("tor-photo.jpg", "\xFF\xD8\xFF\xE0", 350 * 1024); });
    step("voice_message", [&] { return roundTrip("Voice message.m4a", std::string("\x00\x00\x00\x20" "ftypM4A ", 12), 120 * 1024); });
    step("encrypted_file", [&] { return roundTrip("report.pdf", "%PDF-1.7", 1024 * 1024); });

    step("live_long_poll", [&] {
        const auto first = api::live::events(*bobApi, std::nullopt);
        const auto began = std::chrono::steady_clock::now();
        const auto second = api::live::events(*bobApi, first.cursor);
        const auto held = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - began);
        return json{{"heldSeconds", held.count()}, {"events", second.events.size()}};
    });

    step("refuse_clearnet_host", [&] {
        const auto before = transport->socketLog().allowed.size();
        try {
            transport->send({"GET", "https://www.redvaultsystems.com/redvaultmessages/api/health", {}, {}});
            return json{{"ok", false}, {"error", "clearnet request went out"}};
        } catch (const net::NetworkError& e) {
            const auto after = transport->socketLog().allowed.size();
            return json{{"ok", before == after}, {"refused", e.what()}, {"socketsOpened", after - before}};
        }
    });

    step("refuse_direct_server_address", [&] {
        try {
            transport->send({"GET", "http://127.0.0.1:18081/api/conversations", {}, {}});
            return json{{"ok", false}, {"error", "direct request went out"}};
        } catch (const net::NetworkError& e) {
            return json{{"refused", e.what()}};
        }
    });

    step("gate_refuses_other_destinations", [&] {
        // Bypass the transport: talk to the gate directly with valid credentials.
        json results = json::array();
        bool allRefused = true;
        for (const std::string url : {"http://example.com/", "http://2gzyxa5ihm7nsggfxnu52rck2vv4rvmdlkiu3zzui5du4xyclen53wid.onion/",
                                      "http://93.184.215.14/"}) {
            CURL* handle = curl_easy_init();
            std::string sink;
            const std::string proxy = "socks5h://127.0.0.1:" + std::to_string(endpoint.port);
            curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
            curl_easy_setopt(handle, CURLOPT_PROXY, proxy.c_str());
            curl_easy_setopt(handle, CURLOPT_PROXYUSERNAME, endpoint.username.c_str());
            curl_easy_setopt(handle, CURLOPT_PROXYPASSWORD, endpoint.password.c_str());
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &collect);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
            curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
            const CURLcode code = curl_easy_perform(handle);
            curl_easy_cleanup(handle);
            allRefused = allRefused && code != CURLE_OK;
            results.push_back({{"url", url}, {"curl", curl_easy_strerror(code)}});
        }
        const json status = json::parse(takeString(rv_tor_status(gateway)));
        return json{{"ok", allRefused}, {"results", results}, {"gateStats", status["stats"]}};
    });

    step("socket_guard", [&] {
        const auto log = transport->socketLog();
        bool onlyGateway = true;
        for (const auto& entry : log.allowed) {
            onlyGateway = onlyGateway && entry == "127.0.0.1:" + std::to_string(endpoint.port);
        }
        return json{{"ok", onlyGateway && log.refused.empty()},
                    {"socketsToGateway", log.allowed.size()},
                    {"refusedSockets", log.refused}};
    });

    step("server_saw_only_onion", [&] {
        const json server = json::parse(controlRequest(args.control + "/log", "GET"));
        int onion = 0;
        int direct = 0;
        json paths = json::object();
        for (const auto& entry : server["log"]) {
            (entry["listener"] == "onion" ? onion : direct)++;
            const std::string key = entry["method"].get<std::string>() + " " + entry["path"].get<std::string>();
            paths[key] = paths.value(key, 0) + 1;
        }
        return json{{"ok", direct == 0 && onion > 0}, {"viaOnion", onion}, {"viaDirect", direct}, {"routes", paths.size()}};
    });

    step("fail_closed_when_tor_stops", [&] {
        const json serverBefore = json::parse(controlRequest(args.control + "/log", "GET"));
        rv_tor_stop(gateway);
        gateway = nullptr;
        bool threw = false;
        std::string error;
        try {
            // The transport still holds the old endpoint (as an app would until it hears Tor stopped).
            aliceConversations->send(*aliceDm, "this must not be delivered");
        } catch (const std::exception& e) {
            threw = true;
            error = e.what();
        }
        transport->clearEndpoint();
        bool threwAfterClear = false;
        try {
            aliceConversations->send(*aliceDm, "nor this");
        } catch (const std::exception&) {
            threwAfterClear = true;
        }
        const json serverAfter = json::parse(controlRequest(args.control + "/log", "GET"));
        const auto newRequests = serverAfter["log"].size() - serverBefore["log"].size();
        const auto log = transport->socketLog();
        return json{{"ok", threw && threwAfterClear && newRequests == 0 && log.refused.empty()},
                    {"sendFailed", threw},
                    {"error", error},
                    {"serverRequestsAfterStop", newRequests},
                    {"refusedSockets", log.refused}};
    });

    std::cout << json{{"summary", failures == 0 ? "all steps passed" : std::to_string(failures) + " step(s) failed"}}.dump()
              << std::endl;
    return failures == 0 ? 0 : 1;
}
