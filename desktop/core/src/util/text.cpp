#include "redvault/util/text.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>

#include "redvault/crypto/bytes.hpp"

namespace redvault::util {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool isKotlinWhitespace(char32_t c) {
    // Kotlin's Char.isWhitespace(): Java whitespace plus Unicode space separators.
    return isAsciiWhitespace(c) || (c >= 0x1C && c <= 0x1F) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::size_t utf16Units(char32_t c) {
    return c > 0xFFFF ? 2 : 1;
}

template <typename T>
std::optional<T> number(std::string_view text) {
    T value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

}  // namespace

std::u32string decodeUtf8(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        char32_t c = 0;
        if (lead < 0x80) {
            length = 1;
            c = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            c = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            c = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            c = lead & 0x07;
        } else {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        if (i + length > text.size()) {
            out.push_back(kReplacement);
            break;
        }
        bool valid = true;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            c = (c << 6) | (next & 0x3F);
        }
        // Reject overlong forms, surrogates and anything past U+10FFFF.
        if (valid && ((length == 3 && c < 0x800) || (length == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
                      (c >= 0xD800 && c <= 0xDFFF))) {
            valid = false;
        }
        if (!valid) {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        out.push_back(c);
        i += length;
    }
    return out;
}

std::string encodeUtf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char32_t c : text) {
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = kReplacement;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::size_t utf16Length(std::u32string_view text) {
    std::size_t units = 0;
    for (char32_t c : text) units += utf16Units(c);
    return units;
}

bool isAsciiWhitespace(char32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r';
}

std::u32string trim(std::u32string_view text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && isKotlinWhitespace(text[start])) ++start;
    while (end > start && isKotlinWhitespace(text[end - 1])) --end;
    return std::u32string(text.substr(start, end - start));
}

std::u32string collapseWhitespace(std::u32string_view text) {
    std::u32string out;
    out.reserve(text.size());
    bool inRun = false;
    for (char32_t c : text) {
        if (isAsciiWhitespace(c)) {
            if (!inRun) out.push_back(' ');
            inRun = true;
        } else {
            out.push_back(c);
            inRun = false;
        }
    }
    return out;
}

std::u32string takeUtf16(std::u32string_view text, std::size_t maxUnits) {
    std::size_t units = 0;
    std::size_t count = 0;
    for (char32_t c : text) {
        if (units + utf16Units(c) > maxUnits) break;
        units += utf16Units(c);
        ++count;
    }
    return std::u32string(text.substr(0, count));
}

std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::optional<std::int64_t> parseIsoMillis(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SS[.fff…](Z|±HH:MM)
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't') ||
        text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto year = number<int>(text.substr(0, 4));
    const auto month = number<unsigned>(text.substr(5, 2));
    const auto day = number<unsigned>(text.substr(8, 2));
    const auto hour = number<int>(text.substr(11, 2));
    const auto minute = number<int>(text.substr(14, 2));
    const auto second = number<int>(text.substr(17, 2));
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }

    std::size_t pos = 19;
    std::int64_t millis = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 3) millis = millis * 10 + (text[pos] - '0');
            ++digits;
            ++pos;
        }
        if (digits == 0) return std::nullopt;
        for (int d = digits; d < 3; ++d) millis *= 10;
    }

    std::int64_t offsetMinutes = 0;
    const std::string_view zone = text.substr(pos);
    if (zone == "Z" || zone == "z") {
        // UTC
    } else if (zone.size() == 6 && (zone[0] == '+' || zone[0] == '-') && zone[3] == ':') {
        const auto offHour = number<int>(zone.substr(1, 2));
        const auto offMinute = number<int>(zone.substr(4, 2));
        if (!offHour || !offMinute) return std::nullopt;
        offsetMinutes = (*offHour * 60 + *offMinute) * (zone[0] == '-' ? -1 : 1);
    } else {
        return std::nullopt;
    }

    using namespace std::chrono;
    const year_month_day date{std::chrono::year{*year}, std::chrono::month{*month}, std::chrono::day{*day}};
    if (!date.ok()) return std::nullopt;
    const auto seconds = sys_days{date}.time_since_epoch() / std::chrono::seconds{1} + *hour * 3600LL +
                         *minute * 60LL + *second - offsetMinutes * 60;
    return seconds * 1000 + millis;
}

std::string formatIsoMillis(std::int64_t millis) {
    using namespace std::chrono;
    const sys_time<milliseconds> when{milliseconds{millis}};
    const auto day = floor<days>(when);
    const year_month_day date{day};
    const hh_mm_ss<milliseconds> time{when - day};
    char out[32];
    std::snprintf(out, sizeof out, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", int(date.year()), unsigned(date.month()),
                  unsigned(date.day()), int(time.hours().count()), int(time.minutes().count()),
                  int(time.seconds().count()), int(time.subseconds().count()));
    return out;
}

std::int64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string randomUuid() {
    auto bytes = crypto::randomBytes(16);
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    const std::string hex = crypto::toHex(bytes);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
           hex.substr(20);
}

}  // namespace redvault::util
