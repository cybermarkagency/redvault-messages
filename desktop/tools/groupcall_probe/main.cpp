// Joins a LiveKit room through the same code the desktop app uses for group
// calls (redvault_groupcall), sends a synthetic tone and test picture, and
// reports what arrives, so the media path can be checked without a camera,
// a microphone or the GUI.
//
//   redvault_groupcall_probe --url wss://host/path --token <jwt>
//       [--key <64 hex chars> --epoch N]   room key to install (E2EE); without one nothing is published
//       [--tone HZ]                        send this tone (default 440; 0 = send nothing)
//       [--expect HZ]                      measure this tone in what arrives
//       [--video]                          also send a 320x240 test picture
//       [--seconds N]                      how long to stay (default 8)
//       [--late-key HEX --late-epoch N --late-after S]  install a second key mid-call (a new epoch)
//
// Prints one JSON line at the end. Exit code 0 when connected and (if it was
// asked to expect a tone) that tone arrived.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>

#include "redvault/crypto/bytes.hpp"
#include "redvault/groupcall/ffi.hpp"
#include "redvault/groupcall/room.hpp"

using namespace redvault;
using namespace std::chrono_literals;

namespace {

std::vector<std::uint8_t> fromHex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

/// Power of `freq` in 48 kHz mono samples (Goertzel), relative to the signal's total power: 1 is a pure tone.
double toneShare(const std::vector<std::int16_t>& s, double freq) {
    if (s.size() < 960) return 0;
    const double w = 2.0 * std::numbers::pi * freq / lk::kSampleRate;
    const double coeff = 2.0 * std::cos(w);
    double q1 = 0, q2 = 0, total = 0;
    for (const auto x : s) {
        const double q0 = coeff * q1 - q2 + x;
        q2 = q1;
        q1 = q0;
        total += static_cast<double>(x) * x;
    }
    const double power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
    return total > 0 ? 2.0 * power / (static_cast<double>(s.size()) * total) : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string url, token, keyHex, lateKeyHex;
    long long epoch = 1, lateEpoch = 2;
    double tone = 440, expectTone = 0, lateAfter = -1, seconds = 8;
    bool video = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--url") url = next();
        else if (a == "--token") token = next();
        else if (a == "--key") keyHex = next();
        else if (a == "--epoch") epoch = std::stoll(next());
        else if (a == "--tone") tone = std::stod(next());
        else if (a == "--expect") expectTone = std::stod(next());
        else if (a == "--video") video = true;
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--late-key") lateKeyHex = next();
        else if (a == "--late-epoch") lateEpoch = std::stoll(next());
        else if (a == "--late-after") lateAfter = std::stod(next());
    }
    if (url.empty() || token.empty()) {
        std::cerr << "usage: redvault_groupcall_probe --url wss://... --token <jwt> [--key HEX --epoch N] [--tone HZ] [--expect HZ] [--video] [--seconds N]\n";
        return 2;
    }

    lk::Ffi::setSearchDirectories({std::filesystem::path(argv[0]).parent_path()});
    std::string why;
    if (!lk::Ffi::available(&why)) {
        std::cerr << "FFI unavailable: " << why << "\n";
        return 2;
    }
    lk::Ffi::get().setLogSink([](const std::string& level, const std::string& target, const std::string& message) {
        if (level == "error" || level == "warn") std::cerr << "[ffi " << level << "] " << target << ": " << message << "\n";
    });

    std::mutex mutex;
    std::map<std::string, int> videoFrames;
    std::map<std::string, std::pair<int, int>> videoSize;
    std::map<std::string, double> videoLuma;
    std::map<std::string, std::string> e2ee;
    std::string lastState = "none";
    std::atomic<bool> dropped{false};

    lk::RoomCallbacks callbacks;
    callbacks.onState = [&](lk::RoomState s, const std::string& detail) {
        std::lock_guard lock(mutex);
        lastState = s == lk::RoomState::Connected ? "connected" : s == lk::RoomState::Reconnecting ? "reconnecting" : "disconnected:" + detail;
        if (s == lk::RoomState::Disconnected) dropped = true;
    };
    callbacks.onVideo = [&](const std::string& who, lk::VideoSource source, const lk::VideoFrame& f) {
        std::lock_guard lock(mutex);
        const std::string k = who + (source == lk::VideoSource::Screen ? "/screen" : "/camera");
        ++videoFrames[k];
        videoSize[k] = {f.width, f.height};
        double sum = 0;
        for (int i = 0; i < f.width * f.height; i += 97) sum += f.data[static_cast<std::size_t>(i)];
        videoLuma[k] = sum / (f.width * f.height / 97.0);
    };
    callbacks.onEncryption = [&](const std::string& who, lk::E2eeState s) {
        static const char* names[] = {"new", "ok", "encryption_failed", "decryption_failed", "missing_key", "key_ratcheted", "internal_error"};
        std::lock_guard lock(mutex);
        e2ee[who] = names[static_cast<int>(s)];
    };

    std::unique_ptr<lk::Room> room;
    try {
        room = lk::connectLiveKitRoom({url, token}, std::move(callbacks));
    } catch (const std::exception& e) {
        std::cerr << "connect failed: " << e.what() << "\n";
        std::printf("{\"connected\":false,\"error\":\"%s\"}\n", e.what());
        return 1;
    }
    std::cerr << "connected as " << room->localIdentity() << "\n";

    if (!keyHex.empty()) {
        room->setKey(fromHex(keyHex), epoch);
        room->publishMicrophone();
        if (video) room->publishVideo(lk::VideoSource::Camera, 320, 240);
        std::cerr << "published\n";
    }

    std::vector<std::int16_t> heard;
    std::size_t peakParticipants = 0;
    const auto start = std::chrono::steady_clock::now();
    auto next = start;
    double phase = 0;
    int tick = 0;
    bool lateDone = false;
    while (!dropped && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        next += 20ms;
        // What the speaker would play: pull 20 ms, keep it for analysis.
        std::int16_t block[lk::kFrameSamples];
        room->readAudio(block, lk::kFrameSamples);
        heard.insert(heard.end(), block, block + lk::kFrameSamples);

        if (!keyHex.empty() && tone > 0) {
            std::int16_t frame[lk::kFrameSamples];
            for (int i = 0; i < lk::kFrameSamples; ++i) {
                frame[i] = static_cast<std::int16_t>(8000 * std::sin(phase));
                phase += 2.0 * std::numbers::pi * tone / lk::kSampleRate;
            }
            room->sendAudio(frame);
        }
        if (!keyHex.empty() && video && tick % 3 == 0) {
            lk::VideoFrame f;
            f.width = 320;
            f.height = 240;
            f.data.assign(320 * 240 * 3 / 2, 128);
            // A bar that moves, so every picture differs and the mean luma is known (about 70).
            for (int y = 0; y < 240; ++y)
                for (int x = 0; x < 320; ++x) f.data[static_cast<std::size_t>(y * 320 + x)] = static_cast<std::uint8_t>(((x + tick * 4) / 40) % 2 ? 110 : 30);
            room->sendVideo(lk::VideoSource::Camera, f);
        }
        if (!lateKeyHex.empty() && !lateDone && lateAfter >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > lateAfter) {
            room->setKey(fromHex(lateKeyHex), lateEpoch);
            lateDone = true;
            std::cerr << "late key installed\n";
        }
        peakParticipants = std::max(peakParticipants, room->participants().size());
        ++tick;
        std::this_thread::sleep_until(next);
    }

    const auto remote = room->participants();
    // Energy and tone share over the second half, once everything has settled.
    const std::vector<std::int16_t> tail(heard.begin() + static_cast<std::ptrdiff_t>(heard.size() / 2), heard.end());
    double rms = 0;
    for (const auto s : tail) rms += static_cast<double>(s) * s;
    rms = tail.empty() ? 0 : std::sqrt(rms / static_cast<double>(tail.size()));
    const double share = expectTone > 0 ? toneShare(tail, expectTone) : 0;

    std::string json = "{\"connected\":true,\"state\":\"" + lastState + "\",\"remoteParticipants\":" + std::to_string(remote.size()) +
                       ",\"peakParticipants\":" + std::to_string(peakParticipants) + ",\"audioRms\":" + std::to_string(rms) +
                       ",\"expectedToneShare\":" + std::to_string(share) + ",\"video\":{";
    {
        std::lock_guard lock(mutex);
        bool first = true;
        for (const auto& [k, n] : videoFrames) {
            json += std::string(first ? "" : ",") + "\"" + k + "\":{\"frames\":" + std::to_string(n) + ",\"w\":" + std::to_string(videoSize[k].first) +
                    ",\"h\":" + std::to_string(videoSize[k].second) + ",\"meanLuma\":" + std::to_string(videoLuma[k]) + "}";
            first = false;
        }
        json += "},\"e2ee\":{";
        first = true;
        for (const auto& [k, v] : e2ee) {
            json += std::string(first ? "" : ",") + "\"" + k + "\":\"" + v + "\"";
            first = false;
        }
    }
    json += "}}";
    std::printf("%s\n", json.c_str());
    std::fflush(stdout);

    room->disconnect();
    room.reset();
    lk::Ffi::shutdown();
    if (expectTone > 0 && share < 0.5) return 3;
    return 0;
}
