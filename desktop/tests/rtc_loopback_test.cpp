// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "redvault/crypto/call_crypto.hpp"
#include "redvault/rtc/call_media.hpp"

using namespace redvault::rtc;
using namespace std::chrono_literals;

namespace {

std::vector<std::int16_t> tone(int frame, double hz) {
    std::vector<std::int16_t> pcm(kFrameSamples);
    for (int i = 0; i < kFrameSamples; ++i) {
        const double t = (frame * kFrameSamples + i) / static_cast<double>(kSampleRate);
        pcm[i] = static_cast<std::int16_t>(8000 * std::sin(2 * std::numbers::pi * hz * t));
    }
    return pcm;
}

VideoFrame pattern(int n) {
    VideoFrame f = VideoFrame::blank(320, 240);
    for (int yy = 0; yy < 240; ++yy)
        for (int xx = 0; xx < 320; ++xx) f.y()[yy * 320 + xx] = static_cast<std::uint8_t>((xx + yy + n * 4) & 0xFF);
    return f;
}

template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("two engines connect and exchange Opus audio and VP8 video", "[rtc]") {
    std::atomic<bool> aConnected{false}, bConnected{false};
    std::atomic<int> bFrames{0}, aFrames{0};
    std::atomic<int> bWidth{0};

    Config local;
    local.relayOnly = false;  // one machine, no TURN
    local.gatherTimeoutMs = 5000;

    CallMedia a(local, Callbacks{[&](ConnectionState s) { aConnected = s == ConnectionState::Connected; },
                                 [&](const VideoFrame&) { ++aFrames; }});
    CallMedia b(local, Callbacks{[&](ConnectionState s) { bConnected = s == ConnectionState::Connected; },
                                 [&](const VideoFrame& f) {
                                     bWidth = f.width;
                                     ++bFrames;
                                 }});

    const std::string offer = a.createOffer();
    CHECK(offer.find("opus/48000") != std::string::npos);
    CHECK(offer.find("VP8/90000") != std::string::npos);
    const std::string answer = b.acceptOffer(offer);
    a.setAnswer(answer);
    REQUIRE(waitUntil([&] { return aConnected && bConnected; }, 10s));

    // Both sides derive the same safety words from the two fingerprints.
    using redvault::crypto::calls::sasWords;
    using redvault::crypto::calls::sdpFingerprint;
    REQUIRE_FALSE(sdpFingerprint(a.localSdp()).empty());
    CHECK(sasWords(sdpFingerprint(a.localSdp()), sdpFingerprint(a.remoteSdp())) ==
          sasWords(sdpFingerprint(b.localSdp()), sdpFingerprint(b.remoteSdp())));

    for (int i = 0; i < 60; ++i) {
        a.sendAudio(tone(i, 440).data());
        b.sendAudio(tone(i, 660).data());
        if (i % 3 == 0) {
            a.sendVideo(pattern(i));
            b.sendVideo(pattern(i));
        }
        std::this_thread::sleep_for(20ms);
    }

    REQUIRE(waitUntil([&] { return b.audioPacketsReceived() > 40 && bFrames > 5 && aFrames > 5; }, 5s));
    CHECK(bWidth == 320);

    // The received audio decodes to the tone, not silence.
    std::vector<std::int16_t> heard(kFrameSamples * 10);
    b.readAudio(heard.data(), heard.size());
    int loud = 0;
    for (const auto s : heard) loud += std::abs(s) > 1000 ? 1 : 0;
    CHECK(loud > static_cast<int>(heard.size() / 4));

    a.close();
    b.close();
}

TEST_CASE("relay-only calls refuse to start without a relay", "[rtc]") {
    Config noRelay;
    noRelay.relayOnly = true;
    noRelay.gatherTimeoutMs = 1500;
    CallMedia a(noRelay, Callbacks{});
    CHECK_THROWS_AS(a.createOffer(), RtcError);
}
