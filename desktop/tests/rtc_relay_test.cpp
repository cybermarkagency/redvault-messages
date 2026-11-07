// Relay-only calls, tested with real ICE agents. A real TURN server (libjuice's own) runs on loopback, and plain UDP
// sockets stand in for an address the other person wants this computer to reveal itself to: each one records every
// datagram it gets and which port it came from. Relay traffic arrives from the TURN server's relay ports, so a packet
// from anywhere else means this computer talked to that address directly.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <juice/juice.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "redvault/rtc/call_media.hpp"
#include "redvault/rtc/relay_sdp.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(Socket s) { close(s); }
#endif

using namespace redvault::rtc;
using namespace std::chrono_literals;

namespace {

template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

/// A UDP port on loopback that writes down where every datagram came from.
class UdpSink {
public:
    explicit UdpSink(bool ipv6) {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        socket_ = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
        if (socket_ == kNoSocket) return;
        sockaddr_storage address{};
        socklen_t length;
        if (ipv6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&address);
            a->sin6_family = AF_INET6;
            a->sin6_addr = in6addr_loopback;
            length = sizeof(sockaddr_in6);
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&address);
            a->sin_family = AF_INET;
            a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            length = sizeof(sockaddr_in);
        }
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
            getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            closeSocket(socket_);
            socket_ = kNoSocket;
            return;
        }
        port_ = ntohs(ipv6 ? reinterpret_cast<sockaddr_in6*>(&address)->sin6_port : reinterpret_cast<sockaddr_in*>(&address)->sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~UdpSink() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (socket_ != kNoSocket) closeSocket(socket_);
    }

    bool ready() const { return socket_ != kNoSocket; }
    int port() const { return port_; }
    std::vector<int> sources() {
        std::lock_guard lock(mutex_);
        return sources_;
    }

private:
    void run() {
        while (!stop_) {
            fd_set read;
            FD_ZERO(&read);
            FD_SET(socket_, &read);
            timeval timeout{0, 50'000};
            if (select(static_cast<int>(socket_) + 1, &read, nullptr, nullptr, &timeout) <= 0) continue;
            char buffer[2048];
            sockaddr_storage from{};
            socklen_t length = sizeof(from);
            if (recvfrom(socket_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &length) < 0) continue;
            const int port = ntohs(from.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&from)->sin6_port
                                                              : reinterpret_cast<sockaddr_in*>(&from)->sin_port);
            std::lock_guard lock(mutex_);
            sources_.push_back(port);
        }
    }

    Socket socket_ = kNoSocket;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::vector<int> sources_;
};

/// A real TURN server on loopback. Test cases run in parallel processes, so each picks its own ports.
class LoopbackTurn {
public:
    LoopbackTurn() {
        std::mt19937 random{std::random_device{}()};
        for (int attempt = 0; attempt < 50 && !server_; ++attempt) {
            port_ = 20000 + static_cast<int>(random() % 900) * 10;
            juice_server_credentials_t credentials{};
            credentials.username = "rv";
            credentials.password = "relay-test";
            credentials.allocations_quota = 16;
            juice_server_config_t config{};
            config.credentials = &credentials;
            config.credentials_count = 1;
            config.max_allocations = 32;
            config.max_peers = 16;
            config.bind_address = "127.0.0.1";
            config.external_address = "127.0.0.1";
            config.port = static_cast<std::uint16_t>(port_);
            config.relay_port_range_begin = static_cast<std::uint16_t>(port_ + 1);
            config.relay_port_range_end = static_cast<std::uint16_t>(port_ + 9);
            config.realm = "redvault-test";
            server_ = juice_server_create(&config);
        }
    }
    ~LoopbackTurn() {
        if (server_) juice_server_destroy(server_);
    }

    bool ready() const { return server_ != nullptr; }
    IceServer server() const { return IceServer{"turn:127.0.0.1:" + std::to_string(port_) + "?transport=udp", "rv", "relay-test"}; }
    bool isRelayPort(int port) const { return port > port_ && port <= port_ + 9; }

private:
    juice_server_t* server_ = nullptr;
    int port_ = 0;
};

Config relayConfig(const LoopbackTurn& turn) {
    Config config;
    config.servers = {turn.server()};
    config.relayOnly = true;
    config.gatherTimeoutMs = 5000;
    return config;
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    return text;
}

/// Adds these lines at session level and again in front of the first candidate (the media section).
std::string withLines(std::string sdp, const std::string& lines) {
    const auto media = sdp.find("a=candidate:");
    REQUIRE(media != std::string::npos);
    sdp.insert(media, lines);
    const auto session = sdp.find("\nt=");
    REQUIRE(session != std::string::npos);
    sdp.insert(sdp.find('\n', session + 1) + 1, lines);
    return sdp;
}

/// Takes out every candidate the SDP has.
std::string withoutCandidates(std::string sdp) {
    for (std::size_t at = sdp.find("a=candidate:"); at != std::string::npos; at = sdp.find("a=candidate:", at)) {
        sdp.erase(at, sdp.find('\n', at) + 1 - at);
    }
    return sdp;
}

std::string candidate(const std::string& address, int port, const std::string& type) {
    return "a=candidate:9 1 UDP 2130706431 " + address + " " + std::to_string(port) + " typ " + type + "\r\n";
}

}  // namespace

// --- The filter ---

TEST_CASE("the filter keeps relay candidates and drops every other kind", "[rtc][relay]") {
    const std::string sdp =
        "v=0\r\n"
        "a=candidate:1 1 UDP 100 203.0.113.9 3478 typ relay raddr 0.0.0.0 rport 0\r\n"
        "a=candidate:2 1 udp 100 192.0.2.1 5000 typ host\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=candidate:3 1 UDP 100 2001:db8::1 3478 typ relay\r\n"
        "a=candidate:4 1 UDP 100 2001:db8::2 5000 typ host\r\n"
        "a=candidate:5 1 UDP 100 192.0.2.2 5000 typ srflx raddr 10.0.0.1 rport 1\r\n"
        "a=candidate:6 1 UDP 100 192.0.2.3 5000 typ prflx\r\n"
        "a=candidate:7 1 UDP 100 abcd-1234.local 5000 typ host\r\n"
        "a=candidate:8 1 TCP 100 192.0.2.4 9 typ host tcptype active\r\n"
        "a=candidate:9 1 TCP 100 192.0.2.5 443 typ relay\r\n"
        "a=end-of-candidates\r\n";
    const auto result = keepRelayCandidates(sdp);
    CHECK(result.kept == 2);
    CHECK(result.removed == 7);
    CHECK(result.sdp.find("203.0.113.9") != std::string::npos);
    CHECK(result.sdp.find("2001:db8::1") != std::string::npos);
    for (const char* gone : {"192.0.2.", "2001:db8::2", ".local"}) CHECK(result.sdp.find(gone) == std::string::npos);
    // Everything that isn't a candidate stays, untouched.
    CHECK(result.sdp.find("a=mid:0\r\n") != std::string::npos);
    CHECK(result.sdp.find("a=end-of-candidates\r\n") != std::string::npos);
}

TEST_CASE("the filter reads bare-LF SDP the same way", "[rtc][relay]") {
    const auto result = keepRelayCandidates("v=0\na=candidate:1 1 UDP 100 203.0.113.9 3478 typ relay\na=candidate:2 1 UDP 100 192.0.2.1 5000 typ host\n");
    CHECK(result.kept == 1);
    CHECK(result.removed == 1);
}

TEST_CASE("a malformed candidate refuses the whole description", "[rtc][relay]") {
    const std::string line = GENERATE(as<std::string>{},
        "a=candidate:1 1 UDP 100 203.0.113.9 3478 typ",                      // no type
        "a=candidate:1 1 UDP 100 203.0.113.9",                               // cut short
        "a=candidate:1 1 UDP 100 203.0.113.9 3478 type relay",               // not "typ"
        "a=candidate:1 x UDP 100 203.0.113.9 3478 typ relay",                // component
        "a=candidate:1 1 UDP abc 203.0.113.9 3478 typ relay",                // priority
        "a=candidate:1 1 UDP 100 203.0.113.9 0 typ relay",                   // port 0
        "a=candidate:1 1 UDP 100 203.0.113.9 70000 typ relay",               // port out of range
        "a=candidate:1 1 UDP 100 203.0.113.9 http typ relay",                // a service name
        "a=candidate:1 1 UDP 100 relay.attacker.example 3478 typ relay",     // would need a DNS lookup
        "a=candidate:1 1 UDP 100 abcd-1234.local 3478 typ relay",            // mDNS
        "a=candidate:1 1 UDP 100 0.0.0.0 3478 typ relay",                    // unspecified
        "a=candidate:1 1 UDP 100 :: 3478 typ relay",                         // unspecified, IPv6
        "a=candidate:1 1 UDP 100 999.1.1.1 3478 typ relay",                  // not an address
        "a=candidate:1 1 UDP 100 fe80::1%eth0 3478 typ relay",               // zone
        "a=candidate 1 1 UDP 100 203.0.113.9 3478 typ relay",                // not the exact prefix
        "A=CANDIDATE:1 1 UDP 100 203.0.113.9 3478 typ relay",                // another parser might still read it
        " a=candidate:1 1 UDP 100 192.0.2.1 3478 typ host");                 // indented
    CAPTURE(line);
    CHECK_THROWS_AS(keepRelayCandidates("v=0\r\n" + line + "\r\n"), RtcError);
}

TEST_CASE("line tricks that parsers read differently are refused", "[rtc][relay]") {
    // libjuice drops a stray CR from the middle of a line, which would turn this back into a candidate.
    CHECK_THROWS_AS(keepRelayCandidates("v=0\r\na=candi\rdate:1 1 UDP 100 192.0.2.1 5000 typ host\r\n"), RtcError);
    CHECK_THROWS_AS(keepRelayCandidates("v=0\r\na=ice-ufrag:x\ra=candidate:1 1 UDP 100 192.0.2.1 5000 typ host\r\n"), RtcError);
    CHECK_THROWS_AS(keepRelayCandidates(std::string("v=0\r\na=x\0y\r\n", 12)), RtcError);
    CHECK_THROWS_AS(keepRelayCandidates("v=0\r\na=\x01y\r\n"), RtcError);
    // A trailing CR with no LF is a stray CR too.
    CHECK_THROWS_AS(keepRelayCandidates("v=0\r"), RtcError);
}

TEST_CASE("one candidate on its own is judged the same way", "[rtc][relay]") {
    CHECK(isRelayCandidate("a=candidate:1 1 UDP 100 203.0.113.9 3478 typ relay"));
    CHECK(isRelayCandidate("candidate:1 1 UDP 100 2001:db8::1 3478 typ relay"));
    CHECK_FALSE(isRelayCandidate("a=candidate:1 1 UDP 100 192.0.2.1 5000 typ host"));
    CHECK_FALSE(isRelayCandidate("candidate:1 1 UDP 100 2001:db8::2 5000 typ srflx"));
    CHECK_THROWS_AS(isRelayCandidate("a=candidate:1 1 UDP 100 192.0.2.1"), RtcError);
    CHECK_THROWS_AS(isRelayCandidate("candidate:1 1 UDP 100 relay.attacker.example 3478 typ relay"), RtcError);
    CHECK_THROWS_AS(isRelayCandidate("a=candidate:1 1 UDP 100 203.0.113.9 3478 typ relay\r\na=x"), RtcError);
}

// --- Real calls through a real relay ---

TEST_CASE("control: a relay-only call connects through the relay, both ways, with media", "[rtc][relay]") {
    LoopbackTurn turn;
    REQUIRE(turn.ready());
    std::atomic<bool> callerUp{false}, calleeUp{false};
    CallMedia caller(relayConfig(turn), Callbacks{[&](ConnectionState s) { callerUp = callerUp || s == ConnectionState::Connected; }, {}});
    CallMedia callee(relayConfig(turn), Callbacks{[&](ConnectionState s) { calleeUp = calleeUp || s == ConnectionState::Connected; }, {}});

    const std::string offer = caller.createOffer();
    CHECK(offer.find(" typ relay") != std::string::npos);
    CHECK(offer.find(" typ host") == std::string::npos);
    CHECK(offer.find(" typ srflx") == std::string::npos);
    const std::string answer = callee.acceptOffer(offer);
    CHECK(answer.find(" typ host") == std::string::npos);
    caller.setAnswer(answer);
    REQUIRE(waitUntil([&] { return callerUp && calleeUp; }, 15s));
    CHECK(caller.connectedThroughRelay());
    CHECK(callee.connectedThroughRelay());

    std::vector<std::int16_t> pcm(kFrameSamples, 1000);
    for (int i = 0; i < 40; ++i) {
        caller.sendAudio(pcm.data());
        std::this_thread::sleep_for(20ms);
    }
    CHECK(waitUntil([&] { return callee.audioPacketsReceived() > 10; }, 5s));
}

TEST_CASE("host, srflx and prflx candidates in an offer get no packet, and the relay still connects", "[rtc][relay]") {
    LoopbackTurn turn;
    REQUIRE(turn.ready());
    UdpSink v4(false), v6(true);
    REQUIRE(v4.ready());
    std::atomic<bool> callerUp{false}, calleeUp{false};
    CallMedia caller(relayConfig(turn), Callbacks{[&](ConnectionState s) { callerUp = callerUp || s == ConnectionState::Connected; }, {}});
    CallMedia callee(relayConfig(turn), Callbacks{[&](ConnectionState s) { calleeUp = calleeUp || s == ConnectionState::Connected; }, {}});

    std::string extra = candidate("127.0.0.1", v4.port(), "host") + candidate("127.0.0.1", v4.port(), "srflx raddr 0.0.0.0 rport 0") +
                        candidate("127.0.0.1", v4.port(), "prflx");
    if (v6.ready()) extra += candidate("::1", v6.port(), "host");
    const std::string answer = callee.acceptOffer(withLines(caller.createOffer(), extra));

    // None of them even reached the library.
    CHECK(callee.remoteSdp().find(std::to_string(v4.port())) == std::string::npos);
    caller.setAnswer(answer);
    REQUIRE(waitUntil([&] { return callerUp && calleeUp; }, 15s));
    CHECK(callee.connectedThroughRelay());
    std::this_thread::sleep_for(2s);
    CHECK(v4.sources().empty());
    if (v6.ready()) CHECK(v6.sources().empty());
}

TEST_CASE("a host address labelled relay is reached only through the TURN relay, never directly", "[rtc][relay]") {
    LoopbackTurn turn;
    REQUIRE(turn.ready());
    UdpSink v4(false), v6(true);
    REQUIRE(v4.ready());
    CallMedia caller(relayConfig(turn), Callbacks{});
    CallMedia callee(relayConfig(turn), Callbacks{});

    // The label is all the filter can see, so this one passes it. Only these candidates are left to check.
    std::string lies = candidate("127.0.0.1", v4.port(), "relay raddr 0.0.0.0 rport 0");
    if (v6.ready()) lies += candidate("::1", v6.port(), "relay raddr 0.0.0.0 rport 0");
    const std::string offer = withoutCandidates(caller.createOffer());
    const auto media = offer.find("a=mid:");
    REQUIRE(media != std::string::npos);
    std::string evil = offer;
    evil.insert(evil.find('\n', media) + 1, lies);

    callee.acceptOffer(evil);
    CHECK(callee.remoteSdp().find(std::to_string(v4.port())) != std::string::npos);  // passed through, as a relay candidate

    // The checks do go out (this is what makes the next check mean something) ...
    REQUIRE(waitUntil([&] { return !v4.sources().empty(); }, 8s));
    std::this_thread::sleep_for(2s);
    // ... and every one came from the relay, never from this computer's own socket.
    for (const int source : v4.sources()) {
        CAPTURE(source);
        CHECK(turn.isRelayPort(source));
    }
    // The only allocation is IPv4, so an IPv6 address has no way through the relay and hears nothing at all.
    if (v6.ready()) CHECK(v6.sources().empty());
}

TEST_CASE("a real peer behind host candidates can't reach a relay-only call by relabelling them", "[rtc][relay]") {
    const bool relayOnly = GENERATE(true, false);
    CAPTURE(relayOnly);

    Config direct;
    direct.relayOnly = false;
    direct.gatherTimeoutMs = 3000;
    std::atomic<bool> attackerUp{false};
    CallMedia attacker(direct, Callbacks{[&](ConnectionState s) { attackerUp = attackerUp || s == ConnectionState::Connected; }, {}});
    const std::string offer = replaceAll(attacker.createOffer(), " typ host", " typ relay");
    REQUIRE(offer.find(" typ relay") != std::string::npos);

    // Configured like the app's call, with no TURN server: no legitimate path exists.
    // relayOnly false is the control: the same steps do connect, so the attack below is real.
    Config victimConfig;
    victimConfig.relayOnly = relayOnly;
    victimConfig.gatherTimeoutMs = 3000;
    CallMedia victim(victimConfig, Callbacks{});

    auto accepting = std::async(std::launch::async, [&] {
        try {
            return victim.acceptOffer(offer);
        } catch (const RtcError&) {
            return std::string();
        }
    });
    // The answer never goes out when there's no relay, but the attacker can guess at everything except the ICE
    // credentials, so hand them over: this is the worst case, as if they'd leaked.
    REQUIRE(waitUntil([&] { return !victim.localSdp().empty(); }, 3s));
    attacker.setAnswer(replaceAll(victim.localSdp(), "a=setup:actpass", "a=setup:active"));

    if (relayOnly) {
        CHECK_FALSE(waitUntil([&] { return attackerUp.load(); }, 6s));
        CHECK(accepting.get().empty());
        CHECK_FALSE(victim.connectedThroughRelay());
    } else {
        CHECK(waitUntil([&] { return attackerUp.load(); }, 10s));
        CHECK_FALSE(accepting.get().empty());
    }
}

TEST_CASE("an offer with no relay candidate is refused at once", "[rtc][relay]") {
    Config direct;
    direct.relayOnly = false;
    direct.gatherTimeoutMs = 3000;
    CallMedia attacker(direct, Callbacks{});
    const std::string offer = attacker.createOffer();

    Config victimConfig;
    victimConfig.relayOnly = true;
    victimConfig.gatherTimeoutMs = 3000;
    CallMedia victim(victimConfig, Callbacks{});
    CHECK_THROWS_AS(victim.acceptOffer(offer), RtcError);
    CHECK(victim.remoteSdp().empty());
}

TEST_CASE("an answer with only host candidates is refused, and nothing connects", "[rtc][relay]") {
    LoopbackTurn turn;
    REQUIRE(turn.ready());
    Config direct;
    direct.relayOnly = false;
    direct.gatherTimeoutMs = 3000;
    std::atomic<bool> callerUp{false};
    CallMedia caller(relayConfig(turn), Callbacks{[&](ConnectionState s) { callerUp = callerUp || s == ConnectionState::Connected; }, {}});
    CallMedia attacker(direct, Callbacks{});
    const std::string answer = attacker.acceptOffer(caller.createOffer());
    CHECK_THROWS_AS(caller.setAnswer(answer), RtcError);
    CHECK(caller.remoteSdp().empty());
    std::this_thread::sleep_for(2s);
    CHECK_FALSE(callerUp.load());
}

TEST_CASE("a malformed candidate in an offer or answer refuses it", "[rtc][relay]") {
    LoopbackTurn turn;
    REQUIRE(turn.ready());
    CallMedia caller(relayConfig(turn), Callbacks{});
    CallMedia callee(relayConfig(turn), Callbacks{});
    const std::string offer = caller.createOffer();
    const std::string bad = "a=candidate:1 1 UDP 100 relay.attacker.example 3478 typ relay\r\n";
    CHECK_THROWS_AS(callee.acceptOffer(withLines(offer, bad)), RtcError);
    CHECK(callee.remoteSdp().empty());
}

TEST_CASE("candidates that arrive on their own pass the same check", "[rtc][relay]") {
    LoopbackTurn turn;
    REQUIRE(turn.ready());
    UdpSink sink(false);
    REQUIRE(sink.ready());
    CallMedia caller(relayConfig(turn), Callbacks{});
    CallMedia callee(relayConfig(turn), Callbacks{});
    callee.acceptOffer(caller.createOffer());

    const std::string port = std::to_string(sink.port());
    CHECK_FALSE(callee.addRemoteCandidate("a=candidate:1 1 UDP 100 127.0.0.1 " + port + " typ host", "0"));
    CHECK_FALSE(callee.addRemoteCandidate("a=candidate:1 1 UDP 100 127.0.0.1 " + port + " typ srflx raddr 0.0.0.0 rport 0", "0"));
    CHECK_THROWS_AS(callee.addRemoteCandidate("a=candidate:1 1 UDP 100 127.0.0.1", "0"), RtcError);
    CHECK_THROWS_AS(callee.addRemoteCandidate("a=candidate:1 1 UDP 100 relay.attacker.example 3478 typ relay", "0"), RtcError);
    std::this_thread::sleep_for(1500ms);
    CHECK(sink.sources().empty());

    // Control: a relay candidate does go in.
    CHECK(callee.addRemoteCandidate("a=candidate:1 1 UDP 100 127.0.0.1 " + port + " typ relay", "0"));
    CHECK(callee.remoteSdp().find(port) != std::string::npos);
}
