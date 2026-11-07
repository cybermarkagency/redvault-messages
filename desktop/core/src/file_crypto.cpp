#include "redvault/crypto/file_crypto.hpp"

#include <sodium.h>

#include <algorithm>
#include <cctype>

namespace redvault::crypto::files {
namespace {

static_assert(kChunkOverheadBytes == crypto_secretstream_xchacha20poly1305_ABYTES);

/// Reads until `buffer` is full or the stream ends; returns the count read.
std::size_t readFully(std::istream& input, std::uint8_t* buffer, std::size_t size) {
    std::size_t total = 0;
    while (total < size && input) {
        input.read(reinterpret_cast<char*>(buffer + total), static_cast<std::streamsize>(size - total));
        total += static_cast<std::size_t>(input.gcount());
    }
    return total;
}

void writeAll(std::ostream& output, const std::uint8_t* data, std::size_t size) {
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output) throw CryptoError("Failed to write file data");
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Zeroes the secretstream state (it holds key material) on scope exit.
struct StreamState {
    crypto_secretstream_xchacha20poly1305_state value{};
    ~StreamState() { sodium_memzero(&value, sizeof value); }
};

}  // namespace

std::uint64_t encryptedSizeFor(std::uint64_t plaintextSize) {
    const std::uint64_t chunks = std::max<std::uint64_t>(1, (plaintextSize + kChunkBytes - 1) / kChunkBytes);
    return plaintextSize + chunks * kChunkOverheadBytes;
}

EncryptedFile encrypt(std::istream& input, std::ostream& output, const ProgressFn& onProgress) {
    ensureSodium();
    EncryptedFile result;
    result.key = SecretBytes(crypto_secretstream_xchacha20poly1305_KEYBYTES);
    result.header = Bytes(crypto_secretstream_xchacha20poly1305_HEADERBYTES);
    crypto_secretstream_xchacha20poly1305_keygen(result.key.data());
    StreamState state;
    if (crypto_secretstream_xchacha20poly1305_init_push(&state.value, result.header.data(), result.key.data()) != 0) {
        throw CryptoError("secretstream init failed");
    }

    crypto_hash_sha256_state digest;
    crypto_hash_sha256_init(&digest);
    SecretBytes plain(kChunkBytes);
    SecretBytes next(kChunkBytes);
    Bytes cipher(kChunkBytes + kChunkOverheadBytes);

    // One chunk of look-ahead: a chunk can only be tagged FINAL once we know
    // nothing follows it. An empty file is one empty FINAL chunk.
    std::size_t current = readFully(input, plain.data(), kChunkBytes);
    while (true) {
        const std::size_t nextLen = current == kChunkBytes ? readFully(input, next.data(), kChunkBytes) : 0;
        const bool isLast = nextLen == 0;
        const unsigned char tag =
            isLast ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE;
        unsigned long long written = 0;
        if (crypto_secretstream_xchacha20poly1305_push(&state.value, cipher.data(), &written, plain.data(), current,
                                                       nullptr, 0, tag) != 0) {
            throw CryptoError("secretstream push failed");
        }
        crypto_hash_sha256_update(&digest, cipher.data(), written);
        writeAll(output, cipher.data(), static_cast<std::size_t>(written));
        result.plaintextSize += current;
        result.encryptedSize += written;
        if (onProgress) onProgress(result.plaintextSize);
        if (isLast) break;
        std::copy_n(next.data(), nextLen, plain.data());
        current = nextLen;
    }

    Bytes hash(crypto_hash_sha256_BYTES);
    crypto_hash_sha256_final(&digest, hash.data());
    result.ciphertextSha256 = toHex(hash);
    return result;
}

void decrypt(std::istream& input, std::ostream& output, ByteView key, ByteView header, std::uint64_t encryptedSize,
             std::string_view ciphertextSha256, const ProgressFn& onProgress) {
    ensureSodium();
    if (key.size() != crypto_secretstream_xchacha20poly1305_KEYBYTES ||
        header.size() != crypto_secretstream_xchacha20poly1305_HEADERBYTES) {
        throw FileDecryptError();
    }
    StreamState state;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state.value, header.data(), key.data()) != 0) {
        throw FileDecryptError();
    }

    crypto_hash_sha256_state digest;
    crypto_hash_sha256_init(&digest);
    Bytes cipher(kChunkBytes + kChunkOverheadBytes);
    SecretBytes plain(kChunkBytes);
    std::uint64_t received = 0;
    bool finished = false;

    while (true) {
        const std::size_t len = readFully(input, cipher.data(), cipher.size());
        if (len == 0) break;
        if (finished) throw FileDecryptError();  // data after TAG_FINAL
        received += len;
        if (received > encryptedSize) throw FileDecryptError();
        crypto_hash_sha256_update(&digest, cipher.data(), len);
        unsigned long long plainLen = 0;
        unsigned char tag = 0;
        if (crypto_secretstream_xchacha20poly1305_pull(&state.value, plain.data(), &plainLen, &tag, cipher.data(), len,
                                                       nullptr, 0) != 0) {
            throw FileDecryptError();
        }
        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            finished = true;
        } else if (tag != crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) {
            throw FileDecryptError();
        }
        writeAll(output, plain.data(), static_cast<std::size_t>(plainLen));
        if (onProgress) onProgress(received);
    }

    Bytes hash(crypto_hash_sha256_BYTES);
    crypto_hash_sha256_final(&digest, hash.data());
    if (!finished || received != encryptedSize || toHex(hash) != lowercase(ciphertextSha256)) {
        throw FileDecryptError();
    }
}

}  // namespace redvault::crypto::files
