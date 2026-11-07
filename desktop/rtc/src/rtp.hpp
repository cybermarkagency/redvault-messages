#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <rtc/rtppacketizer.hpp>

#include "codecs.hpp"

namespace redvault::rtc {

/// The parts of one received RTP packet this engine needs.
struct RtpView {
    std::uint8_t payloadType = 0;
    bool marker = false;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

/// Null for RTCP or anything that isn't a well-formed RTP packet.
std::optional<RtpView> parseRtp(const std::uint8_t* data, std::size_t size);

/// Splits a VP8 frame into RTP payloads with the RFC 7741 descriptor (S bit on the first, partition 0).
class Vp8RtpPacketizer final : public ::rtc::RtpPacketizer {
public:
    explicit Vp8RtpPacketizer(std::shared_ptr<::rtc::RtpPacketizationConfig> config) : RtpPacketizer(std::move(config)) {}

protected:
    std::vector<::rtc::binary> fragment(::rtc::binary data) override;
};

/// Rebuilds VP8 frames from RTP packets (RFC 7741). A frame with a missing packet is dropped.
class Vp8Assembler {
public:
    struct Result {
        std::optional<std::vector<std::uint8_t>> frame;
        bool keyframe = false;
        /// A packet was lost: ask the sender for a keyframe.
        bool lost = false;
    };
    Result push(const RtpView& packet);

private:
    std::vector<std::uint8_t> buffer_;
    std::uint32_t timestamp_ = 0;
    std::optional<std::uint16_t> lastSeq_;
    bool collecting_ = false;
    bool broken_ = false;
};

/**
 * Received Opus packets in, steady 48 kHz audio out. Holds about 60 ms
 * before starting, conceals a lost packet (with the next packet's FEC when
 * it's there), and drops old audio when it falls too far behind so the call
 * doesn't drift into lag.
 */
class AudioJitterBuffer {
public:
    void push(std::uint16_t sequence, const std::uint8_t* payload, std::size_t size);
    void read(std::int16_t* out, std::size_t samples);
    std::uint64_t received() const;

private:
    std::int64_t unwrap(std::uint16_t sequence);
    void decodeNext(std::int16_t* frame);

    mutable std::mutex mutex_;
    OpusDec decoder_;
    std::map<std::int64_t, std::vector<std::uint8_t>> packets_;
    std::optional<std::int64_t> highest_;
    std::int64_t next_ = 0;
    bool playing_ = false;
    std::vector<std::int16_t> pending_;  // decoded samples not handed out yet
    std::uint64_t received_ = 0;
};

}  // namespace redvault::rtc
