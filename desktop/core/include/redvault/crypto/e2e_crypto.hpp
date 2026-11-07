#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/crypto/bytes.hpp"

// Desktop port of android/.../crypto/E2eCrypto.kt (itself a port of
// ui/src/lib/e2eCrypto.ts; client/src/crypto.ts documents the design in
// full). Same libsodium primitives, same parameters, same byte layouts, so
// desktop, web and Android clients can talk to each other:
//
//   - X25519 keypair per user
//   - private key wrapped with an Argon2id-derived key (from the password)
//   - random symmetric conversation key, sealed once per participant with
//     crypto_box_seal (anonymous ECIES over X25519)
//   - message bodies encrypted with the conversation key via
//     XChaCha20-Poly1305 (AEAD)
//
// Compatibility is pinned by tests/e2e_crypto_compat_test.cpp, which uses
// the same web-produced vectors as the Android E2eCryptoCompatTest.
//
// Nothing here ever sends a private key or plaintext message anywhere.

namespace redvault::crypto {

inline constexpr std::size_t kBoxPublicKeyBytes = 32;
inline constexpr std::size_t kBoxSecretKeyBytes = 32;
inline constexpr std::size_t kBoxSealBytes = 48;  // ephemeral public key (32) + MAC (16)
inline constexpr std::size_t kAeadKeyBytes = 32;
inline constexpr std::size_t kAeadNonceBytes = 24;
inline constexpr std::size_t kAeadTagBytes = 16;
inline constexpr std::size_t kPwhashSaltBytes = 16;

// libsodium's crypto_pwhash_argon2id_{OPS,MEM}LIMIT_INTERACTIVE. Every account
// from before settings were stored used these, so they must never change.
inline constexpr unsigned long long kPwhashOpsLimitInteractive = 2;
inline constexpr std::size_t kPwhashMemLimitInteractive = 67'108'864;

/// Argon2id's cost: passes, and memory in bytes. Signed, because the numbers can come from a server.
struct KdfSettings {
    std::int64_t ops = 0;
    std::int64_t mem = 0;
    bool operator==(const KdfSettings&) const = default;
};

/// What every account had before settings were stored, and the lowest anything here accepts.
inline constexpr KdfSettings kKdfInteractive{static_cast<std::int64_t>(kPwhashOpsLimitInteractive),
                                             static_cast<std::int64_t>(kPwhashMemLimitInteractive)};
/**
 * Used for every new wrap. Raising it takes a benchmark on real low-end devices first: Argon2 is memory-hard, and a
 * setting a device can't finish locks that person out of their account (SECURITY_DESIGN_AND_MIGRATION.md, M-1).
 */
inline constexpr KdfSettings kKdfCurrent = kKdfInteractive;
inline constexpr KdfSettings kKdfMax{10, 1'073'741'824};
inline constexpr const char* kSignInDetailsWrong = "This account's sign-in details look wrong. Your password wasn't sent.";

/// The server asked for key-stretching settings this app won't use. Nothing was derived.
class UnsafeKdfError : public CryptoError {
public:
    UnsafeKdfError() : CryptoError(kSignInDetailsWrong) {}
};

/// Settings come from the server. Too low would make a captured sign-in key cheap to attack, too high would hang the app.
void requireSafeKdf(const KdfSettings& kdf);

struct KeyPair {
    Bytes publicKey;
    SecretBytes privateKey;
};

struct WrappedPrivateKey {
    Bytes encryptedPrivateKey;
    Bytes nonce;
    Bytes kdfSalt;
    /// Empty on data from before settings were stored: those all used kKdfInteractive.
    std::optional<KdfSettings> kdf;
};

struct AeadCiphertext {
    Bytes ciphertext;
    Bytes nonce;
};

/// The public key that goes with a private key.
Bytes publicKeyOf(ByteView privateKey);

/// Generates a fresh X25519 keypair for E2E messaging.
KeyPair generateKeyPair();

/**
 * Derives a symmetric key from a password + salt via Argon2id. Deliberately
 * slow (64 MiB at the least, a noticeable fraction of a second): call it off
 * the UI thread. `kdf` may come from the server, so it's checked first and
 * nothing is derived from settings out of range (UnsafeKdfError).
 */
SecretBytes deriveWrapKey(std::string_view password, ByteView kdfSalt, const KdfSettings& kdf = kKdfInteractive);

/// The value sent to the server instead of the password (docs/auth-key-split.md). One-way from the unlock key.
std::string deriveSignInKey(std::string_view password, ByteView kdfSalt, const KdfSettings& kdf = kKdfInteractive);

/// Wraps a private key with a password-derived key under a fresh random salt, at kKdfCurrent. Store every output.
WrappedPrivateKey wrapPrivateKey(ByteView privateKey, std::string_view password);

/// Unwraps a private key given the password and the stored salt, nonce and settings. Throws CryptoError on a wrong password or tampering.
SecretBytes unwrapPrivateKey(const WrappedPrivateKey& wrapped, std::string_view password);

/// A public fingerprint of a conversation key. One-way: it shows two people hold the same key without revealing it.
Bytes keyCommitment(ByteView conversationKey);

/**
 * The exact bytes a group key notice signs, identical on every app (data/key_trust.hpp): context, 0x00, conversation
 * id and issuer id (36 lowercase ASCII chars each), epoch, seq and member count (u32 BE), each member id, sorted,
 * 36 chars, then the commitment (32 bytes). Throws CryptoError unless every id is a UUID and both numbers fit.
 */
Bytes keyNoticeInput(std::string_view conversationId, std::string_view issuerId, std::int64_t epoch, std::int64_t seq,
                     const std::vector<std::string>& members, ByteView commitment);

/// Generates a fresh random symmetric key for a new conversation.
SecretBytes generateConversationKey();

/// Seals a conversation key to one participant's public key. Call once per participant.
Bytes sealConversationKey(ByteView conversationKey, ByteView recipientPublicKey);

/// Unseals a conversation key using the recipient's own keypair.
SecretBytes unsealConversationKey(ByteView sealed, ByteView recipientPublicKey, ByteView recipientPrivateKey);

/// Encrypts a message (or conversation name) body, as UTF-8, with the conversation key.
AeadCiphertext encryptWithConversationKey(std::string_view plaintext, ByteView conversationKey);

/// Decrypts a message (or conversation name) body with the conversation key.
std::string decryptWithConversationKey(ByteView ciphertext, ByteView nonce, ByteView conversationKey);

/**
 * Order-independent 60-digit fingerprint of both participants' real public
 * keys, as 12 groups of 5 digits. Must match the web client's
 * safetyNumberDigits exactly, since two people compare it across apps:
 * SHA-256 over "base64(lower key):base64(higher key)" (keys ordered by
 * unsigned byte comparison), first 30 bytes, each byte % 100 as 2 digits.
 */
std::vector<std::string> safetyNumberDigits(ByteView publicKeyA, ByteView publicKeyB);

// --- Message signatures (db/migrations/030, docs/group-keys-and-signatures.md) ---
//
// Everyone in a chat holds its key, so encryption alone can't tell members
// apart. Each account also has an Ed25519 signing key pair, derived from its
// X25519 private key with libsodium's KDF (so every one of its apps gets the
// same pair), and every message carries a detached signature over
// messageSignatureInput. Pinned to the web by tests/message_signature_test.cpp.

inline constexpr std::size_t kSignPublicKeyBytes = 32;
inline constexpr std::size_t kSignSecretKeyBytes = 64;
inline constexpr std::size_t kSignatureBytes = 64;
inline constexpr std::string_view kMessageSignatureContext = "redvault-message-signature-v1";

struct SigningKeyPair {
    Bytes publicKey;
    /// libsodium's 64-byte Ed25519 secret key (seed followed by the public key).
    SecretBytes secretKey;
};

/// The account's signing key pair, derived from its X25519 private key.
SigningKeyPair deriveSigningKeyPair(ByteView accountPrivateKey);

/**
 * The exact bytes a message signature covers, identical on every app and the
 * server (server/src/crypto/messageSignature.ts): context, 0x00, conversation
 * id and sender id (36 lowercase ASCII chars each), key epoch (u32 BE), nonce
 * length (u32 BE), nonce, ciphertext. Throws CryptoError unless both ids are UUIDs.
 */
Bytes messageSignatureInput(std::string_view conversationId, std::string_view senderId, std::uint32_t keyEpoch,
                            ByteView nonce, ByteView ciphertext);

Bytes signMessage(ByteView input, ByteView secretKey);

/// False for a bad signature, a wrong key, or malformed input.
bool verifyMessageSignature(ByteView signature, ByteView input, ByteView publicKey);

/// XChaCha20-Poly1305 with a fresh random nonce.
AeadCiphertext aeadEncrypt(ByteView plaintext, ByteView key);

/// XChaCha20-Poly1305 with a caller-chosen nonce. Only for tests and vectors: never reuse a nonce.
AeadCiphertext aeadEncrypt(ByteView plaintext, ByteView key, ByteView nonce);

/// Throws CryptoError on a wrong key, wrong nonce or tampered ciphertext.
SecretBytes aeadDecrypt(ByteView ciphertext, ByteView nonce, ByteView key);

}  // namespace redvault::crypto
