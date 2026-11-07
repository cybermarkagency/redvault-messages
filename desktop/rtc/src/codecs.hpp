#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "redvault/rtc/call_media.hpp"

struct OpusEncoder;
struct OpusDecoder;
struct vpx_codec_ctx;

namespace redvault::rtc {

/// Opus at 48 kHz mono, tuned for voice: in-band FEC on, so one lost packet can be rebuilt from the next.
class OpusEnc {
public:
    OpusEnc();
    ~OpusEnc();
    OpusEnc(const OpusEnc&) = delete;
    OpusEnc& operator=(const OpusEnc&) = delete;
    std::vector<std::uint8_t> encode(const std::int16_t* pcm);  // kFrameSamples in

private:
    OpusEncoder* enc_ = nullptr;
};

class OpusDec {
public:
    OpusDec();
    ~OpusDec();
    OpusDec(const OpusDec&) = delete;
    OpusDec& operator=(const OpusDec&) = delete;
    /// Decodes one packet (or conceals a lost one when `data` is null) into kFrameSamples samples.
    void decode(const std::uint8_t* data, std::size_t size, std::int16_t* out);
    /// Rebuilds a lost packet from the FEC carried in the packet after it.
    void decodeFec(const std::uint8_t* nextData, std::size_t nextSize, std::int16_t* out);

private:
    OpusDecoder* dec_ = nullptr;
};

/// VP8 for real-time: one frame in, one frame out, keyframes on request.
class Vp8Enc {
public:
    Vp8Enc();
    ~Vp8Enc();
    Vp8Enc(const Vp8Enc&) = delete;
    Vp8Enc& operator=(const Vp8Enc&) = delete;
    /// Returns the encoded frame, and whether it's a keyframe. Reconfigures itself when the size changes.
    std::vector<std::uint8_t> encode(const VideoFrame& frame, bool forceKeyframe, bool& isKeyframe);
    void setBitrate(int kbps);

private:
    void configure(int width, int height);
    vpx_codec_ctx* ctx_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int kbps_ = 900;
    std::int64_t pts_ = 0;
};

class Vp8Dec {
public:
    Vp8Dec();
    ~Vp8Dec();
    Vp8Dec(const Vp8Dec&) = delete;
    Vp8Dec& operator=(const Vp8Dec&) = delete;
    /// Null if the frame couldn't be decoded (ask for a keyframe then).
    std::optional<VideoFrame> decode(const std::vector<std::uint8_t>& frame);

private:
    vpx_codec_ctx* ctx_ = nullptr;
};

}  // namespace redvault::rtc
