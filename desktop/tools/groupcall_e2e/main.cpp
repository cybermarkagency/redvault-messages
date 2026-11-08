// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// A desktop group call participant without the window: the real GroupCallSession (room keys, LiveKit media
// through livekit-ffi, the live channel, the invite message) against a real server, with a synthetic tone as the
// microphone and a test picture as the camera, measuring what comes back. For checking interoperability with
// the web app and for end-to-end encryption; not shipped.
//
//   redvault_groupcall_e2e [--api URL] setup <host> <guest> <password>
//       Registers both accounts (or signs in if they exist), makes them friends and creates a group "Weekend plans".
//   redvault_groupcall_e2e [--api URL] run <handle> [options]        (password in RV_PASSWORD)
//       --start <group id>          start a call there (--kind voice|video, default video)
//       --join <link or call id>    join by a pasted link / the whole message
//       --wait                      wait for a ring and answer it
//       --tone HZ / --expect HZ     the tone sent as the microphone (default 440) and the tone to look for
//       --video                     send a 320x240 test picture as the camera
//       --screen                    share a synthetic 640x360 "screen" (needs Premium)
//       --seconds N                 stay this long once the key is held (default 12)
//       --leave-after-first-peer    leave as soon as someone else has been in the call for --seconds
//
// Prints progress on stderr and one JSON line on stdout. Exit 0 when the call connected and every
// --expect tone arrived.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/friend_repository.hpp"
#include "redvault/groupcall/ffi.hpp"
#include "redvault/groupcall/session.hpp"
#include "redvault/model/group_call_link.hpp"
#include "redvault/net/curl_transport.hpp"

using namespace redvault;
using namespace std::chrono_literals;

namespace {

std::string webBase(const std::string& apiBase) {
    std::string base = apiBase;
    if (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, "/api") == 0) base.resize(base.size() - 4);
    return base + "/";
}

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

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

/// The "video.room" claim of a LiveKit token (its payload is plain base64url JSON).
std::string roomOfToken(const std::string& token) {
    const auto a = token.find('.');
    const auto b = token.find('.', a + 1);
    if (a == std::string::npos || b == std::string::npos) return {};
    std::string payload = token.substr(a + 1, b - a - 1);
    std::replace(payload.begin(), payload.end(), '-', '+');
    std::replace(payload.begin(), payload.end(), '_', '/');
    while (payload.size() % 4) payload += '=';
    try {
        const auto bytes = crypto::base64Decode(payload);
        const std::string text(bytes.begin(), bytes.end());
        const auto at = text.find("\"room\":\"");
        if (at == std::string::npos) return {};
        const auto start = at + 8;
        return text.substr(start, text.find('"', start) - start);
    } catch (const std::exception&) {
        return {};
    }
}

struct Tasks {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    void push(std::function<void()> fn) {
        std::lock_guard lock(mutex);
        queue.push_back(std::move(fn));
    }
    void runAll() {
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard lock(mutex);
                if (queue.empty()) return;
                fn = std::move(queue.front());
                queue.pop_front();
            }
            fn();
        }
    }
};

int setup(const std::string& apiBase, const std::string& hostName, const std::string& guestName, const std::string& password) {
    auto makeApi = [&] {
        return std::make_shared<api::ApiClient>(apiBase, std::make_shared<net::CurlTransport>(), std::make_shared<net::SessionCookieJar>());
    };
    auto signUpOrIn = [&](const std::string& handle, std::shared_ptr<api::ApiClient> api) {
        data::AuthRepository auth(api);
        try {
            auto registered = auth.registerAccount(handle, password, data::SignupConsent{true, true, true});
            std::cerr << "registered " << handle << "\n";
            return std::move(registered.identity);
        } catch (const std::exception& e) {
            std::cerr << "register " << handle << ": " << e.what() << " (signing in instead)\n";
            auto outcome = auth.login(handle, password);
            return std::move(std::get<data::SelfIdentity>(outcome));
        }
    };
    auto hostApi = makeApi();
    auto guestApi = makeApi();
    data::SelfIdentity host = signUpOrIn(hostName, hostApi);
    data::SelfIdentity guest = signUpOrIn(guestName, guestApi);

    data::FriendRepository hostFriends(hostApi);
    data::FriendRepository guestFriends(guestApi);
    try {
        hostFriends.sendRequest(guestName);
    } catch (const std::exception& e) {
        std::cerr << "friend request: " << e.what() << "\n";
    }
    for (const auto& r : guestFriends.pending().incoming) {
        if (r.handle == hostName) {
            guestFriends.accept(guest, r);
            std::cerr << "accepted\n";
        }
    }
    data::GroupRepository groups(hostApi);
    std::vector<api::FriendDto> members;
    for (const auto& f : groups.friends()) {
        if (f.username == guestName) members.push_back(f);
    }
    const std::string groupId = groups.create(host, "Weekend plans", members);
    std::printf("{\"groupId\":\"%s\",\"host\":\"%s\",\"guest\":\"%s\",\"hostId\":\"%s\",\"guestId\":\"%s\"}\n", groupId.c_str(), hostName.c_str(),
                guestName.c_str(), host.userId.c_str(), guest.userId.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string apiBase = std::getenv("REDVAULT_API_BASE") ? std::getenv("REDVAULT_API_BASE") : "http://127.0.0.1:3000/api/";
    if (args.size() >= 2 && args[0] == "--api") {
        apiBase = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty()) {
        std::cerr << "usage: redvault_groupcall_e2e [--api URL] setup <host> <guest> <password> | run <handle> [options]\n";
        return 2;
    }
    try {
        if (args[0] == "setup" && args.size() == 4) return setup(apiBase, args[1], args[2], args[3]);
        if (args[0] != "run" || args.size() < 2) return 2;

        const std::string handle = args[1];
        std::string startGroup, joinText, kind = "video";
        double tone = 440, expectTone = 0, seconds = 12;
        bool sendVideo = false, wait = false, shareScreen = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : ""; };
            if (a == "--start") startGroup = next();
            else if (a == "--join") joinText = next();
            else if (a == "--kind") kind = next();
            else if (a == "--tone") tone = std::stod(next());
            else if (a == "--expect") expectTone = std::stod(next());
            else if (a == "--video") sendVideo = true;
            else if (a == "--seconds") seconds = std::stod(next());
            else if (a == "--wait") wait = true;
            else if (a == "--screen") shareScreen = true;
        }
        const char* pw = std::getenv("RV_PASSWORD");
        if (!pw) {
            std::cerr << "set RV_PASSWORD\n";
            return 2;
        }

        auto api = std::make_shared<api::ApiClient>(apiBase, std::make_shared<net::CurlTransport>(), std::make_shared<net::SessionCookieJar>());
        data::AuthRepository auth(api);
        auto outcome = auth.login(handle, pw);
        auto identity = std::make_shared<data::SelfIdentity>(std::move(std::get<data::SelfIdentity>(outcome)));
        std::cerr << "signed in as " << identity->handle << " (" << identity->subscriptionTier << ")\n";
        auto repo = std::make_shared<data::ConversationRepository>(api);

        std::mutex metaMutex;
        std::map<std::string, data::ConversationMeta> metas;
        auto loadMetas = [&] {
            for (const auto& dto : repo->listConversations()) {
                try {
                    auto meta = repo->hydrateMeta(dto, *identity);
                    if (meta) {
                        std::lock_guard lock(metaMutex);
                        metas.erase(meta->id);
                        metas.emplace(meta->id, std::move(*meta));
                    }
                } catch (const std::exception&) {
                }
            }
        };
        loadMetas();

        lk::Ffi::setSearchDirectories({std::filesystem::path(argv[0]).parent_path()});
        if (!lk::Ffi::available()) {
            std::cerr << "media library unavailable\n";
            return 2;
        }
        lk::Ffi::get().setLogSink([](const std::string& level, const std::string& target, const std::string& message) {
            // RV_FFI_LOG=set prints every level (the library's ICE and TURN activity), for diagnosing a call that won't connect.
            static const bool everything = std::getenv("RV_FFI_LOG") != nullptr;
            if (everything || level == "error" || level == "warn") std::cerr << "[ffi " << level << "] " << target << ": " << message << "\n";
        });

        Tasks tasks;
        std::mutex statsMutex;
        std::map<std::string, int> frames;
        std::map<std::string, std::pair<int, int>> frameSize;
        std::map<std::string, double> frameLuma;
        std::vector<std::string> announced;
        std::string roomName;
        std::vector<std::int16_t> heard;
        // What came out of the speaker in each 4 s stretch: the shape of the call over time (people leaving and rejoining).
        struct Slice {
            double at, rms, tone;
        };
        std::vector<Slice> timeline;
        std::size_t sliceStart = 0;

        lk::GroupCallSession::Environment env;
        env.api = api;
        env.self = identity;
        env.siteBase = webBase(apiBase);
        env.group = [&](const std::string& id) -> std::optional<lk::GroupCallSession::GroupInfo> {
            std::lock_guard lock(metaMutex);
            const auto it = metas.find(id);
            if (it == metas.end() || !it->second.isGroup) return std::nullopt;
            lk::GroupCallSession::GroupInfo g;
            g.label = it->second.handle;
            for (const auto& m : it->second.members) {
                g.members.push_back({m.userId, m.handle, m.avatarUrl.value_or(""), crypto::base64Decode(m.publicKeyB64)});
            }
            return g;
        };
        env.tier = [&] { return identity->subscriptionTier; };
        env.otherCallActive = [] { return false; };
        env.post = [&](std::function<void()> fn) { tasks.push(std::move(fn)); };
        env.announce = [&](const std::string& conversationId, const std::string& text) {
            {
                std::lock_guard lock(metaMutex);
                const auto it = metas.find(conversationId);
                if (it == metas.end()) return false;
                // The meta holds a secret (move-only), so send through a pointer into the map.
                repo->send(it->second, text);
            }
            std::lock_guard lock(statsMutex);
            announced.push_back(text);
            return true;
        };
        env.roomFactory = [&](const lk::RoomConfig& config, lk::RoomCallbacks callbacks) {
            roomName = roomOfToken(config.token);
            std::cerr << "ROOM=" << roomName << "\n";
            return lk::connectLiveKitRoom(config, std::move(callbacks));
        };

        lk::GroupCallSession::Hooks hooks;
        std::string lastPhase;
        hooks.changed = [&] {};
        hooks.video = [&](const std::string& key, const lk::VideoFrame& f) {
            std::lock_guard lock(statsMutex);
            ++frames[key];
            frameSize[key] = {f.width, f.height};
            double sum = 0;
            int n = 0;
            for (int i = 0; i < f.width * f.height; i += 97, ++n) sum += f.data[static_cast<std::size_t>(i)];
            frameLuma[key] = n ? sum / n : 0;
        };
        lk::GroupCallSession session(std::move(env), std::move(hooks));

        // The live channel: events for the session, and what keeps this device "online" for the server.
        std::atomic<bool> stopLive{false};
        std::thread live([&] {
            std::optional<std::int64_t> cursor;
            while (!stopLive) {
                try {
                    auto events = api::live::events(*api, cursor);
                    cursor = events.cursor;
                    for (auto& e : events.events) tasks.push([&session, e] { session.onLiveEvent(e); });
                } catch (const std::exception&) {
                    std::this_thread::sleep_for(1s);
                }
            }
        });
        live.detach();

        std::string startMessage;
        if (!startGroup.empty()) startMessage = session.start(startGroup, kind);
        else if (!joinText.empty()) {
            // A pasted link, or the whole chat message carrying it (or just the call id).
            const auto id = model::group_calls::parseCallLink(joinText);
            startMessage = id ? session.joinByLink(*id) : session.joinByLink(joinText);
        }
        if (!startMessage.empty()) std::cerr << "start refused: " << startMessage << "\n";

        const auto began = std::chrono::steady_clock::now();
        auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(); };
        std::optional<std::chrono::steady_clock::time_point> keyAt;
        auto next = std::chrono::steady_clock::now();
        double phase = 0;
        int tick = 0;
        bool accepted = false;
        bool screenAsked = false;
        std::size_t peakTiles = 0;
        std::string finalPhase, endedMessage;
        std::vector<std::string> sas;
        while (true) {
            next += 20ms;
            tasks.runAll();
            const auto& v = session.view();
            finalPhase = v.phase;
            endedMessage = v.endedMessage;
            if (!v.sas.empty()) sas = v.sas;
            peakTiles = std::max(peakTiles, v.tiles.size());
            if (v.phase != lastPhase) {
                std::cerr << "[" << elapsed() << "s] phase " << v.phase << (v.notice.empty() ? "" : " notice: " + v.notice) << "\n";
                lastPhase = v.phase;
            }
            if (wait && !accepted && v.phase == "incoming") {
                session.accept();
                accepted = true;
            }
            if (v.present && v.phase == "ended") break;
            if (!v.present && (startGroup.empty() && joinText.empty() ? accepted : true) && elapsed() > 2) break;
            if (v.keyReady && v.published && !keyAt) keyAt = std::chrono::steady_clock::now();
            if (keyAt && std::chrono::duration<double>(std::chrono::steady_clock::now() - *keyAt).count() > seconds) break;
            if (elapsed() > seconds + 60) break;

            // The microphone and the speaker.
            std::int16_t block[lk::kFrameSamples];
            session.readAudio(block, lk::kFrameSamples);
            if (keyAt) heard.insert(heard.end(), block, block + lk::kFrameSamples);
            if (keyAt && heard.size() - sliceStart >= static_cast<std::size_t>(4 * lk::kSampleRate)) {
                const std::vector<std::int16_t> slice(heard.begin() + static_cast<std::ptrdiff_t>(sliceStart), heard.end());
                double e = 0;
                for (const auto x : slice) e += static_cast<double>(x) * x;
                timeline.push_back({elapsed(), std::sqrt(e / static_cast<double>(slice.size())), expectTone > 0 ? toneShare(slice, expectTone) : 0});
                sliceStart = heard.size();
            }
            if (tone > 0) {
                std::int16_t frame[lk::kFrameSamples];
                for (int i = 0; i < lk::kFrameSamples; ++i) {
                    frame[i] = static_cast<std::int16_t>(8000 * std::sin(phase));
                    phase += 2.0 * std::numbers::pi * tone / lk::kSampleRate;
                }
                session.sendAudio(frame);
            }
            if (shareScreen && v.published && !screenAsked) {
                screenAsked = true;
                const std::string refused = session.toggleScreenShare();
                std::cerr << "screen share asked: " << (refused.empty() ? "ok" : refused) << std::endl;
            }
            if (v.sharingScreen && tick % 6 == 0) {
                lk::VideoFrame f;
                f.width = 640;
                f.height = 360;
                f.data.assign(640 * 360 * 3 / 2, 128);
                for (int y = 0; y < 360; ++y)
                    for (int x = 0; x < 640; ++x) f.data[static_cast<std::size_t>(y * 640 + x)] = static_cast<std::uint8_t>(((y + tick * 3) / 30) % 2 ? 220 : 60);
                session.sendVideo(lk::VideoSource::Screen, f);
            }
            if (sendVideo && v.published && !v.cameraOff && tick % 3 == 0) {
                lk::VideoFrame f;
                f.width = 320;
                f.height = 240;
                f.data.assign(320 * 240 * 3 / 2, 128);
                for (int y = 0; y < 240; ++y)
                    for (int x = 0; x < 320; ++x) f.data[static_cast<std::size_t>(y * 320 + x)] = static_cast<std::uint8_t>(((x + tick * 4) / 40) % 2 ? 110 : 30);
                session.sendVideo(lk::VideoSource::Camera, f);
            }
            ++tick;
            std::this_thread::sleep_until(next);
        }

        // Summary.
        const std::vector<std::int16_t> tail(heard.begin() + static_cast<std::ptrdiff_t>(heard.size() / 3), heard.end());
        double rms = 0;
        for (const auto s : tail) rms += static_cast<double>(s) * s;
        rms = tail.empty() ? 0 : std::sqrt(rms / static_cast<double>(tail.size()));
        const double share = expectTone > 0 ? toneShare(tail, expectTone) : 0;
        // A tone that isn't being sent: how much of the signal is there by chance. The real tone should dwarf it.
        const double control = expectTone > 0 ? toneShare(tail, expectTone * 1.37) : 0;
        std::string json = "{\"connected\":" + std::string(keyAt ? "true" : "false") + ",\"finalPhase\":\"" + finalPhase + "\",\"endedMessage\":\"" +
                           jsonEscape(endedMessage) + "\",\"room\":\"" + roomName + "\",\"peakTiles\":" + std::to_string(peakTiles) +
                           ",\"audioRms\":" + std::to_string(rms) + ",\"expectedToneShare\":" + std::to_string(share) + ",\"controlToneShare\":" + std::to_string(control) + ",\"sas\":[";
        for (std::size_t i = 0; i < sas.size(); ++i) json += std::string(i ? "," : "") + "\"" + sas[i] + "\"";
        json += "],\"timeline\":[";
        for (std::size_t i = 0; i < timeline.size(); ++i) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s{\"t\":%.1f,\"rms\":%.0f,\"tone\":%.2f}", i ? "," : "", timeline[i].at, timeline[i].rms, timeline[i].tone);
            json += buf;
        }
        json += "],\"announced\":[";
        {
            std::lock_guard lock(statsMutex);
            for (std::size_t i = 0; i < announced.size(); ++i) json += std::string(i ? "," : "") + "\"" + jsonEscape(announced[i]) + "\"";
            json += "],\"video\":{";
            bool first = true;
            for (const auto& [k, n] : frames) {
                json += std::string(first ? "" : ",") + "\"" + k + "\":{\"frames\":" + std::to_string(n) + ",\"w\":" + std::to_string(frameSize[k].first) +
                        ",\"h\":" + std::to_string(frameSize[k].second) + ",\"meanLuma\":" + std::to_string(frameLuma[k]) + "}";
                first = false;
            }
        }
        json += "}}";
        std::printf("%s\n", json.c_str());
        std::fflush(stdout);

        session.leave();
        tasks.runAll();
        stopLive = true;
        // The session's destructor lets the goodbye go out; the live poll thread may still be waiting, so don't join it.
        const bool ok = keyAt.has_value() && (expectTone <= 0 || (share >= 0.3 && share > 8 * control));
        return ok ? 0 : 3;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
