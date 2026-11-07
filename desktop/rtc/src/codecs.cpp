#include "codecs.hpp"

#include <opus.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>

#include <algorithm>
#include <cstring>

namespace redvault::rtc {

VideoFrame VideoFrame::blank(int width, int height) {
    VideoFrame f;
    f.width = width;
    f.height = height;
    f.data.assign(static_cast<std::size_t>(width) * height * 3 / 2, 0);
    std::fill(f.data.begin() + static_cast<std::ptrdiff_t>(width) * height, f.data.end(), std::uint8_t{128});
    return f;
}

// --- Opus ---

OpusEnc::OpusEnc() {
    int error = 0;
    enc_ = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !enc_) throw RtcError("Couldn't start the Opus encoder");
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
}

OpusEnc::~OpusEnc() {
    if (enc_) opus_encoder_destroy(enc_);
}

std::vector<std::uint8_t> OpusEnc::encode(const std::int16_t* pcm) {
    std::vector<std::uint8_t> out(1500);
    const int n = opus_encode(enc_, pcm, kFrameSamples, out.data(), static_cast<opus_int32>(out.size()));
    if (n < 0) throw RtcError("Opus encoding failed");
    out.resize(static_cast<std::size_t>(n));
    return out;
}

OpusDec::OpusDec() {
    int error = 0;
    // One channel: a stereo stream from a browser is mixed down.
    dec_ = opus_decoder_create(kSampleRate, 1, &error);
    if (error != OPUS_OK || !dec_) throw RtcError("Couldn't start the Opus decoder");
}

OpusDec::~OpusDec() {
    if (dec_) opus_decoder_destroy(dec_);
}

void OpusDec::decode(const std::uint8_t* data, std::size_t size, std::int16_t* out) {
    const int n = opus_decode(dec_, data, static_cast<opus_int32>(data ? size : 0), out, kFrameSamples, 0);
    if (n < 0) {
        std::memset(out, 0, sizeof(std::int16_t) * kFrameSamples);
    } else if (n < kFrameSamples) {
        std::memset(out + n, 0, sizeof(std::int16_t) * static_cast<std::size_t>(kFrameSamples - n));
    }
}

void OpusDec::decodeFec(const std::uint8_t* nextData, std::size_t nextSize, std::int16_t* out) {
    const int n = opus_decode(dec_, nextData, static_cast<opus_int32>(nextSize), out, kFrameSamples, 1);
    if (n < 0) decode(nullptr, 0, out);
}

// --- VP8 ---

Vp8Enc::Vp8Enc() = default;

Vp8Enc::~Vp8Enc() {
    if (ctx_) {
        vpx_codec_destroy(ctx_);
        delete ctx_;
    }
}

void Vp8Enc::configure(int width, int height) {
    if (ctx_) {
        vpx_codec_destroy(ctx_);
        delete ctx_;
        ctx_ = nullptr;
    }
    vpx_codec_enc_cfg_t cfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) throw RtcError("VP8 isn't available");
    cfg.g_w = static_cast<unsigned>(width);
    cfg.g_h = static_cast<unsigned>(height);
    cfg.g_timebase = {1, 1000};  // pts in milliseconds
    cfg.rc_target_bitrate = static_cast<unsigned>(kbps_);
    cfg.rc_end_usage = VPX_CBR;
    cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
    cfg.g_lag_in_frames = 0;
    cfg.g_threads = 2;
    cfg.kf_mode = VPX_KF_AUTO;
    cfg.kf_max_dist = 300;
    cfg.rc_min_quantizer = 4;
    cfg.rc_max_quantizer = 56;
    cfg.rc_undershoot_pct = 100;
    cfg.rc_overshoot_pct = 15;
    cfg.rc_buf_initial_sz = 500;
    cfg.rc_buf_optimal_sz = 600;
    cfg.rc_buf_sz = 1000;
    ctx_ = new vpx_codec_ctx_t{};
    if (vpx_codec_enc_init(ctx_, vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) {
        delete ctx_;
        ctx_ = nullptr;
        throw RtcError("Couldn't start the VP8 encoder");
    }
    vpx_codec_control(ctx_, VP8E_SET_CPUUSED, -6);  // fast enough for real time on a laptop
    vpx_codec_control(ctx_, VP8E_SET_STATIC_THRESHOLD, 1);
    width_ = width;
    height_ = height;
}

void Vp8Enc::setBitrate(int kbps) {
    kbps_ = std::clamp(kbps, 100, 4000);
    width_ = 0;  // reconfigure on the next frame
}

std::vector<std::uint8_t> Vp8Enc::encode(const VideoFrame& frame, bool forceKeyframe, bool& isKeyframe) {
    isKeyframe = false;
    if (frame.width <= 0 || frame.height <= 0 || frame.width % 2 || frame.height % 2) return {};
    if (frame.width != width_ || frame.height != height_ || !ctx_) {
        configure(frame.width, frame.height);
        forceKeyframe = true;
    }
    vpx_image_t img;
    vpx_img_wrap(&img, VPX_IMG_FMT_I420, static_cast<unsigned>(frame.width), static_cast<unsigned>(frame.height), 1,
                 const_cast<std::uint8_t*>(frame.data.data()));
    pts_ += 33;
    if (vpx_codec_encode(ctx_, &img, pts_, 33, forceKeyframe ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_REALTIME) != VPX_CODEC_OK) return {};
    std::vector<std::uint8_t> out;
    vpx_codec_iter_t iter = nullptr;
    while (const vpx_codec_cx_pkt_t* pkt = vpx_codec_get_cx_data(ctx_, &iter)) {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
        const auto* bytes = static_cast<const std::uint8_t*>(pkt->data.frame.buf);
        out.insert(out.end(), bytes, bytes + pkt->data.frame.sz);
        if (pkt->data.frame.flags & VPX_FRAME_IS_KEY) isKeyframe = true;
    }
    return out;
}

Vp8Dec::Vp8Dec() {
    ctx_ = new vpx_codec_ctx_t{};
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 2;
    if (vpx_codec_dec_init(ctx_, vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) {
        delete ctx_;
        ctx_ = nullptr;
        throw RtcError("Couldn't start the VP8 decoder");
    }
}

Vp8Dec::~Vp8Dec() {
    if (ctx_) {
        vpx_codec_destroy(ctx_);
        delete ctx_;
    }
}

std::optional<VideoFrame> Vp8Dec::decode(const std::vector<std::uint8_t>& frame) {
    if (frame.empty()) return std::nullopt;
    if (vpx_codec_decode(ctx_, frame.data(), static_cast<unsigned>(frame.size()), nullptr, 0) != VPX_CODEC_OK) return std::nullopt;
    vpx_codec_iter_t iter = nullptr;
    vpx_image_t* img = vpx_codec_get_frame(ctx_, &iter);
    if (!img || img->fmt != VPX_IMG_FMT_I420) return std::nullopt;
    const int w = static_cast<int>(img->d_w) & ~1;
    const int h = static_cast<int>(img->d_h) & ~1;
    VideoFrame out = VideoFrame::blank(w, h);
    for (int row = 0; row < h; ++row) std::memcpy(out.y() + static_cast<std::ptrdiff_t>(row) * w, img->planes[0] + row * img->stride[0], static_cast<std::size_t>(w));
    for (int row = 0; row < h / 2; ++row) {
        std::memcpy(out.u() + static_cast<std::ptrdiff_t>(row) * (w / 2), img->planes[1] + row * img->stride[1], static_cast<std::size_t>(w / 2));
        std::memcpy(out.v() + static_cast<std::ptrdiff_t>(row) * (w / 2), img->planes[2] + row * img->stride[2], static_cast<std::size_t>(w / 2));
    }
    return out;
}

}  // namespace redvault::rtc
