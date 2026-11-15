// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/studio/audio_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace redvault::studio {

namespace {

constexpr double kPi = std::numbers::pi;

double clamp(double v, double lo, double hi) {
    return std::min(hi, std::max(lo, v));
}

/// Math.round: halves go up.
std::int64_t jsRound(double v) {
    return static_cast<std::int64_t>(std::floor(v + 0.5));
}

std::size_t clampIndex(std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return static_cast<std::size_t>(std::min(hi, std::max(lo, v)));
}

/// A pitch change by playing faster or slower, so the length changes with it (a toy effect, on purpose).
Mono repitch(const Mono& m, double factor) {
    return Mono{audio::resample(m, m.sampleRate / factor).samples, m.sampleRate};
}

Mono ringModulate(const Mono& m, double hz) {
    Mono out{std::vector<float>(m.samples.size()), m.sampleRate};
    for (std::size_t i = 0; i < m.samples.size(); ++i) {
        out.samples[i] = static_cast<float>(static_cast<double>(m.samples[i]) * std::sin((2 * kPi * hz * static_cast<double>(i)) / m.sampleRate));
    }
    return out;
}

Mono echo(const Mono& m, double delayMs, double feedback, double mix) {
    const auto delay = static_cast<std::size_t>(std::max<std::int64_t>(1, jsRound(delayMs / 1000 * m.sampleRate)));
    const std::size_t tail = delay * 6;
    std::vector<float> out(m.samples.size() + tail);
    std::vector<float> wet(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double dry = i < m.samples.size() ? static_cast<double>(m.samples[i]) : 0.0;
        const double echoed = i >= delay ? static_cast<double>(wet[i - delay]) * feedback : 0.0;
        wet[i] = static_cast<float>(dry + echoed);
        out[i] = static_cast<float>(dry * (1 - mix * 0.5) + (static_cast<double>(wet[i]) - dry) * mix);
    }
    return Mono{std::move(out), m.sampleRate};
}

/// A second-order filter: the state is kept in doubles, the result is stored as floats (as the web does).
std::vector<float> biquad(const std::vector<float>& samples, const double (&c)[5]) {
    std::vector<float> out(samples.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double x0 = samples[i];
        const double y0 = c[0] * x0 + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
        out[i] = static_cast<float>(y0);
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
    }
    return out;
}

void lowpass(double rate, double hz, double (&c)[5]) {
    const double w0 = (2 * kPi * hz) / rate;
    const double alpha = std::sin(w0) / (2 * 0.7071);
    const double cs = std::cos(w0);
    const double a0 = 1 + alpha;
    c[0] = (1 - cs) / 2 / a0;
    c[1] = (1 - cs) / a0;
    c[2] = (1 - cs) / 2 / a0;
    c[3] = (-2 * cs) / a0;
    c[4] = (1 - alpha) / a0;
}

void highpass(double rate, double hz, double (&c)[5]) {
    const double w0 = (2 * kPi * hz) / rate;
    const double alpha = std::sin(w0) / (2 * 0.7071);
    const double cs = std::cos(w0);
    const double a0 = 1 + alpha;
    c[0] = (1 + cs) / 2 / a0;
    c[1] = -(1 + cs) / a0;
    c[2] = (1 + cs) / 2 / a0;
    c[3] = (-2 * cs) / a0;
    c[4] = (1 - alpha) / a0;
}

Mono radio(const Mono& m) {
    double hp[5];
    double lp[5];
    highpass(m.sampleRate, 400.0, hp);
    lowpass(m.sampleRate, std::min(3200.0, m.sampleRate / 2.5), lp);
    std::vector<float> s = biquad(biquad(m.samples, hp), lp);
    // A little grit: soft clipping.
    for (auto& v : s) v = static_cast<float>(std::tanh(static_cast<double>(v) * 2.2) * 0.8);
    return Mono{std::move(s), m.sampleRate};
}

void put16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

std::uint32_t get32(std::string_view b, std::size_t o) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<std::uint8_t>(b[o + static_cast<std::size_t>(i)]);
    return v;
}

std::int16_t get16(std::string_view b, std::size_t o) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[o]) | (static_cast<std::uint8_t>(b[o + 1]) << 8)));
}

}  // namespace

std::string_view filterWire(VoiceFilter filter) {
    switch (filter) {
        case VoiceFilter::None: return "none";
        case VoiceFilter::Chipmunk: return "chipmunk";
        case VoiceFilter::Deep: return "deep";
        case VoiceFilter::Robot: return "robot";
        case VoiceFilter::Echo: return "echo";
        case VoiceFilter::Radio: return "radio";
    }
    return "none";
}

std::string_view filterLabel(VoiceFilter filter) {
    switch (filter) {
        case VoiceFilter::None: return "Normal";
        case VoiceFilter::Chipmunk: return "Chipmunk";
        case VoiceFilter::Deep: return "Deep";
        case VoiceFilter::Robot: return "Robot";
        case VoiceFilter::Echo: return "Echo";
        case VoiceFilter::Radio: return "Radio";
    }
    return "Normal";
}

std::optional<VoiceFilter> filterFromWire(std::string_view wire) {
    for (const auto f : kVoiceFilters) {
        if (filterWire(f) == wire) return f;
    }
    return std::nullopt;
}

namespace audio {

std::int64_t durationMs(const Mono& m) {
    if (m.sampleRate <= 0) return 0;
    return jsRound(static_cast<double>(m.samples.size()) / m.sampleRate * 1000);
}

Mono mixToMono(const std::vector<std::vector<float>>& channels, double sampleRate) {
    if (channels.empty()) return Mono{{}, sampleRate};
    if (channels.size() == 1) return Mono{channels[0], sampleRate};
    std::size_t length = channels[0].size();
    for (const auto& c : channels) length = std::min(length, c.size());
    std::vector<float> out(length);
    for (std::size_t i = 0; i < length; ++i) {
        double sum = 0;
        for (const auto& c : channels) sum += c[i];
        out[i] = static_cast<float>(sum / static_cast<double>(channels.size()));
    }
    return Mono{std::move(out), sampleRate};
}

Mono resample(const Mono& m, double toRate) {
    if (toRate == m.sampleRate || m.samples.empty()) return Mono{m.samples, toRate};
    const double ratio = m.sampleRate / toRate;
    const auto length = static_cast<std::size_t>(std::max(1.0, std::floor(static_cast<double>(m.samples.size()) / ratio)));
    std::vector<float> out(length);
    const std::size_t last = m.samples.size() - 1;
    for (std::size_t i = 0; i < length; ++i) {
        const double pos = static_cast<double>(i) * ratio;
        const auto i0 = static_cast<std::size_t>(std::floor(pos));
        const double frac = pos - static_cast<double>(i0);
        const double a = m.samples[std::min(i0, last)];
        const double b = m.samples[std::min(i0 + 1, last)];
        out[i] = static_cast<float>(a + (b - a) * frac);
    }
    return Mono{std::move(out), toRate};
}

Mono trim(const Mono& m, double startMs, double endMs) {
    const auto n = static_cast<std::int64_t>(m.samples.size());
    const std::size_t start = clampIndex(jsRound(startMs / 1000 * m.sampleRate), 0, n);
    const std::size_t end = clampIndex(jsRound(endMs / 1000 * m.sampleRate), static_cast<std::int64_t>(start), n);
    return Mono{std::vector<float>(m.samples.begin() + static_cast<std::ptrdiff_t>(start), m.samples.begin() + static_cast<std::ptrdiff_t>(end)),
                m.sampleRate};
}

Mono fade(const Mono& m, double inMs, double outMs) {
    std::vector<float> out = m.samples;
    const auto size = static_cast<std::int64_t>(out.size());
    const auto inN = static_cast<std::size_t>(std::max<std::int64_t>(0, std::min(size, jsRound(inMs / 1000 * m.sampleRate))));
    const auto outN = static_cast<std::size_t>(std::max<std::int64_t>(0, std::min(size, jsRound(outMs / 1000 * m.sampleRate))));
    for (std::size_t i = 0; i < inN; ++i) out[i] = static_cast<float>(static_cast<double>(out[i]) * (static_cast<double>(i) / static_cast<double>(inN)));
    for (std::size_t i = 0; i < outN; ++i) {
        auto& v = out[out.size() - 1 - i];
        v = static_cast<float>(static_cast<double>(v) * (static_cast<double>(i) / static_cast<double>(outN)));
    }
    return Mono{std::move(out), m.sampleRate};
}

double peak(const std::vector<float>& samples) {
    double p = 0;
    for (const float s : samples) p = std::max(p, std::abs(static_cast<double>(s)));
    return p;
}

Mono normalize(const Mono& m, double target) {
    const double p = peak(m.samples);
    if (p < 1e-4) return m;
    const double g = target / p;
    Mono out{std::vector<float>(m.samples.size()), m.sampleRate};
    for (std::size_t i = 0; i < m.samples.size(); ++i) out.samples[i] = static_cast<float>(static_cast<double>(m.samples[i]) * g);
    return out;
}

Mono gain(const Mono& m, double factor) {
    Mono out{std::vector<float>(m.samples.size()), m.sampleRate};
    for (std::size_t i = 0; i < m.samples.size(); ++i) out.samples[i] = static_cast<float>(clamp(static_cast<double>(m.samples[i]) * factor, -1.0, 1.0));
    return out;
}

Mono applyVoiceFilter(const Mono& m, VoiceFilter filter) {
    switch (filter) {
        case VoiceFilter::None: return m;
        case VoiceFilter::Chipmunk: return repitch(m, 1.4);
        case VoiceFilter::Deep: return repitch(m, 0.75);
        case VoiceFilter::Robot: return normalize(ringModulate(m, 55.0), 0.9);
        case VoiceFilter::Echo: return normalize(echo(m, 240.0, 0.45, 0.6), 0.95);
        case VoiceFilter::Radio: return radio(m);
    }
    return m;
}

std::vector<int> waveformBars(const std::vector<float>& samples, int bars) {
    if (bars <= 0) return {};
    if (samples.empty()) return std::vector<int>(static_cast<std::size_t>(bars), 0);
    const auto n = static_cast<std::int64_t>(samples.size());
    std::vector<double> rms(static_cast<std::size_t>(bars));
    for (std::int64_t b = 0; b < bars; ++b) {
        const std::int64_t start = (b * n) / bars;
        const std::int64_t end = std::max(start + 1, ((b + 1) * n) / bars);
        double sum = 0;
        for (std::int64_t i = start; i < end && i < n; ++i) {
            const double v = samples[static_cast<std::size_t>(i)];
            sum += v * v;
        }
        rms[static_cast<std::size_t>(b)] = std::sqrt(sum / static_cast<double>(end - start));
    }
    const double top = std::max(*std::max_element(rms.begin(), rms.end()), 1e-6);
    std::vector<int> out;
    out.reserve(rms.size());
    for (const double r : rms) out.push_back(static_cast<int>(jsRound(clamp(r / top, 0.0, 1.0) * 100)));
    return out;
}

std::string encodeWav(const Mono& m) {
    const auto n = static_cast<std::uint32_t>(m.samples.size());
    std::string out;
    out.reserve(44 + static_cast<std::size_t>(n) * 2);
    out += "RIFF";
    put32(out, 36 + n * 2);
    out += "WAVE";
    out += "fmt ";
    put32(out, 16);
    put16(out, 1);  // PCM
    put16(out, 1);  // mono
    put32(out, static_cast<std::uint32_t>(static_cast<std::int64_t>(m.sampleRate)));
    put32(out, static_cast<std::uint32_t>(static_cast<std::int64_t>(m.sampleRate * 2)));
    put16(out, 2);
    put16(out, 16);
    out += "data";
    put32(out, n * 2);
    for (const float sample : m.samples) {
        const double s = clamp(sample, -1.0, 1.0);
        const auto v = static_cast<std::int32_t>(s < 0 ? s * 0x8000 : s * 0x7fff);
        put16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
    }
    return out;
}

std::optional<Mono> decodeWav(std::string_view bytes) {
    if (bytes.size() < 44) return std::nullopt;
    if (bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE" || bytes.substr(12, 4) != "fmt " || bytes.substr(36, 4) != "data") return std::nullopt;
    if (get16(bytes, 20) != 1 || get16(bytes, 22) != 1 || get16(bytes, 34) != 16) return std::nullopt;
    const double declared = static_cast<double>(get32(bytes, 40)) / 2.0;
    const auto n = static_cast<std::size_t>(std::min(declared, std::floor(static_cast<double>(bytes.size() - 44) / 2.0)));
    Mono out{std::vector<float>(n), static_cast<double>(get32(bytes, 24))};
    for (std::size_t i = 0; i < n; ++i) {
        const int v = get16(bytes, 44 + i * 2);
        out.samples[i] = static_cast<float>(v < 0 ? v / 32768.0 : v / 32767.0);
    }
    return out;
}

FinishedVoice finishVoice(const Mono& m, double startMs, double endMs, VoiceFilter filter) {
    Mono work = trim(m, startMs, endMs);
    work = applyVoiceFilter(work, filter);
    work = resample(work, std::min(work.sampleRate, kStoryAudioRate));
    work = fade(normalize(work, 0.92), 40.0, 120.0);
    return FinishedVoice{encodeWav(work), durationMs(work), waveformBars(work.samples, 48)};
}

}  // namespace audio
}  // namespace redvault::studio
