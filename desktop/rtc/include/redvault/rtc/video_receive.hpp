// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// Keeping call video steady when the network loses or reorders packets.
//
// Before this, one late or missing packet threw the whole frame away and asked
// the sender for a fresh keyframe. A keyframe squeezed into the call's bitrate
// is very blocky, so the picture kept going blurry, sharpening, and going
// blurry again. Now:
//
//   - VideoReceiveBuffer holds packets briefly so late ones still fit, asks
//     for the missing ones again (RTCP NACK), and only falls back to a
//     keyframe when a resend doesn't come. Until that keyframe arrives it
//     hands out nothing, so the last good picture stays up instead of a
//     corrupted one.
//   - SendBitrate adapts what this side sends to the loss and bandwidth the
//     other side reports, instead of a fixed rate.
//
// Pure logic, no libdatachannel, so it's unit-tested on its own.

namespace redvault::rtc {

/// One received VP8 RTP packet: its header fields and the payload after the RTP header.
struct VideoPacket {
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    bool marker = false;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

class VideoReceiveBuffer {
public:
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::milliseconds;

    struct Settings {
        /// A gap this young may just be reordering: don't ask for a resend yet.
        Ms reorderGrace{10};
        /// Ask again for a packet that still hasn't come after this long.
        Ms nackInterval{80};
        int maxNacks = 3;
        /// Stop waiting for a missing packet and ask for a keyframe instead.
        Ms giveUpAfter{350};
        /// Never hold more than this many packets (a long wait for a keyframe).
        std::size_t maxPackets = 3000;
    };

    struct Frame {
        std::vector<std::uint8_t> data;  // the VP8 frame, descriptors removed
        bool keyframe = false;
    };

    struct Output {
        /// Whole frames, in order, ready for the decoder.
        std::vector<Frame> frames;
        /// Sequence numbers to ask the sender for again.
        std::vector<std::uint16_t> nack;
        /// Nothing can be decoded until a keyframe comes (the caller rate-limits the request).
        bool needKeyframe = false;
    };

    VideoReceiveBuffer() = default;
    explicit VideoReceiveBuffer(Settings settings) : settings_(settings) {}

    Output push(const VideoPacket& packet, Clock::time_point now);
    /// Resends and give-ups that are due without a new packet.
    Output poll(Clock::time_point now);

    /// The decoder couldn't use a frame: drop everything up to the next keyframe.
    void resync();

private:
    struct Packet {
        std::uint32_t timestamp = 0;
        bool marker = false;
        bool frameStart = false;
        bool keyframe = false;  // only meaningful on frameStart
        std::vector<std::uint8_t> data;
    };
    struct Missing {
        Clock::time_point since;
        std::optional<Clock::time_point> lastNack;
        int tries = 0;
    };

    std::int64_t unwrap(std::uint16_t sequence) const;
    /// The frame starting at `first`: its last sequence number, or null if a packet is still missing.
    std::optional<std::int64_t> completeFrame(std::int64_t first) const;
    void dropBefore(std::int64_t sequence);
    void collect(Output& out, Clock::time_point now);
    void dueNacks(Output& out, Clock::time_point now);

    Settings settings_;
    std::map<std::int64_t, Packet> packets_;
    std::map<std::int64_t, Missing> missing_;
    std::optional<std::int64_t> highest_;
    std::optional<std::int64_t> next_;  // the first packet of the next frame to hand out
    bool waitingForKeyframe_ = true;
};

/// What a peer's RTCP says about the video this side sends.
struct RtcpFeedback {
    /// Loss on our stream since the last report, 0-255 (RFC 3550 "fraction lost").
    std::optional<std::uint8_t> fractionLost;
    /// The peer's estimate of the bandwidth it can receive (REMB), in bits per second.
    std::optional<std::uint64_t> rembBps;
};

/// Reads receiver reports (in SR or RR) for `ourSsrc` and REMB out of a (compound) RTCP packet.
RtcpFeedback parseRtcpFeedback(const std::uint8_t* data, std::size_t size, std::uint32_t ourSsrc);

/// An RTCP generic NACK (RFC 4585) asking `mediaSsrc` to resend `sequences`.
std::vector<std::uint8_t> buildNack(std::uint32_t senderSsrc, std::uint32_t mediaSsrc, const std::vector<std::uint16_t>& sequences);

/**
 * The bitrate this side sends video at. Climbs while the peer reports little
 * loss, backs off when it reports a lot, and never goes above the peer's own
 * bandwidth estimate. Thread-safe.
 */
class SendBitrate {
public:
    SendBitrate(int startKbps = 800, int minKbps = 150, int maxKbps = 2500);
    /// A receiver report arrived; returns the new target.
    int onLoss(std::uint8_t fractionLost);
    /// The peer's bandwidth estimate arrived; returns the new target.
    int onRemb(std::uint64_t bps);
    int targetKbps() const;
    /// The largest picture worth sending at the current target (keeps the last size between thresholds).
    std::pair<int, int> maxSize();

private:
    int capLocked() const;

    mutable std::mutex mutex_;
    int target_;
    int min_;
    int max_;
    std::optional<int> rembKbps_;
    bool large_ = false;
};

}  // namespace redvault::rtc
