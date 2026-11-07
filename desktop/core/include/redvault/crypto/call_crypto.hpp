#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/crypto/bytes.hpp"

// End-to-end protection for calls, byte-compatible with the web app
// (ui/src/lib/callCrypto.ts, callLinkCrypto.ts, sas.ts, merkle.ts,
// transparencyLog.ts). See those files for the design; in short:
//
//   - A call's offer and answer carry each side's DTLS fingerprint, which
//     is what the encrypted media is pinned to. They're sealed with
//     crypto_box between the two identity keys (calls between contacts) or
//     with XChaCha20-Poly1305 under a call link's secret, so the server
//     that relays them can't read, alter or forge them.
//   - Four words from both fingerprints let two people check the live
//     connection aloud.
//   - The transparency log's checkpoints and proofs are verified here too.

namespace redvault::crypto::calls {

class CallSignalError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The sealed payload (JSON on the wire, as the web app writes it).
struct CallSignal {
    std::string type;            // "offer" or "answer"
    std::string callId;
    std::string conversationId;  // "link:<id>" for a call link
    std::string from;
    std::string to;
    std::string kind;            // "voice" or "video"
    std::string sdp;
    std::int64_t sentAt = 0;     // epoch ms
};

struct Sealed {
    std::string ciphertext;  // base64
    std::string nonce;       // base64
};

struct Expected {
    std::string type;
    std::string callId;
    std::string conversationId;
    std::string from;
    std::string to;
    std::optional<std::string> kind;
};

std::int64_t nowMs();

/// Between contacts: crypto_box from our private key to their public key.
Sealed seal(const CallSignal& signal, ByteView recipientPublicKey, ByteView senderPrivateKey);
/// Opens and checks a contact's signal; throws CallSignalError if it isn't exactly the one expected.
CallSignal open(const Sealed& sealed, ByteView senderPublicKey, ByteView recipientPrivateKey, const Expected& expected,
                std::int64_t now = nowMs());

// --- Call links ---

/// A new random 32-byte link secret.
SecretBytes newLinkSecret();
/// The secret as it goes in the URL fragment (base64url, no padding).
std::string linkSecretToFragment(ByteView secret);
/// Null if the fragment isn't a 32-byte secret.
std::optional<SecretBytes> linkSecretFromFragment(std::string_view fragment);
/// https://host/redvaultmessages/?join=<id>#<secret>, from the web app's address.
std::string linkUrl(std::string_view webAppBase, std::string_view linkId, ByteView secret);
Sealed sealLinkName(std::string_view name, ByteView secret);
std::string openLinkName(const Sealed& name, ByteView secret);
/// The owner's copy of the secret, sealed to their own public key.
std::string sealSecretForOwner(ByteView secret, ByteView ownerPublicKey);
SecretBytes openOwnerSecret(std::string_view sealedB64, ByteView ownerPublicKey, ByteView ownerPrivateKey);
Sealed sealWithLink(const CallSignal& signal, ByteView secret);
/// Checks it's for this call through link `linkId`; throws CallSignalError otherwise.
CallSignal openWithLink(const Sealed& sealed, ByteView secret, const Expected& expected, std::int64_t now = nowMs());

// --- Short authentication words ---

/// "sha-256 AB:CD:..." from an SDP's a=fingerprint line, or empty.
std::string sdpFingerprint(std::string_view sdp);
/// One of the 256 words the safety words are drawn from (also used by the password generator).
std::string_view wordListEntry(std::size_t index);
/// The same four words the other side sees (order of the fingerprints doesn't matter).
std::vector<std::string> sasWords(std::string_view fingerprintA, std::string_view fingerprintB, std::size_t count = 4);

// --- Keys derived from the identity key, for data kept only on this device ---

/// BLAKE2b(32, label, key = private key): e.g. "redvault-call-history-v1", "redvault-device-label-v1".
SecretBytes deriveLocalKey(ByteView privateKey, std::string_view label);

// --- Transparency log ---

namespace merkle {
Bytes leafHash(ByteView data);
bool verifyInclusion(ByteView leaf, std::uint64_t index, std::uint64_t size, const std::vector<Bytes>& proof, ByteView root);
bool verifyConsistency(std::uint64_t from, std::uint64_t to, ByteView fromRoot, ByteView toRoot, const std::vector<Bytes>& proof);
Bytes fromHex(std::string_view hex);
std::string toHex(ByteView bytes);
}  // namespace merkle

/// Exactly what the server signs for a checkpoint.
std::string checkpointMessage(std::int64_t treeSize, std::string_view rootHashHex, std::string_view signedAtIso);
bool checkpointSignatureValid(std::int64_t treeSize, std::string_view rootHashHex, std::string_view signedAtIso,
                              std::string_view signatureB64, std::string_view publicKeyB64);

}  // namespace redvault::crypto::calls
