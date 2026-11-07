#include <catch2/catch_test_macros.hpp>

#include "redvault/crypto/safety_number_qr.hpp"

using namespace redvault::crypto;

namespace {

const std::vector<std::string> kMine = {"79320", "21968", "96733", "34230", "77447", "92420",
                                        "43079", "66269", "30648", "82455", "53512", "64617"};

}  // namespace

TEST_CASE("safety QR round-trips and classifies scans", "[safety]") {
    const std::string code = safety_qr::encode(kMine);
    CHECK(code == "redvault-safety:v1:793202196896733342307744792420430796626930648824555351264617");
    CHECK(safety_qr::compare(code, kMine) == safety_qr::Result::Match);
    CHECK(safety_qr::compare("  " + code + "\n", kMine) == safety_qr::Result::Match);

    std::string different = code;
    different.back() = different.back() == '0' ? '1' : '0';
    CHECK(safety_qr::compare(different, kMine) == safety_qr::Result::Mismatch);

    CHECK(safety_qr::compare("https://example.com", kMine) == safety_qr::Result::NotASafetyCode);
    CHECK(safety_qr::compare("redvault-safety:v1:1234", kMine) == safety_qr::Result::NotASafetyCode);
    CHECK(safety_qr::compare(code.substr(0, code.size() - 1) + "x", kMine) == safety_qr::Result::NotASafetyCode);
}
