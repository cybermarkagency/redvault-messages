#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Small UTF-8 and time helpers for porting the Kotlin/TypeScript message
// formats, which measure strings in UTF-16 code units and use ISO-8601 UTC
// timestamps. Invalid UTF-8 decodes to U+FFFD rather than throwing: every
// string here may come from another participant.

namespace redvault::util {

std::u32string decodeUtf8(std::string_view text);
std::string encodeUtf8(std::u32string_view text);

/// Length as JavaScript/Kotlin count it (UTF-16 code units).
std::size_t utf16Length(std::u32string_view text);

/// Java's \s: space, \t, \n, \x0B, \f, \r.
bool isAsciiWhitespace(char32_t c);

/// Kotlin's String.trim(): strips leading/trailing characters that are whitespace.
std::u32string trim(std::u32string_view text);

/// Collapses every run of Java \s characters into one space.
std::u32string collapseWhitespace(std::u32string_view text);

/**
 * The longest prefix that fits in `maxUnits` UTF-16 code units, never
 * splitting a surrogate pair (so it can come out one unit shorter than
 * Kotlin's substring would, which could split one).
 */
std::u32string takeUtf16(std::u32string_view text, std::size_t maxUnits);

std::string toLowerAscii(std::string_view text);

/// Parses "2026-09-24T09:24:19.676Z" (optionally without fraction, or with a ±hh:mm offset) to Unix milliseconds.
std::optional<std::int64_t> parseIsoMillis(std::string_view text);

/// Unix milliseconds as "2026-09-24T09:24:19.676Z" (UTC), the format JavaScript's toISOString() produces.
std::string formatIsoMillis(std::int64_t millis);

/// Milliseconds since the Unix epoch, now.
std::int64_t nowMillis();

/// A random (version 4) UUID, lowercase.
std::string randomUuid();

}  // namespace redvault::util
