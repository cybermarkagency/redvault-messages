#pragma once

#include <string>
#include <string_view>
#include <vector>

// The QR code two people scan to verify each other in person. It carries
// the 60-digit safety number, which is a hash of both public keys and is
// the same on both devices regardless of order. So if the number scanned
// from their screen equals the one this device computed, both are using the
// same two keys and nobody in between has swapped one.
//
// It holds nothing secret: the same digits are shown on screen for people
// comparing by eye. Port of android/.../crypto/SafetyNumberQr.kt.

namespace redvault::crypto::safety_qr {

enum class Result { Match, Mismatch, NotASafetyCode };

std::string encode(const std::vector<std::string>& safetyNumber);

Result compare(std::string_view scanned, const std::vector<std::string>& mine);

}  // namespace redvault::crypto::safety_qr
