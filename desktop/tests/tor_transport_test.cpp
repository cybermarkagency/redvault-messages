// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// CurlTransport's Tor mode (docs/tor-routing-plan.md): every request goes to
// the onion host through the gateway's SOCKS5 port, nothing else is possible,
// and with no gateway nothing is sent at all. A fake SOCKS5 gate stands in
// for tor-core, so libcurl's real proxy handshake is exercised.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "redvault/net/curl_transport.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
static void closeSock(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
static void closeSock(Socket s) { close(s); }
#endif

using namespace redvault;

namespace {

const std::string kOnion = "2gzyxa5ihm7nsggfxnu52rck2vv4rvmdlkiu3zzui5du4xyclen53wid.onion";

bool readExact(Socket s, char* out, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const int r = recv(s, out + got, static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

/// A SOCKS5 gate like tor-core's: password auth only, then answers HTTP itself (keep-alive).
class FakeGate {
public:
    FakeGate() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        REQUIRE(listen(listener_, 16) == 0);
        socklen_t length = sizeof addr;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~FakeGate() {
        stopping_ = true;
        Socket wake = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<unsigned short>(port_));
        connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        thread_.join();
        for (auto& t : workers_) t.join();
        closeSock(wake);
        closeSock(listener_);
    }

    net::SocksEndpoint endpoint() const { return {"127.0.0.1", port_, "user-0123456789abcdef", "pass-0123456789abcdef"}; }
    int connections() const { return connections_; }
    std::vector<std::string> targets() const {
        std::lock_guard lock(mutex_);
        return targets_;
    }
    std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void run() {
        while (true) {
            Socket client = accept(listener_, nullptr, nullptr);
            if (stopping_) {
                if (client != kNoSocket) closeSock(client);
                return;
            }
            if (client == kNoSocket) continue;
            ++connections_;
            workers_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(Socket c) {
        char head[2];
        if (!readExact(c, head, 2) || head[0] != 5) return closeSock(c);
        std::string methods(static_cast<unsigned char>(head[1]), '\0');
        readExact(c, methods.data(), methods.size());
        if (methods.find('\x02') == std::string::npos) {
            send(c, "\x05\xFF", 2, 0);
            return closeSock(c);
        }
        send(c, "\x05\x02", 2, 0);
        char ver;
        unsigned char len;
        readExact(c, &ver, 1);
        readExact(c, reinterpret_cast<char*>(&len), 1);
        std::string user(len, '\0');
        readExact(c, user.data(), len);
        readExact(c, reinterpret_cast<char*>(&len), 1);
        std::string pass(len, '\0');
        readExact(c, pass.data(), len);
        if (user != endpoint().username || pass != endpoint().password) {
            send(c, "\x01\x01", 2, 0);
            return closeSock(c);
        }
        send(c, "\x01\x00", 2, 0);
        char req[4];
        readExact(c, req, 4);
        std::string target;
        if (req[3] == 3) {
            readExact(c, reinterpret_cast<char*>(&len), 1);
            target.resize(len);
            readExact(c, target.data(), len);
        } else {
            target = "ADDRESS-TYPE-" + std::to_string(req[3]);
            char skip[16];
            readExact(c, skip, req[3] == 1 ? 4 : 16);
        }
        char portBytes[2];
        readExact(c, portBytes, 2);
        target += ":" + std::to_string((static_cast<unsigned char>(portBytes[0]) << 8) | static_cast<unsigned char>(portBytes[1]));
        {
            std::lock_guard lock(mutex_);
            targets_.push_back(target);
        }
        send(c, "\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10, 0);
        // HTTP, keep-alive: answer requests until the client closes.
        std::string buffer;
        char chunk[4096];
        while (true) {
            const auto end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) {
                const int n = recv(c, chunk, sizeof chunk, 0);
                if (n <= 0) break;
                buffer.append(chunk, static_cast<std::size_t>(n));
                continue;
            }
            std::size_t total = end + 4;
            std::smatch m;
            const std::string headText = buffer.substr(0, end);
            if (std::regex_search(headText, m, std::regex("\r\ncontent-length: *([0-9]+)", std::regex::icase))) total += std::stoul(m[1]);
            while (buffer.size() < total) {
                const int n = recv(c, chunk, sizeof chunk, 0);
                if (n <= 0) return closeSock(c);
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(buffer.substr(0, total));
            }
            buffer.erase(0, total);
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"ok\":true}";
            send(c, response.data(), static_cast<int>(response.size()), 0);
        }
        closeSock(c);
    }

    Socket listener_ = kNoSocket;
    int port_ = 0;
    std::thread thread_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_ = false;
    std::atomic<int> connections_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> targets_;
    std::vector<std::string> requests_;
};

net::CurlTransport torTransport(std::function<std::optional<net::SocksEndpoint>()> gate) {
    net::CurlTransport::Options options;
    options.allowedHost = kOnion;
    options.torGate = std::move(gate);
    return net::CurlTransport(options);
}

}  // namespace

TEST_CASE("tor transport: requests go to the onion host through the gate, by name, with its password", "[tor]") {
    FakeGate gate;
    auto transport = torTransport([&] { return gate.endpoint(); });
    const auto response = transport.send({"POST", "http://" + kOnion + "/api/conversations/x/messages", {{"content-type", "application/json"}}, "{\"ciphertext\":\"AA\"}"});
    CHECK(response.status == 200);
    CHECK(response.body == "{\"ok\":true}");
    REQUIRE(gate.targets().size() == 1);
    // socks5h: the gate got the name, not an address resolved here.
    CHECK(gate.targets()[0] == kOnion + ":80");
    CHECK(gate.requests().at(0).rfind("POST /api/conversations/x/messages HTTP/1.1\r\n", 0) == 0);
    CHECK(transport.socketLog().allowed >= 1);
    CHECK(transport.socketLog().refused.empty());
}

TEST_CASE("tor transport: connections are reused (one Tor stream for several requests)", "[tor]") {
    FakeGate gate;
    auto transport = torTransport([&] { return gate.endpoint(); });
    for (int i = 0; i < 5; ++i) CHECK(transport.send({"GET", "http://" + kOnion + "/api/conversations", {}, {}}).status == 200);
    CHECK(gate.requests().size() == 5);
    CHECK(gate.connections() == 1);
}

TEST_CASE("tor transport: without a gateway nothing is sent and no socket is opened", "[tor]") {
    auto transport = torTransport([] { return std::nullopt; });
    CHECK_THROWS_AS(transport.send({"GET", "http://" + kOnion + "/api/auth/me", {}, {}}), net::TorUnavailableError);
    CHECK(transport.socketLog().allowed == 0);
    CHECK(transport.socketLog().refused.empty());
}

TEST_CASE("tor transport: other hosts, https and the clearnet are refused before any socket", "[tor]") {
    FakeGate gate;
    auto transport = torTransport([&] { return gate.endpoint(); });
    for (const std::string url : std::vector<std::string>{"https://www.redvaultsystems.com/redvaultmessages/api/auth/me", "http://127.0.0.1:3000/api/auth/me",
                                  "https://" + kOnion + "/api/auth/me", "http://example.com/"}) {
        CHECK_THROWS_AS(transport.send({"GET", url, {}, {}}), net::NetworkError);
    }
    CHECK(gate.connections() == 0);
    CHECK(transport.socketLog().allowed == 0);
}

TEST_CASE("tor transport: a gateway that went away fails closed (no direct retry)", "[tor]") {
    int deadPort = 0;
    {
        FakeGate gone;
        deadPort = gone.endpoint().port;
    }
    auto transport = torTransport([&] { return net::SocksEndpoint{"127.0.0.1", deadPort, "u", "p"}; });
    CHECK_THROWS_AS(transport.send({"GET", "http://" + kOnion + "/api/auth/me", {}, {}}), net::NetworkError);
    const auto log = transport.socketLog();
    CHECK(log.allowed == 1);  // the one attempt, to the gate
    CHECK(log.refused.empty());
}

TEST_CASE("tor transport: only a v3 onion server address is accepted", "[tor]") {
    net::CurlTransport::Options options;
    options.torGate = [] { return std::nullopt; };
    for (const std::string host : std::vector<std::string>{"www.redvaultsystems.com", "127.0.0.1", "expyuzz4wqqyqhjn.onion", ""}) {
        options.allowedHost = host;
        CHECK_THROWS_AS(net::CurlTransport(options), std::invalid_argument);
    }
    CHECK(net::isOnionHost(kOnion));
    CHECK_FALSE(net::isOnionHost("www." + kOnion));
    CHECK_FALSE(net::isOnionHost("2GZYXA5IHM7NSGGFXNU52RCK2VV4RVMDLKIU3ZZUI5DU4XYCLEN53WID.onion"));
}

TEST_CASE("tor transport: proxy environment variables can't redirect it", "[tor]") {
    FakeGate gate;
#ifdef _WIN32
    _putenv_s("ALL_PROXY", "socks5h://127.0.0.1:1");
    _putenv_s("NO_PROXY", "*");
#else
    setenv("ALL_PROXY", "socks5h://127.0.0.1:1", 1);
    setenv("NO_PROXY", "*", 1);
#endif
    auto transport = torTransport([&] { return gate.endpoint(); });
    CHECK(transport.send({"GET", "http://" + kOnion + "/health", {}, {}}).status == 200);
    CHECK(gate.connections() == 1);
#ifdef _WIN32
    _putenv_s("ALL_PROXY", "");
    _putenv_s("NO_PROXY", "");
#else
    unsetenv("ALL_PROXY");
    unsetenv("NO_PROXY");
#endif
}
