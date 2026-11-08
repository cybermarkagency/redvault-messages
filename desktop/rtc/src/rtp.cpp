// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "rtp.hpp"

#include <algorithm>
#include <cstring>

namespace redvault::rtc {

std::optional<RtpView> parseRtp(const std::uint8_t* data, std::size_t size) {
    if (size < 12 || (data[0] >> 6) != 2) return std::nullopt;
    const std::uint8_t pt = data[1] & 0x7F;
    // RTCP packet types 200-206 land on 72-78 once the marker bit is masked off.
    if (pt >= 72 && pt <= 78) return std::nullopt;
    RtpView v;
    v.payloadType = pt;
    v.marker = (data[1] & 0x80) != 0;
    v.sequence = static_cast<std::uint16_t>(data[2] << 8 | data[3]);
    v.timestamp = static_cast<std::uint32_t>(data[4]) << 24 | static_cast<std::uint32_t>(data[5]) << 16 | static_cast<std::uint32_t>(data[6]) << 8 | data[7];
    v.ssrc = static_cast<std::uint32_t>(data[8]) << 24 | static_cast<std::uint32_t>(data[9]) << 16 | static_cast<std::uint32_t>(data[10]) << 8 | data[11];
    std::size_t offset = 12 + 4u * (data[0] & 0x0F);
    if (offset > size) return std::nullopt;
    if (data[0] & 0x10) {  // header extension
        if (offset + 4 > size) return std::nullopt;
        const std::size_t words = static_cast<std::size_t>(data[offset + 2] << 8 | data[offset + 3]);
        offset += 4 + words * 4;
        if (offset > size) return std::nullopt;
    }
    std::size_t end = size;
    if (data[0] & 0x20) {  // padding
        const std::size_t pad = data[size - 1];
        if (pad == 0 || pad > end - offset) return std::nullopt;
        end -= pad;
    }
    v.payload = data + offset;
    v.payloadSize = end - offset;
    return v;
}

std::vector<::rtc::binary> Vp8RtpPacketizer::fragment(::rtc::binary data) {
    // RFC 7741: one-byte descriptor, X=0, N=0, S on the first packet of the frame, PID 0.
    constexpr std::size_t kMaxPayload = DefaultMaxFragmentSize - 1;
    std::vector<::rtc::binary> out;
    std::size_t offset = 0;
    do {
        const std::size_t n = std::min(kMaxPayload, data.size() - offset);
        ::rtc::binary chunk;
        chunk.reserve(n + 1);
        chunk.push_back(static_cast<std::byte>(offset == 0 ? 0x10 : 0x00));
        chunk.insert(chunk.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + n));
        out.push_back(std::move(chunk));
        offset += n;
    } while (offset < data.size());
    return out;
}

Vp8Assembler::Result Vp8Assembler::push(const RtpView& packet) {
    Result result;
    if (packet.payloadSize < 1) return result;
    const std::uint8_t* p = packet.payload;
    std::size_t n = packet.payloadSize;
    // Payload descriptor.
    const std::uint8_t first = p[0];
    const bool extended = first & 0x80;
    const bool start = first & 0x10;
    const std::uint8_t partition = first & 0x07;
    std::size_t header = 1;
    if (extended) {
        if (n < 2) return result;
        const std::uint8_t ext = p[1];
        header = 2;
        if (ext & 0x80) {  // PictureID
            if (n < header + 1) return result;
            header += (p[header] & 0x80) ? 2 : 1;
        }
        if (ext & 0x40) header += 1;           // TL0PICIDX
        if (ext & 0x20 || ext & 0x10) header += 1;  // TID / KEYIDX
    }
    if (n <= header) return result;

    const bool gap = lastSeq_ && static_cast<std::uint16_t>(*lastSeq_ + 1) != packet.sequence;
    lastSeq_ = packet.sequence;
    if (gap && collecting_) {
        broken_ = true;
        result.lost = true;
    }

    if (start && partition == 0) {
        if (collecting_ && !broken_ && packet.timestamp != timestamp_) result.lost = true;  // previous frame never finished
        buffer_.clear();
        collecting_ = true;
        broken_ = false;
        timestamp_ = packet.timestamp;
    } else if (!collecting_ || packet.timestamp != timestamp_) {
        // The start of this frame was lost.
        collecting_ = false;
        result.lost = gap || result.lost;
        return result;
    }
    buffer_.insert(buffer_.end(), p + header, p + n);
    if (packet.marker) {
        collecting_ = false;
        if (broken_) {
            result.lost = true;
        } else if (!buffer_.empty()) {
            result.keyframe = (buffer_[0] & 0x01) == 0;  // VP8 payload header: P bit clear on keyframes
            result.frame = std::move(buffer_);
        }
        buffer_ = {};
    }
    return result;
}

namespace {
constexpr std::size_t kStartPackets = 3;  // ~60 ms before playing
constexpr std::size_t kMaxPackets = 15;   // past ~300 ms, skip ahead
}  // namespace

std::int64_t AudioJitterBuffer::unwrap(std::uint16_t sequence) {
    if (!highest_) return sequence;
    const std::int64_t base = *highest_ & ~0xFFFFLL;
    std::int64_t candidate = base | sequence;
    if (candidate - *highest_ > 0x8000) candidate -= 0x10000;
    else if (*highest_ - candidate > 0x8000) candidate += 0x10000;
    return candidate;
}

void AudioJitterBuffer::push(std::uint16_t sequence, const std::uint8_t* payload, std::size_t size) {
    std::lock_guard lock(mutex_);
    ++received_;
    const std::int64_t seq = unwrap(sequence);
    if (!highest_ || seq > *highest_) highest_ = seq;
    if (playing_ && seq < next_) return;  // too late
    packets_[seq].assign(payload, payload + size);
    while (packets_.size() > kMaxPackets) packets_.erase(packets_.begin());
}

void AudioJitterBuffer::decodeNext(std::int16_t* frame) {
    auto it = packets_.find(next_);
    if (it != packets_.end()) {
        decoder_.decode(it->second.data(), it->second.size(), frame);
        packets_.erase(it);
    } else {
        auto following = packets_.find(next_ + 1);
        if (following != packets_.end()) decoder_.decodeFec(following->second.data(), following->second.size(), frame);
        else decoder_.decode(nullptr, 0, frame);
    }
    ++next_;
}

void AudioJitterBuffer::read(std::int16_t* out, std::size_t samples) {
    std::lock_guard lock(mutex_);
    std::size_t written = 0;
    while (written < samples) {
        if (pending_.empty()) {
            if (!playing_) {
                if (packets_.size() < kStartPackets) break;
                playing_ = true;
                next_ = packets_.begin()->first;
            }
            if (packets_.empty()) {
                // Ran dry: wait for the buffer to refill instead of stuttering.
                playing_ = false;
                break;
            }
            // Fell behind (e.g. after a stall): jump to the oldest packet still held.
            if (packets_.begin()->first > next_ + static_cast<std::int64_t>(kMaxPackets)) next_ = packets_.begin()->first;
            pending_.resize(kFrameSamples);
            decodeNext(pending_.data());
        }
        const std::size_t n = std::min(samples - written, pending_.size());
        std::memcpy(out + written, pending_.data(), n * sizeof(std::int16_t));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        written += n;
    }
    if (written < samples) std::memset(out + written, 0, (samples - written) * sizeof(std::int16_t));
}

std::uint64_t AudioJitterBuffer::received() const {
    std::lock_guard lock(mutex_);
    return received_;
}

}  // namespace redvault::rtc
