// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/studio/captions.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <regex>

#include <nlohmann/json.hpp>

#include "redvault/util/text.hpp"

namespace redvault::studio::captions {

namespace st = model::stories;

namespace {

/// Math.round: halves go up.
std::int64_t jsRound(double v) {
    return static_cast<std::int64_t>(std::floor(v + 0.5));
}

/// What JavaScript's trim() and `\s` count as white space.
bool isJsSpace(char32_t c) {
    switch (c) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r': case U' ': case 0xA0: case 0x1680:
        case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

std::u32string jsTrim(std::u32string_view s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && isJsSpace(s[a])) ++a;
    while (b > a && isJsSpace(s[b - 1])) --b;
    return std::u32string(s.substr(a, b - a));
}

std::string jsTrim(std::string_view utf8) {
    return util::encodeUtf8(jsTrim(util::decodeUtf8(utf8)));
}

std::vector<std::string> split(std::string_view s, std::string_view by) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto at = s.find(by, start);
        if (at == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            return out;
        }
        out.emplace_back(s.substr(start, at - start));
        start = at + by.size();
    }
}

/// toFixed(1) of a non-negative double below 1e21: the nearest tenth to its exact value, the larger one on a tie.
std::string toFixed1(double x) {
    // x = mantissa * 2^exponent exactly; compare x * 10 with n + 1/2 in whole numbers.
    int exp2 = 0;
    const double frac = std::frexp(x, &exp2);  // x = frac * 2^exp2, 0.5 <= frac < 1
    const auto mantissa = static_cast<unsigned __int128>(std::ldexp(frac, 53));
    const int e = exp2 - 53;  // x = mantissa * 2^e
    // The result is the largest k with x * 10 >= k - 1/2, i.e. 20 * mantissa * 2^e >= 2k - 1. Floating point gets it
    // right to within one, so the neighbours of its guess are checked exactly.
    const auto guess = static_cast<std::int64_t>(std::floor(x * 10 + 0.5));
    const auto atLeast = [&](std::int64_t k) {
        if (k <= 0) return true;
        unsigned __int128 lhs = mantissa * 20;
        auto rhs = static_cast<unsigned __int128>(2 * k - 1);
        if (e >= 0) lhs <<= e;
        else rhs <<= -e;
        return lhs >= rhs;
    };
    std::int64_t best = std::max<std::int64_t>(0, guess - 1);
    for (std::int64_t k = best; k <= guess + 1; ++k) {
        if (atLeast(k)) best = k;
    }
    const std::string digits = std::to_string(best);
    if (digits.size() == 1) return "0." + digits;
    return digits.substr(0, digits.size() - 1) + "." + digits.substr(digits.size() - 1);
}

}  // namespace

std::string formatClock(double ms) {
    const std::int64_t total = std::max<std::int64_t>(0, jsRound(ms));
    const std::int64_t minutes = total / 60000;
    const double seconds = static_cast<double>(total % 60000) / 1000.0;
    std::string text = toFixed1(seconds);
    while (text.size() < 4) text = "0" + text;
    return std::to_string(minutes) + ":" + text;
}

std::optional<std::int64_t> parseClock(std::string_view raw) {
    std::string text = jsTrim(raw);
    if (const auto comma = text.find(','); comma != std::string::npos) text[comma] = '.';
    const auto parts = split(text, ":");
    if (parts.empty() || parts.size() > 3) return std::nullopt;
    static const std::regex kPart("^[0-9]+(\\.[0-9]+)?$");
    double seconds = 0;
    for (const auto& p : parts) {
        if (!std::regex_match(p, kPart)) return std::nullopt;
        double n = 0;
        const auto r = std::from_chars(p.data(), p.data() + p.size(), n);
        if (r.ec != std::errc()) return std::nullopt;
        seconds = seconds * 60 + n;
    }
    const double ms = std::floor(seconds * 1000 + 0.5);
    if (!std::isfinite(ms) || ms < 0 || ms > static_cast<double>(st::kMaxCaptionEndMs)) return std::nullopt;
    return static_cast<std::int64_t>(ms);
}

std::vector<st::Caption> parseSrt(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (const char c : text) {
        if (c != '\r') clean += c;
    }
    static const std::regex kBlocks("\n{2,}");
    static const std::regex kTags("<[^>]*>");
    static const std::regex kBraces("\\{[^}]*\\}");
    nlohmann::json cues = nlohmann::json::array();
    for (std::sregex_token_iterator it(clean.begin(), clean.end(), kBlocks, -1), end; it != end; ++it) {
        std::vector<std::string> lines;
        for (auto& line : split(it->str(), "\n")) {
            if (!jsTrim(line).empty()) lines.push_back(std::move(line));
        }
        const auto timing = std::find_if(lines.begin(), lines.end(), [](const std::string& l) { return l.find("-->") != std::string::npos; });
        if (timing == lines.end()) continue;
        const auto ends = split(*timing, "-->");
        const auto start = parseClock(ends[0]);
        // The end is the first word after the arrow ("00:00:03,500 X1:40" and the like).
        const std::u32string after = jsTrim(util::decodeUtf8(ends.size() > 1 ? ends[1] : std::string()));
        const auto space = std::find_if(after.begin(), after.end(), isJsSpace);
        const auto endTime = parseClock(util::encodeUtf8(std::u32string(after.begin(), space)));
        if (!start || !endTime) continue;
        // Markup in SRT (<i>, {\an8}) is dropped: the viewer shows plain text only.
        std::string words;
        for (auto l = timing + 1; l != lines.end(); ++l) {
            if (l != timing + 1) words += ' ';
            words += *l;
        }
        words = std::regex_replace(words, kTags, "");
        words = std::regex_replace(words, kBraces, "");
        cues.push_back({{"startMs", *start}, {"endMs", *endTime}, {"text", words}});
    }
    return st::parseCaptions(cues.dump());
}

std::string toSrt(const std::vector<st::Caption>& captions) {
    const auto stamp = [](std::int64_t ms) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", static_cast<long long>(ms / 3600000), static_cast<long long>((ms % 3600000) / 60000),
                      static_cast<long long>((ms % 60000) / 1000), static_cast<long long>(ms % 1000));
        return std::string(buf);
    };
    std::string out;
    for (std::size_t i = 0; i < captions.size(); ++i) {
        if (i > 0) out += '\n';
        const auto& c = captions[i];
        out += std::to_string(i + 1) + "\n" + stamp(c.startMs) + " --> " + stamp(c.endMs) + "\n" + c.text + "\n";
    }
    return out;
}

std::vector<st::Caption> evenCaptions(const std::vector<std::string>& lines, std::int64_t durationMs) {
    std::vector<std::u32string> clean;
    for (const auto& l : lines) {
        auto t = jsTrim(util::decodeUtf8(l));
        if (!t.empty()) clean.push_back(std::move(t));
        if (clean.size() >= st::kMaxCaptions) break;
    }
    if (clean.empty() || durationMs <= 0) return {};
    const std::int64_t slot = durationMs / static_cast<std::int64_t>(clean.size());
    // The last line runs to the very end, so rounding never leaves it short.
    nlohmann::json cues = nlohmann::json::array();
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const auto k = static_cast<std::int64_t>(i);
        cues.push_back({{"startMs", k * slot},
                        {"endMs", i + 1 == clean.size() ? durationMs : (k + 1) * slot},
                        {"text", util::encodeUtf8(util::takeUtf16(clean[i], st::kMaxCaptionText))}});
    }
    return st::parseCaptions(cues.dump());
}

}  // namespace redvault::studio::captions
