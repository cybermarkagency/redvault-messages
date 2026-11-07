#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/crypto/bytes.hpp"

// The password manager's cryptography and password tools, all on the
// device (server/src/services/passwordVaultService.ts only stores
// ciphertext):
//
//   - Items are JSON, padded to a 256-byte bucket (so the stored size says
//     little about what's inside), then sealed with XChaCha20-Poly1305
//     under a key derived from the account's private key
//     ("redvault-password-vault-v1"). A shared copy is sealed to the
//     recipient's public key instead (crypto_box_seal).
//   - One-time codes: RFC 6238 TOTP (SHA-1/256/512), from a base32 secret
//     or an otpauth:// URI.
//   - Breach check: SHA-1 of the password, of which only the first 5 hex
//     characters ever leave the device (k-anonymity).
//   - Strength estimate and generator.

namespace redvault::crypto::vault {

// --- Hashes ---

/// SHA-1 (for TOTP and the breach check only: both are defined on it).
Bytes sha1(ByteView data);
Bytes hmacSha1(ByteView key, ByteView message);
Bytes hmacSha256(ByteView key, ByteView message);
Bytes hmacSha512(ByteView key, ByteView message);

// --- One-time codes ---

/// RFC 4648 base32 (case-insensitive, spaces and padding ignored); nullopt if invalid.
std::optional<Bytes> base32Decode(std::string_view text);

struct TotpParams {
    Bytes secret;
    std::string algorithm = "SHA1";  // "SHA1", "SHA256" or "SHA512"
    int digits = 6;
    int period = 30;
    std::string issuer;
    std::string account;
};

/// A base32 secret or an otpauth://totp/... URI; nullopt if it's neither.
std::optional<TotpParams> parseTotp(std::string_view secretOrUri);
/// The code at `unixSeconds`, zero-padded to `digits`.
std::string totpCode(const TotpParams& params, std::int64_t unixSeconds);
/// Seconds until the code changes.
int totpSecondsLeft(const TotpParams& params, std::int64_t unixSeconds);

// --- Breach check (k-anonymity) ---

/// Uppercase hex SHA-1 of the password: the first 5 characters are sent, the rest compared on the device.
std::string breachHash(std::string_view password);
/// How many times `hashHex` appears in a range response ("SUFFIX:COUNT" lines for prefix hashHex[0..5]). 0: not breached.
std::int64_t breachCount(std::string_view hashHex, std::string_view rangeResponse);

// --- Strength ---

enum class Strength { Weak, Fair, Strong };

struct StrengthEstimate {
    Strength strength = Strength::Weak;
    double bits = 0;
    /// "Weak", "Fair" or "Strong".
    std::string label;
};

/// A conservative guess-resistance estimate: character classes and length, minus repeats, sequences and common passwords.
StrengthEstimate estimateStrength(std::string_view password);

// --- Generator ---

struct GeneratorOptions {
    int length = 24;
    bool upper = true;  // A-Z (lowercase is always in)
    bool digits = true;
    bool symbols = true;
    /// Words instead of characters: `length` words (4-12) from a 256-word list, separated by "-", with a number.
    bool words = false;
};

std::string generatePassword(const GeneratorOptions& options);

// --- Items ---

/// The vault's key: BLAKE2b(32, "redvault-password-vault-v1", key = private key).
SecretBytes vaultKey(ByteView privateKey);

struct SealedItem {
    Bytes ciphertext;
    Bytes nonce;
};

/// Pads `json` with trailing spaces to a multiple of 256 bytes (JSON parsers ignore them), then seals it.
SealedItem sealItem(std::string_view json, ByteView key);
std::string openItem(ByteView ciphertext, ByteView nonce, ByteView key);

/// A copy for someone else: crypto_box_seal to their public key (padded the same way).
Bytes sealForRecipient(std::string_view json, ByteView recipientPublicKey);
std::string openFromSender(ByteView sealed, ByteView publicKey, ByteView privateKey);

}  // namespace redvault::crypto::vault
