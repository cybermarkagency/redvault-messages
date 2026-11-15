// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Voice stories, edited on this computer: trim, fade, even out, change the voice, draw a picture of the loudness and
// write a WAV. A port of the web app's ui/src/lib/studio/audioDsp.ts by way of Android's studio/AudioDsp.kt, sample for
// sample (the maths runs in doubles and is stored as 32-bit floats, as it is in JavaScript), pinned to what the web's
// code makes by tests/vectors/studio_vectors.json. Nothing here touches the network.

namespace redvault::studio {

/// Mono sound: samples from -1 to 1 and the rate they were made at.
struct Mono {
    std::vector<float> samples;
    double sampleRate = 0;
};

/// The voice effects of a voice story, in the order every app offers them.
enum class VoiceFilter { None, Chipmunk, Deep, Robot, Echo, Radio };

/// "none", "chipmunk", "deep", "robot", "echo", "radio".
std::string_view filterWire(VoiceFilter filter);
/// "Normal", "Chipmunk", "Deep", "Robot", "Echo", "Radio".
std::string_view filterLabel(VoiceFilter filter);
std::optional<VoiceFilter> filterFromWire(std::string_view wire);
inline constexpr VoiceFilter kVoiceFilters[] = {VoiceFilter::None, VoiceFilter::Chipmunk, VoiceFilter::Deep,
                                                VoiceFilter::Robot, VoiceFilter::Echo, VoiceFilter::Radio};

/// What a finished voice is: a WAV file, how long it is, and the bars that draw its loudness.
struct FinishedVoice {
    std::string wav;
    std::int64_t durationMs = 0;
    std::vector<int> waveform;
};

namespace audio {

/// What a story keeps: voices don't need CD quality, and the file stays small.
inline constexpr double kStoryAudioRate = 22050.0;

std::int64_t durationMs(const Mono& m);
Mono mixToMono(const std::vector<std::vector<float>>& channels, double sampleRate);
/// Linear resampling (good enough for speech).
Mono resample(const Mono& m, double toRate);
/// Keeps `startMs` to `endMs` (clamped to the recording).
Mono trim(const Mono& m, double startMs, double endMs);
/// Fades the start in and the end out so a cut doesn't click.
Mono fade(const Mono& m, double inMs, double outMs);
double peak(const std::vector<float>& samples);
/// Scales the recording so its loudest point is `target` (never boosts silence into noise).
Mono normalize(const Mono& m, double target = 0.95);
Mono gain(const Mono& m, double factor);
/// Applies a voice effect. The result may be a different length (chipmunk, deep, echo).
Mono applyVoiceFilter(const Mono& m, VoiceFilter filter);
/// `bars` loudness values, 0 to 100, from the recording's volume (RMS per slice, scaled to the loudest slice).
std::vector<int> waveformBars(const std::vector<float>& samples, int bars = 48);
/// 16-bit mono PCM in a WAV file.
std::string encodeWav(const Mono& m);
/// Reads a WAV this module wrote (16-bit mono PCM); nullopt for anything else.
std::optional<Mono> decodeWav(std::string_view bytes);
/// The whole chain for a story: trim, effect, even out, fade the cut, bring the rate down, write a WAV.
FinishedVoice finishVoice(const Mono& m, double startMs, double endMs, VoiceFilter filter);

}  // namespace audio
}  // namespace redvault::studio
