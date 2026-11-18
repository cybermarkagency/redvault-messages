// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/rtc/video_receive.hpp"

#include <algorithm>

namespace redvault::rtc {

namespace {

struct Vp8Descriptor {
    std::size_t length = 0;  // bytes before the VP8 data
    bool start = false;
    std::uint8_t partition = 0;
};

/// The RFC 7741 payload descriptor at the front of a VP8 RTP payload. Null if it's malformed.
std::optional<Vp8Descriptor> parseDescriptor(const std::uint8_t* p, std::size_t n) {
    if (n < 1) return std::nullopt;
    Vp8Descriptor d;
    d.start = (p[0] & 0x10) != 0;
    d.partition = p[0] & 0x07;
    d.length = 1;
    if (p[0] & 0x80) {  // extension byte
        if (n < 2) return std::nullopt;
        const std::uint8_t ext = p[1];
        d.length = 2;
        if (ext & 0x80) {  // PictureID, 7 or 15 bits
            if (n < d.length + 1) return std::nullopt;
            d.length += (p[d.length] & 0x80) ? 2 : 1;
        }
        if (ext & 0x40) d.length += 1;                // TL0PICIDX
        if ((ext & 0x20) || (ext & 0x10)) d.length += 1;  // TID / KEYIDX
    }
    if (n <= d.length) return std::nullopt;
    return d;
}

std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 | static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

void putBe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void putBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    putBe16(out, static_cast<std::uint16_t>(v >> 16));
    putBe16(out, static_cast<std::uint16_t>(v));
}

constexpr std::int64_t kResetJump = 1000;  // a jump this big is a restarted stream, not loss
constexpr std::size_t kMaxNacksPerCall = 64;

}  // namespace

// --- VideoReceiveBuffer ---

std::int64_t VideoReceiveBuffer::unwrap(std::uint16_t sequence) const {
    if (!highest_) return sequence + 0x10000LL;  // room below the first packet for reordering
    const std::int64_t base = *highest_ & ~0xFFFFLL;
    std::int64_t candidate = base | sequence;
    if (candidate - *highest_ > 0x8000) candidate -= 0x10000;
    else if (*highest_ - candidate > 0x8000) candidate += 0x10000;
    return candidate;
}

VideoReceiveBuffer::Output VideoReceiveBuffer::push(const VideoPacket& packet, Clock::time_point now) {
    const auto descriptor = parseDescriptor(packet.payload, packet.payloadSize);
    if (!descriptor) return poll(now);
    const std::int64_t seq = unwrap(packet.sequence);

    if (highest_ && (seq - *highest_ > kResetJump || *highest_ - seq > kResetJump)) {
        // The sender started over (or this is junk): forget everything and wait for a keyframe.
        packets_.clear();
        missing_.clear();
        highest_.reset();
        next_.reset();
        waitingForKeyframe_ = true;
        return push(packet, now);
    }
    if (next_ && seq < *next_) return poll(now);  // too late: that frame is already out (or given up)
    if (packets_.count(seq)) return poll(now);    // a duplicate, e.g. a resend that crossed the original

    if (highest_ && seq > *highest_ + 1) {
        for (std::int64_t s = *highest_ + 1; s < seq; ++s) missing_.emplace(s, Missing{now, std::nullopt, 0});
    }
    missing_.erase(seq);
    if (!highest_ || seq > *highest_) highest_ = seq;

    Packet stored;
    stored.timestamp = packet.timestamp;
    stored.marker = packet.marker;
    stored.frameStart = descriptor->start && descriptor->partition == 0;
    const std::uint8_t* vp8 = packet.payload + descriptor->length;
    stored.keyframe = stored.frameStart && (vp8[0] & 0x01) == 0;  // VP8 payload header: P bit clear on keyframes
    stored.data.assign(vp8, packet.payload + packet.payloadSize);
    packets_.emplace(seq, std::move(stored));

    while (packets_.size() > settings_.maxPackets) {
        const std::int64_t oldest = packets_.begin()->first;
        packets_.erase(packets_.begin());
        if (next_ && oldest >= *next_) waitingForKeyframe_ = true;
    }
    return poll(now);
}

VideoReceiveBuffer::Output VideoReceiveBuffer::poll(Clock::time_point now) {
    Output out;
    collect(out, now);
    dueNacks(out, now);
    // Forget holes nobody is waiting for any more.
    for (auto it = missing_.begin(); it != missing_.end();) {
        if (now - it->second.since > settings_.giveUpAfter * 4) it = missing_.erase(it);
        else ++it;
    }
    return out;
}

void VideoReceiveBuffer::resync() {
    waitingForKeyframe_ = true;
}

std::optional<std::int64_t> VideoReceiveBuffer::completeFrame(std::int64_t first) const {
    const auto start = packets_.find(first);
    if (start == packets_.end()) return std::nullopt;
    const std::uint32_t timestamp = start->second.timestamp;
    for (std::int64_t s = first;; ++s) {
        const auto it = packets_.find(s);
        if (it == packets_.end() || it->second.timestamp != timestamp) return std::nullopt;
        if (it->second.marker) return s;
    }
}

void VideoReceiveBuffer::dropBefore(std::int64_t sequence) {
    packets_.erase(packets_.begin(), packets_.lower_bound(sequence));
    missing_.erase(missing_.begin(), missing_.lower_bound(sequence));
}

void VideoReceiveBuffer::collect(Output& out, Clock::time_point now) {
    for (;;) {
        if (waitingForKeyframe_) {
            bool found = false;
            for (const auto& [seq, p] : packets_) {
                if (!p.frameStart || !p.keyframe || !completeFrame(seq)) continue;
                dropBefore(seq);
                next_ = seq;
                waitingForKeyframe_ = false;
                found = true;
                break;
            }
            if (!found) {
                out.needKeyframe = true;
                return;
            }
        }

        const auto it = packets_.find(*next_);
        if (it != packets_.end()) {
            if (!it->second.frameStart) {
                waitingForKeyframe_ = true;  // a frame we only have the middle of
                continue;
            }
            if (const auto last = completeFrame(*next_)) {
                Frame frame;
                frame.keyframe = it->second.keyframe;
                for (std::int64_t s = *next_; s <= *last; ++s) {
                    auto& data = packets_[s].data;
                    frame.data.insert(frame.data.end(), data.begin(), data.end());
                }
                dropBefore(*last + 1);
                next_ = *last + 1;
                out.frames.push_back(std::move(frame));
                continue;
            }
            // Not complete: either a packet hasn't come yet (wait below), or the next frame started
            // before this one ended with nothing missing in between, so it can never be finished.
            std::int64_t s = *next_;
            while (packets_.count(s) && packets_.at(s).timestamp == it->second.timestamp) ++s;
            if (packets_.count(s)) {
                waitingForKeyframe_ = true;
                continue;
            }
        }

        // Something at or after next_ hasn't arrived. Wait for it, unless it's been too long.
        const auto hole = missing_.lower_bound(*next_);
        if (hole != missing_.end() && now - hole->second.since >= settings_.giveUpAfter) {
            waitingForKeyframe_ = true;
            continue;
        }
        return;
    }
}

void VideoReceiveBuffer::dueNacks(Output& out, Clock::time_point now) {
    for (auto& [seq, m] : missing_) {
        if (out.nack.size() >= kMaxNacksPerCall) break;
        if (next_ && !waitingForKeyframe_ && seq < *next_) continue;
        const auto age = now - m.since;
        if (age < settings_.reorderGrace || age >= settings_.giveUpAfter || m.tries >= settings_.maxNacks) continue;
        if (m.lastNack && now - *m.lastNack < settings_.nackInterval) continue;
        out.nack.push_back(static_cast<std::uint16_t>(seq & 0xFFFF));
        m.lastNack = now;
        ++m.tries;
    }
}

// --- RTCP ---

RtcpFeedback parseRtcpFeedback(const std::uint8_t* data, std::size_t size, std::uint32_t ourSsrc) {
    RtcpFeedback feedback;
    std::size_t offset = 0;
    while (offset + 4 <= size) {
        const std::uint8_t* p = data + offset;
        if ((p[0] >> 6) != 2) break;
        const std::uint8_t count = p[0] & 0x1F;
        const std::uint8_t type = p[1];
        const std::size_t length = (static_cast<std::size_t>(p[2] << 8 | p[3]) + 1) * 4;
        if (offset + length > size) break;

        if (type == 200 || type == 201) {  // SR or RR: report blocks after the header (and an SR's sender info)
            const std::size_t first = type == 200 ? 28 : 8;
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t block = first + i * 24;
                if (block + 24 > length) break;
                if (be32(p + block) == ourSsrc) feedback.fractionLost = p[block + 4];
            }
        } else if (type == 206 && count == 15 && length >= 20 && p[12] == 'R' && p[13] == 'E' && p[14] == 'M' && p[15] == 'B') {
            const unsigned exponent = p[17] >> 2;
            const std::uint64_t mantissa = static_cast<std::uint64_t>(p[17] & 0x03) << 16 | static_cast<std::uint64_t>(p[18]) << 8 | p[19];
            feedback.rembBps = mantissa << exponent;
        }
        offset += length;
    }
    return feedback;
}

std::vector<std::uint8_t> buildNack(std::uint32_t senderSsrc, std::uint32_t mediaSsrc, const std::vector<std::uint16_t>& sequences) {
    // Each FCI entry: a packet ID and a bitmask of the 16 after it.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> entries;
    for (const std::uint16_t seq : sequences) {
        if (!entries.empty()) {
            const std::uint16_t distance = static_cast<std::uint16_t>(seq - entries.back().first);
            if (distance == 0) continue;
            if (distance <= 16) {
                entries.back().second |= static_cast<std::uint16_t>(1u << (distance - 1));
                continue;
            }
        }
        entries.emplace_back(seq, 0);
    }
    std::vector<std::uint8_t> out;
    if (entries.empty()) return out;
    out.push_back(0x80 | 1);  // V=2, FMT=1 (generic NACK)
    out.push_back(205);       // RTPFB
    putBe16(out, static_cast<std::uint16_t>(2 + entries.size()));
    putBe32(out, senderSsrc);
    putBe32(out, mediaSsrc);
    for (const auto& [pid, mask] : entries) {
        putBe16(out, pid);
        putBe16(out, mask);
    }
    return out;
}

// --- SendBitrate ---

SendBitrate::SendBitrate(int startKbps, int minKbps, int maxKbps) : target_(startKbps), min_(minKbps), max_(maxKbps) {}

int SendBitrate::capLocked() const {
    int cap = max_;
    if (rembKbps_) cap = std::min(cap, std::max(min_, *rembKbps_ * 95 / 100));
    return cap;
}

int SendBitrate::onLoss(std::uint8_t fractionLost) {
    std::lock_guard lock(mutex_);
    const double loss = fractionLost / 256.0;
    if (loss < 0.02) {
        target_ = static_cast<int>(target_ * 1.08) + 10;
    } else if (loss > 0.10) {
        target_ = static_cast<int>(target_ * (1.0 - 0.5 * loss));
    }
    target_ = std::clamp(target_, min_, capLocked());
    return target_;
}

int SendBitrate::onRemb(std::uint64_t bps) {
    std::lock_guard lock(mutex_);
    rembKbps_ = static_cast<int>(std::min<std::uint64_t>(bps / 1000, 1'000'000));
    target_ = std::clamp(target_, min_, capLocked());
    return target_;
}

int SendBitrate::targetKbps() const {
    std::lock_guard lock(mutex_);
    return target_;
}

std::pair<int, int> SendBitrate::maxSize() {
    std::lock_guard lock(mutex_);
    if (target_ >= 1000) large_ = true;
    else if (target_ <= 650) large_ = false;
    return large_ ? std::pair{1280, 720} : std::pair{640, 480};
}

}  // namespace redvault::rtc
