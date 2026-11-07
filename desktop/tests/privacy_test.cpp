// Proof that what the desktop app actually puts on the wire says nothing
// about the person: the real libcurl transport and ApiClient talk to a
// loopback server that records every request byte for byte. No device, OS,
// language, location or referrer headers go out, a sent message is only
// ciphertext and nonce, and no connection is ever opened to a host other
// than our own server. What the server saw is written to
// privacy-proof/desktop-wire-capture.txt in the test's working directory.
// A source scan backs this up for code paths the wire test doesn't reach.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/api/channel_api.hpp"
#include "redvault/net/cookie_jar.hpp"
#include "redvault/net/curl_transport.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(Socket s) { close(s); }
#endif

using namespace redvault;
namespace fs = std::filesystem;

namespace {

/// A minimal HTTP/1.1 server on 127.0.0.1 that keeps each raw request and answers 204.
class RecordingServer {
public:
    RecordingServer() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener_ != kNoSocket);
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

    ~RecordingServer() {
        stopping_ = true;
        // Wake the blocked accept() with one last connection.
        Socket wake = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<unsigned short>(port_));
        connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        thread_.join();
        closeSocket(wake);
        closeSocket(listener_);
    }

    int port() const { return port_; }
    int connections() const { return connections_; }

    std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void run() {
        while (true) {
            Socket client = accept(listener_, nullptr, nullptr);
            if (stopping_) {
                if (client != kNoSocket) closeSocket(client);
                return;
            }
            if (client == kNoSocket) continue;
            ++connections_;
            std::string raw = readRequest(client);
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(raw);
            }
            const std::string response =
                "HTTP/1.1 204 No Content\r\nSet-Cookie: sid=session; Path=/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, response.data(), static_cast<int>(response.size()), 0);
            closeSocket(client);
        }
    }

    static std::string readRequest(Socket client) {
        std::string raw;
        char buffer[4096];
        std::size_t headerEnd = std::string::npos;
        std::size_t total = 0;
        while (true) {
            const int n = recv(client, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            raw.append(buffer, static_cast<std::size_t>(n));
            if (headerEnd == std::string::npos && (headerEnd = raw.find("\r\n\r\n")) != std::string::npos) {
                std::smatch match;
                const std::string head = raw.substr(0, headerEnd);
                const std::regex contentLength("\r\ncontent-length: *([0-9]+)", std::regex::icase);
                total = headerEnd + 4 + (std::regex_search(head, match, contentLength) ? std::stoul(match[1]) : 0);
            }
            if (headerEnd != std::string::npos && raw.size() >= total) break;
        }
        return raw;
    }

    Socket listener_ = kNoSocket;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopping_ = false;
    std::atomic<int> connections_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
};

std::set<std::string> headerNames(const std::string& raw) {
    std::set<std::string> names;
    std::istringstream lines(raw.substr(0, raw.find("\r\n\r\n")));
    std::string line;
    std::getline(lines, line);  // request line
    while (std::getline(lines, line)) {
        if (const auto colon = line.find(':'); colon != std::string::npos) names.insert(net::toLower(line.substr(0, colon)));
    }
    return names;
}

/// The line up to a `//` comment; the `//` in "https://" doesn't count.
std::string stripComment(const std::string& line) {
    for (auto at = line.find("//"); at != std::string::npos; at = line.find("//", at + 2)) {
        if (at == 0 || line[at - 1] != ':') return line.substr(0, at);
    }
    return line;
}

std::string body(const std::string& raw) {
    const auto end = raw.find("\r\n\r\n");
    return end == std::string::npos ? "" : raw.substr(end + 4);
}

void appendToReport(const std::string& text) {
    fs::create_directories("privacy-proof");
    std::ofstream("privacy-proof/desktop-wire-capture.txt", std::ios::app | std::ios::binary) << text;
}

std::shared_ptr<api::ApiClient> realClient(int port) {
    net::CurlTransport::Options options;
    options.allowedHost = "localhost";
    return std::make_shared<api::ApiClient>("http://localhost:" + std::to_string(port) + "/api/",
                                            std::make_shared<net::CurlTransport>(options),
                                            std::make_shared<net::SessionCookieJar>());
}

}  // namespace

TEST_CASE("API calls carry no device, location or usage metadata", "[privacy]") {
    RecordingServer server;
    auto api = realClient(server.port());

    // The server's 204 isn't a valid answer to every call; only what was sent matters here.
    const auto attempt = [](auto&& call) {
        try {
            call();
        } catch (const std::exception&) {
        }
    };
    attempt([&] { api->login({"alice", "correct horse battery staple"}); });
    attempt([&] { api->sendMessage("conv-1", {"c2VhbGVk", "bm9uY2U=", std::nullopt}); });
    attempt([&] { api->markRead("conv-1", {"read"}); });
    attempt([&] { api->fetchAvatar("/api/users/0b9c3d1e-5f2a-4c6b-9e8d-7a1f2b3c4d5e/avatar?v=1"); });

    const auto requests = server.requests();
    REQUIRE(requests.size() == 4);
    const std::set<std::string> allowed{"host", "content-type", "content-length", "accept", "cookie"};
    std::string report;
    for (const auto& raw : requests) {
        report += "=== request as received\n" + raw + "\n\n";
        for (const auto& name : headerNames(raw)) {
            INFO(raw);
            CHECK(allowed.contains(name));
        }
    }
    appendToReport(report);

    // A message is only ciphertext and nonce: no time, time zone, device or recipient list.
    CHECK(api::Json::parse(body(requests[1])) == api::Json{{"ciphertext", "c2VhbGVk"}, {"nonce", "bm9uY2U="}});
    // The session cookie is the only thing tying requests together.
    CHECK(requests[1].find("\r\ncookie: sid=session") != std::string::npos);
}

TEST_CASE("reading a public channel carries no session cookie", "[privacy][channels]") {
    RecordingServer server;
    auto api = realClient(server.port());
    const auto attempt = [](auto&& call) {
        try {
            call();
        } catch (const std::exception&) {
        }
    };
    // Signed in: the server's answer sets the session cookie (the recording server sets one on every answer).
    attempt([&] { api->login({"alice", "correct horse battery staple"}); });
    attempt([&] { api::channels::search(*api, "news"); });
    attempt([&] { api::channels::get(*api, "6f1e2d3c-4b5a-4968-8776-655443322110"); });
    attempt([&] { api::channels::posts(*api, "6f1e2d3c-4b5a-4968-8776-655443322110"); });
    // A private channel (its link's access token, still no cookie), a channel's photo, and anonymous view counts.
    const std::string access = "KQ5oUWp7EuQY/qlUwD5APFsYvK80h+szX5tNzQAxJlk=";
    attempt([&] { api::channels::get(*api, "6f1e2d3c-4b5a-4968-8776-655443322110", access); });
    attempt([&] { api::channels::posts(*api, "6f1e2d3c-4b5a-4968-8776-655443322110", std::nullopt, access); });
    attempt([&] { api::channels::avatar(*api, "/api/public/channels/6f1e2d3c-4b5a-4968-8776-655443322110/avatar?v=1", access); });
    attempt([&] { api::channels::recordViews(*api, {"aaaaaaaa-0000-4000-8000-000000000001"}); });
    attempt([&] { api::channels::mine(*api); });

    const auto requests = server.requests();
    REQUIRE(requests.size() == 9);
    std::string report = "=== public channel reads, photo and view counts (no cookie), then a signed-in call (cookie)\n";
    for (std::size_t i = 1; i <= 7; ++i) {
        INFO(requests[i]);
        CHECK(requests[i].find("/api/public/channels") != std::string::npos);
        CHECK_FALSE(headerNames(requests[i]).contains("cookie"));
        // Only a private channel's reads say anything more: its link's token, never an account.
        CHECK(headerNames(requests[i]).contains("x-channel-access") == (i >= 4 && i <= 6));
        report += requests[i] + "\n\n";
    }
    // Signed-in calls still carry the session (channels_test checks a public answer's cookie isn't kept).
    CHECK(requests[8].find("\r\ncookie: sid=session") != std::string::npos);
    report += requests[8] + "\n\n";
    appendToReport(report);
}

TEST_CASE("channel media is fetched without the cookie, and uploads carry nothing but the session", "[privacy][channels]") {
    RecordingServer server;
    auto api = realClient(server.port());
    const auto attempt = [](auto&& call) {
        try {
            call();
        } catch (const std::exception&) {
        }
    };
    const std::string media = "/api/channel-media/bbbbbbbb-1111-4111-8111-000000000002?exp=1792354770&sig=abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123456";
    const auto path = (fs::temp_directory_path() / "redvault-privacy-upload.bin").string();
    {
        std::ofstream out(path, std::ios::binary);
        out << "photo bytes";
    }
    attempt([&] { api->login({"alice", "correct horse battery staple"}); });
    attempt([&] { api::channels::fetchMedia(*api, media); });
    attempt([&] { api::channels::fetchMediaRange(*api, media, 0, 99); });
    attempt([&] { api::channels::uploadMedia(*api, "/api/channel-uploads/bbbbbbbb-1111-4111-8111-000000000002", path, 11); });
    fs::remove(path);

    const auto requests = server.requests();
    REQUIRE(requests.size() == 4);
    std::string report = "=== channel media read (no cookie), a byte range of it, then an upload (cookie)\n";
    const std::set<std::string> allowed{"host", "content-type", "content-length", "accept", "cookie", "range"};
    for (std::size_t i = 1; i <= 3; ++i) {
        INFO(requests[i]);
        for (const auto& name : headerNames(requests[i])) CHECK(allowed.contains(name));
        report += requests[i] + "\n\n";
    }
    CHECK_FALSE(headerNames(requests[1]).contains("cookie"));
    CHECK_FALSE(headerNames(requests[2]).contains("cookie"));
    CHECK(requests[2].find("\r\nrange: bytes=0-99") != std::string::npos);
    // The upload is the owner's own, signed in: the session, the bytes, and nothing about the computer.
    CHECK(requests[3].find("PUT /api/channel-uploads/") == 0);
    CHECK(requests[3].find("\r\ncookie: sid=session") != std::string::npos);
    CHECK(requests[3].find("\r\ncontent-type: application/octet-stream") != std::string::npos);
    appendToReport(report);
}

TEST_CASE("no connection is ever opened to a third party", "[privacy]") {
    RecordingServer server;
    net::CurlTransport::Options options;
    options.allowedHost = "localhost";
    net::CurlTransport transport(options);

    // Same machine, different host name: "localhost" is our server, 127.0.0.1 plays a third party.
    const std::string thirdParty = "http://127.0.0.1:" + std::to_string(server.port()) + "/presigned-upload";
    CHECK_THROWS_AS(transport.send({"PUT", thirdParty, {}, "ciphertext"}), net::NetworkError);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(server.connections() == 0);
    appendToReport("=== third-party request to " + thirdParty + "\nrefused before connecting; connections the server saw: " +
                   std::to_string(server.connections()) + "\n\n");
}

TEST_CASE("no shipped source reads location, fingerprints the machine, or phones home", "[privacy]") {
    const fs::path root = REDVAULT_SOURCE_DIR;
    std::vector<std::pair<std::string, std::regex>> forbidden = {
        {"location", std::regex(R"(QGeo|QtPositioning|Geolocation|geolocation)")},
        {"machine fingerprinting",
         std::regex(R"(QSysInfo::(machineUniqueId|bootUniqueId|machineHostName|prettyProductName|productType|productVersion|kernelVersion)|QNetworkInterface|hardwareAddress|GetComputerName|gethostname|GetUserName|getlogin|uname\()")},
        {"a User-Agent", std::regex(R"(CURLOPT_USERAGENT|User-Agent|user-agent)")},
        {"other network stacks", std::regex(R"(QNetworkAccessManager|QWebEngine|QTcpSocket|QUdpSocket|QDnsLookup|XMLHttpRequest)")},
        {"telemetry or crash reporting", std::regex(R"(sentry|crashpad|breakpad|telemetry|analytics|Analytics)")},
        {"a server other than our own", std::regex(R"(https?://(?!www\.redvaultsystems\.com/)[A-Za-z])")},
    };
    int scanned = 0;
    for (const char* dir : {"app/src", "app/qml", "core/src", "core/include", "groupcall/src", "groupcall/include"}) {
        for (const auto& entry : fs::recursive_directory_iterator(root / dir)) {
            const auto ext = entry.path().extension();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".qml") continue;
            ++scanned;
            std::ifstream in(entry.path());
            std::string line;
            int number = 0;
            while (std::getline(in, line)) {
                ++number;
                // Comments may describe what the code doesn't do; only code counts.
                const auto code = stripComment(line);
                const auto first = code.find_first_not_of(" \t");
                if (first == std::string::npos || code[first] == '*' || code.compare(first, 2, "/*") == 0) continue;
                for (const auto& [what, pattern] : forbidden) {
                    if (std::regex_search(code, pattern)) {
                        FAIL_CHECK(what << " in " << entry.path().string() << ":" << number << ": " << line);
                    }
                }
            }
        }
    }
    CHECK(scanned > 40);
}
