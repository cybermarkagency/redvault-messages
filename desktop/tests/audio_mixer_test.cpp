// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "redvault/groupcall/audio_mixer.hpp"

using redvault::lk::AudioMixer;

namespace {

std::vector<std::int16_t> constant(std::int16_t value, std::size_t count) {
    return std::vector<std::int16_t>(count, value);
}

}  // namespace

TEST_CASE("the mixer plays silence when nobody is talking", "[groupcall][mixer]") {
    AudioMixer mixer;
    std::vector<std::int16_t> out(960, 7);
    mixer.mix(out.data(), out.size());
    for (const auto s : out) CHECK(s == 0);
}

TEST_CASE("a lane waits for a little audio before it plays, then plays what it has", "[groupcall][mixer]") {
    AudioMixer mixer;
    const auto few = constant(1000, 480);  // 10 ms: under the prebuffer
    mixer.push("a", few.data(), few.size());
    std::vector<std::int16_t> out(480, 0);
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == 0);  // still waiting

    const auto more = constant(1000, AudioMixer::kPrebufferSamples);
    mixer.push("a", more.data(), more.size());
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == 1000);
    CHECK(out[479] == 1000);
}

TEST_CASE("everyone's audio is summed and clipped, not wrapped", "[groupcall][mixer]") {
    AudioMixer mixer;
    const auto a = constant(20000, AudioMixer::kPrebufferSamples * 2);
    const auto b = constant(-5000, AudioMixer::kPrebufferSamples * 2);
    const auto c = constant(20000, AudioMixer::kPrebufferSamples * 2);
    mixer.push("a", a.data(), a.size());
    mixer.push("b", b.data(), b.size());
    std::vector<std::int16_t> out(480, 0);
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == 15000);

    mixer.push("c", c.data(), c.size());
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == 32767);  // 20000 - 5000 + 20000 clips

    const auto d = constant(-30000, AudioMixer::kPrebufferSamples * 2);
    const auto e = constant(-30000, AudioMixer::kPrebufferSamples * 2);
    mixer.clear();
    mixer.push("d", d.data(), d.size());
    mixer.push("e", e.data(), e.size());
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == -32768);
}

TEST_CASE("a lane that runs dry stops and starts over once it has audio again", "[groupcall][mixer]") {
    AudioMixer mixer;
    const auto start = constant(500, AudioMixer::kPrebufferSamples);
    mixer.push("a", start.data(), start.size());
    std::vector<std::int16_t> out(AudioMixer::kPrebufferSamples + 480, 9);
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == 500);
    CHECK(out[AudioMixer::kPrebufferSamples - 1] == 500);
    CHECK(out[AudioMixer::kPrebufferSamples] == 0);  // ran out: silence, not a repeat

    // A trickle doesn't play until it adds up to the prebuffer again.
    const auto trickle = constant(500, 480);
    mixer.push("a", trickle.data(), trickle.size());
    std::vector<std::int16_t> next(480, 9);
    mixer.mix(next.data(), next.size());
    CHECK(next[0] == 0);
}

TEST_CASE("a lane that falls far behind drops its oldest audio instead of delaying the call", "[groupcall][mixer]") {
    AudioMixer mixer;
    // Older half 100, newer half 200, more than the mixer keeps.
    const auto old = constant(100, AudioMixer::kMaxBufferedSamples);
    const auto fresh = constant(200, AudioMixer::kMaxBufferedSamples);
    mixer.push("a", old.data(), old.size());
    mixer.push("a", fresh.data(), fresh.size());
    std::vector<std::int16_t> out(480, 0);
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == 200);
}

TEST_CASE("someone who leaves stops being mixed", "[groupcall][mixer]") {
    AudioMixer mixer;
    const auto a = constant(300, AudioMixer::kPrebufferSamples * 2);
    mixer.push("a", a.data(), a.size());
    CHECK(mixer.activeLanes() == 1);
    mixer.remove("a");
    CHECK(mixer.activeLanes() == 0);
    std::vector<std::int16_t> out(480, 5);
    mixer.mix(out.data(), out.size());
    CHECK(out[0] == 0);
}
