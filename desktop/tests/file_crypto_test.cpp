// Pins the desktop file encryption to the web client's format
// (ui/src/lib/fileCrypto.ts), using the same vector as Android's FileCryptoTest.

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>

#include "redvault/crypto/file_crypto.hpp"
#include "test_helpers.hpp"

using namespace redvault::crypto;
using redvault::test::b64;

namespace {

std::string readAsset(const std::string& name) {
    std::ifstream in(std::string(REDVAULT_TEST_VECTORS_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Pulls one flat field out of the vector's JSON; enough for this fixed file without a JSON dependency.
std::string jsonField(const std::string& json, const std::string& name) {
    std::smatch match;
    const std::regex pattern("\"" + name + "\"\\s*:\\s*\"?([^\",}]*)");
    REQUIRE(std::regex_search(json, match, pattern));
    return match[1].str();
}

std::string pattern(std::size_t size) {
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>((i * 31 + 7) & 0xff);
    return out;
}

std::string decrypt(const std::string& ciphertext, ByteView key, ByteView header, std::uint64_t encryptedSize,
                    const std::string& sha) {
    std::istringstream in(ciphertext);
    std::ostringstream out;
    files::decrypt(in, out, key, header, encryptedSize, sha);
    return out.str();
}

std::string decrypt(const std::string& ciphertext, const files::EncryptedFile& e) {
    return decrypt(ciphertext, e.key, e.header, e.encryptedSize, e.ciphertextSha256);
}

}  // namespace

TEST_CASE("decrypts a file encrypted by the web client", "[files][compat]") {
    const std::string meta = readAsset("web_file_vector.json");
    const std::string ciphertext = readAsset("web_file_vector.bin");
    const std::string plain = decrypt(ciphertext, b64(jsonField(meta, "key")), b64(jsonField(meta, "header")),
                                      std::stoull(jsonField(meta, "encryptedSize")),
                                      jsonField(meta, "ciphertextSha256"));
    CHECK(plain == pattern(std::stoul(jsonField(meta, "plaintextSize"))));
}

TEST_CASE("round-trips across chunk boundaries", "[files]") {
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, files::kChunkBytes - 1, files::kChunkBytes,
                             files::kChunkBytes + 1, std::size_t{200'000}}) {
        INFO("size " << size);
        const std::string plain = pattern(size);
        std::istringstream in(plain);
        std::ostringstream ciphertext;
        std::uint64_t lastProgress = 0;
        files::EncryptedFile e = files::encrypt(in, ciphertext, [&](std::uint64_t n) { lastProgress = n; });
        CHECK(e.plaintextSize == size);
        CHECK(lastProgress == size);
        CHECK(e.encryptedSize == files::encryptedSizeFor(size));
        CHECK(ciphertext.str().size() == e.encryptedSize);
        CHECK(decrypt(ciphertext.str(), e) == plain);
    }
}

TEST_CASE("rejects tampering, truncation and extra data", "[files]") {
    std::istringstream in(pattern(150'000));
    std::ostringstream ciphertext;
    files::EncryptedFile e = files::encrypt(in, ciphertext);
    const std::string good = ciphertext.str();

    std::string flipped = good;
    flipped[70'000] = static_cast<char>(flipped[70'000] ^ 1);
    CHECK_THROWS_AS(decrypt(flipped, e), files::FileDecryptError);

    const std::string truncated = good.substr(0, files::kChunkBytes + files::kChunkOverheadBytes);
    CHECK_THROWS_AS(decrypt(truncated, e.key, e.header, truncated.size(), std::string(64, '0')),
                    files::FileDecryptError);

    CHECK_THROWS_AS(decrypt(good + std::string(10, '\0'), e), files::FileDecryptError);

    CHECK_THROWS_AS(decrypt(good, e.key, e.header, e.encryptedSize, std::string(64, 'f')), files::FileDecryptError);
}
