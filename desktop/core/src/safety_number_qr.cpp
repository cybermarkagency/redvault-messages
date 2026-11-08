// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/safety_number_qr.hpp"

#include <algorithm>
#include <cctype>

namespace redvault::crypto::safety_qr {
namespace {

constexpr std::string_view kPrefix = "redvault-safety:v1:";
constexpr std::size_t kDigits = 60;

std::string joined(const std::vector<std::string>& groups) {
    std::string out;
    for (const auto& group : groups) out += group;
    return out;
}

std::string_view trim(std::string_view text) {
    // Same as Kotlin's trim(): drops leading/trailing whitespace and control characters.
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') text.remove_prefix(1);
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') text.remove_suffix(1);
    return text;
}

}  // namespace

std::string encode(const std::vector<std::string>& safetyNumber) {
    return std::string(kPrefix) + joined(safetyNumber);
}

Result compare(std::string_view scanned, const std::vector<std::string>& mine) {
    std::string_view trimmed = trim(scanned);
    if (!trimmed.starts_with(kPrefix)) return Result::NotASafetyCode;
    std::string_view digits = trimmed.substr(kPrefix.size());
    if (digits.size() != kDigits ||
        !std::ranges::all_of(digits, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        return Result::NotASafetyCode;
    }
    return digits == joined(mine) ? Result::Match : Result::Mismatch;
}

}  // namespace redvault::crypto::safety_qr
