// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/rtc/relay_sdp.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

#include "redvault/rtc/call_media.hpp"

namespace redvault::rtc {

namespace {

constexpr std::string_view kPrefix = "a=candidate:";

[[noreturn]] void malformed() {
    throw RtcError("bad_candidate");
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool allDigits(std::string_view text, std::size_t maxLength) {
    if (text.empty() || text.size() > maxLength) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

/// A literal address only: a host name would have to be looked up, and that lookup is itself a leak.
bool isIpLiteral(const std::string& address) {
    in_addr v4{};
    if (inet_pton(AF_INET, address.c_str(), &v4) == 1) return v4.s_addr != 0;
    in6_addr v6{};
    if (inet_pton(AF_INET6, address.c_str(), &v6) != 1) return false;
    return std::any_of(std::begin(v6.s6_addr), std::end(v6.s6_addr), [](std::uint8_t b) { return b != 0; });
}

std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        std::size_t end = i;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
        if (end > i) out.push_back(line.substr(i, end - i));
        i = end;
    }
    return out;
}

/// RFC 8839: foundation component transport priority address port "typ" type *(name value).
bool relayFields(std::string_view body) {
    const auto fields = tokens(body);
    if (fields.size() < 8) malformed();
    if (fields[0].size() > 32 || !allDigits(fields[1], 3) || !allDigits(fields[3], 10) || !allDigits(fields[5], 5)) malformed();
    if (fields[6] != "typ") malformed();
    const int port = std::stoi(std::string(fields[5]));
    if (port < 1 || port > 65535) malformed();
    if (lower(fields[7]) != "relay" || lower(fields[2]) != "udp") return false;
    if (!isIpLiteral(std::string(fields[4]))) malformed();
    return true;
}

}  // namespace

bool isRelayCandidate(std::string_view candidate) {
    if (candidate.starts_with("a=")) candidate.remove_prefix(2);
    if (candidate.starts_with("candidate:")) candidate.remove_prefix(10);
    for (const unsigned char c : candidate) {
        if (c == 0x7f || (c < 0x20 && c != '\t')) malformed();
    }
    return relayFields(candidate);
}

RelayFilterResult keepRelayCandidates(std::string_view sdp) {
    // Only CRLF and LF end a line. libjuice drops a stray CR from the middle of a line, so "a=candi\rdate:..." would
    // be no candidate here and a candidate there. A NUL would cut the text short in C code further down.
    for (std::size_t i = 0; i < sdp.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(sdp[i]);
        const bool lineEnd = c == '\n' || (c == '\r' && i + 1 < sdp.size() && sdp[i + 1] == '\n');
        if (!lineEnd && c != '\t' && (c < 0x20 || c == 0x7f)) malformed();
    }

    RelayFilterResult out;
    out.sdp.reserve(sdp.size());
    std::size_t start = 0;
    while (start < sdp.size()) {
        const auto newline = sdp.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? sdp.size() : newline + 1;
        const std::string_view raw = sdp.substr(start, end - start);
        start = end;

        std::string_view line = raw;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.remove_prefix(1);

        // Anything that even resembles a candidate has to be exactly one.
        if (lower(trimmed.substr(0, 11)) != "a=candidate") {
            out.sdp.append(raw);
            continue;
        }
        if (!line.starts_with(kPrefix)) malformed();
        if (relayFields(line.substr(kPrefix.size()))) {
            out.sdp.append(raw);
            ++out.kept;
        } else {
            ++out.removed;
        }
    }
    return out;
}

}  // namespace redvault::rtc
