// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

#include "redvault/rtc/video_receive.hpp"

using namespace redvault::rtc;
using namespace std::chrono_literals;
using Clock = VideoReceiveBuffer::Clock;

namespace {

/// One frame split into `parts` RTP packets; the VP8 data is {keyframe ? 0x00 : 0x01, frameId, part}.
struct TestFrame {
    std::vector<std::vector<std::uint8_t>> payloads;
    std::uint32_t timestamp;
    std::uint16_t firstSeq;
};

TestFrame makeFrame(std::uint16_t firstSeq, std::uint32_t timestamp, bool keyframe, int parts, std::uint8_t id) {
    TestFrame f{{}, timestamp, firstSeq};
    for (int i = 0; i < parts; ++i) {
        std::vector<std::uint8_t> p;
        p.push_back(i == 0 ? 0x10 : 0x00);  // S bit on the first packet, partition 0
        if (i == 0) p.push_back(keyframe ? 0x00 : 0x01);
        p.push_back(id);
        p.push_back(static_cast<std::uint8_t>(i));
        f.payloads.push_back(std::move(p));
    }
    return f;
}

VideoPacket packetOf(const TestFrame& f, int i) {
    return VideoPacket{static_cast<std::uint16_t>(f.firstSeq + i), f.timestamp, i + 1 == static_cast<int>(f.payloads.size()),
                       f.payloads[static_cast<std::size_t>(i)].data(), f.payloads[static_cast<std::size_t>(i)].size()};
}

std::uint8_t frameId(const VideoReceiveBuffer::Frame& f) {
    return f.data.at(1);  // after the VP8 header byte
}

}  // namespace

TEST_CASE("video frames come out whole and in order", "[rtc][video]") {
    VideoReceiveBuffer buffer;
    const auto t = Clock::now();
    const auto key = makeFrame(100, 9000, true, 3, 1);
    const auto delta = makeFrame(103, 12000, false, 2, 2);

    std::vector<VideoReceiveBuffer::Frame> out;
    for (int i = 0; i < 3; ++i) {
        auto r = buffer.push(packetOf(key, i), t);
        CHECK(r.nack.empty());
        for (auto& f : r.frames) out.push_back(std::move(f));
    }
    for (int i = 0; i < 2; ++i) {
        auto r = buffer.push(packetOf(delta, i), t);
        for (auto& f : r.frames) out.push_back(std::move(f));
    }
    REQUIRE(out.size() == 2);
    CHECK(out[0].keyframe);
    CHECK(frameId(out[0]) == 1);
    CHECK(out[0].data == std::vector<std::uint8_t>{0x00, 1, 0, 1, 1, 1, 2});  // descriptors stripped, parts joined
    CHECK_FALSE(out[1].keyframe);
    CHECK(frameId(out[1]) == 2);
}

TEST_CASE("a reordered video packet doesn't cost a keyframe", "[rtc][video]") {
    VideoReceiveBuffer buffer;
    const auto t = Clock::now();
    const auto key = makeFrame(10, 0, true, 1, 1);
    const auto delta = makeFrame(11, 3000, false, 3, 2);
    REQUIRE(buffer.push(packetOf(key, 0), t).frames.size() == 1);

    auto r = buffer.push(packetOf(delta, 0), t);
    r = buffer.push(packetOf(delta, 2), t + 2ms);  // the middle one is late
    CHECK(r.frames.empty());
    CHECK_FALSE(r.needKeyframe);
    CHECK(r.nack.empty());  // still within the reordering grace
    r = buffer.push(packetOf(delta, 1), t + 4ms);
    REQUIRE(r.frames.size() == 1);
    CHECK(frameId(r.frames[0]) == 2);
    CHECK_FALSE(r.needKeyframe);
}

TEST_CASE("a lost video packet is asked for again and fills the frame", "[rtc][video]") {
    VideoReceiveBuffer buffer;
    const auto t = Clock::now();
    const auto key = makeFrame(500, 0, true, 1, 1);
    const auto delta = makeFrame(501, 3000, false, 3, 2);
    const auto next = makeFrame(504, 6000, false, 1, 3);
    buffer.push(packetOf(key, 0), t);
    buffer.push(packetOf(delta, 0), t);
    buffer.push(packetOf(delta, 2), t);
    auto r = buffer.push(packetOf(next, 0), t + 20ms);
    REQUIRE(r.nack == std::vector<std::uint16_t>{502});
    CHECK(r.frames.empty());  // the next frame waits behind the hole

    r = buffer.poll(t + 40ms);
    CHECK(r.nack.empty());  // not again so soon
    r = buffer.poll(t + 110ms);
    CHECK(r.nack == std::vector<std::uint16_t>{502});

    r = buffer.push(packetOf(delta, 1), t + 150ms);  // the resend
    REQUIRE(r.frames.size() == 2);
    CHECK(frameId(r.frames[0]) == 2);
    CHECK(frameId(r.frames[1]) == 3);
    CHECK_FALSE(r.needKeyframe);
}

TEST_CASE("video gives up on a packet that never comes and waits for a keyframe", "[rtc][video]") {
    VideoReceiveBuffer buffer;
    const auto t = Clock::now();
    buffer.push(packetOf(makeFrame(1, 0, true, 1, 1), 0), t);
    const auto lost = makeFrame(2, 3000, false, 2, 2);
    buffer.push(packetOf(lost, 0), t);
    // packet 3 never arrives
    auto r = buffer.push(packetOf(makeFrame(4, 6000, false, 1, 3), 0), t + 30ms);
    CHECK_FALSE(r.needKeyframe);

    r = buffer.poll(t + 500ms);
    CHECK(r.needKeyframe);
    CHECK(r.frames.empty());
    // Later delta frames can't be shown (they'd decode into a damaged picture).
    r = buffer.push(packetOf(makeFrame(5, 9000, false, 1, 4), 0), t + 510ms);
    CHECK(r.frames.empty());
    CHECK(r.needKeyframe);

    const auto key = makeFrame(6, 12000, true, 2, 5);
    buffer.push(packetOf(key, 0), t + 520ms);
    r = buffer.push(packetOf(key, 1), t + 521ms);
    REQUIRE(r.frames.size() == 1);
    CHECK(r.frames[0].keyframe);
    CHECK(frameId(r.frames[0]) == 5);
    CHECK_FALSE(r.needKeyframe);

    r = buffer.push(packetOf(makeFrame(8, 15000, false, 1, 6), 0), t + 550ms);
    REQUIRE(r.frames.size() == 1);
    CHECK(frameId(r.frames[0]) == 6);
}

TEST_CASE("video starts at the first keyframe and survives sequence wrap", "[rtc][video]") {
    VideoReceiveBuffer buffer;
    const auto t = Clock::now();
    auto r = buffer.push(packetOf(makeFrame(65530, 0, false, 1, 1), 0), t);  // joined mid-stream
    CHECK(r.frames.empty());
    CHECK(r.needKeyframe);

    const auto key = makeFrame(65534, 3000, true, 3, 2);  // 65534, 65535, 0
    std::size_t frames = 0;
    for (int i = 0; i < 3; ++i) frames += buffer.push(packetOf(key, i), t).frames.size();
    CHECK(frames == 1);
    r = buffer.push(packetOf(makeFrame(1, 6000, false, 1, 3), 0), t);
    REQUIRE(r.frames.size() == 1);
    CHECK(frameId(r.frames[0]) == 3);
    CHECK(r.nack.empty());
}

TEST_CASE("a decoder failure resyncs on the next keyframe", "[rtc][video]") {
    VideoReceiveBuffer buffer;
    const auto t = Clock::now();
    buffer.push(packetOf(makeFrame(1, 0, true, 1, 1), 0), t);
    buffer.resync();
    auto r = buffer.push(packetOf(makeFrame(2, 3000, false, 1, 2), 0), t);
    CHECK(r.frames.empty());
    CHECK(r.needKeyframe);
    r = buffer.push(packetOf(makeFrame(3, 6000, true, 1, 3), 0), t);
    REQUIRE(r.frames.size() == 1);
    CHECK(frameId(r.frames[0]) == 3);
}

TEST_CASE("NACKs pack nearby sequence numbers into one entry", "[rtc][video]") {
    const auto nack = buildNack(0x11223344, 0xAABBCCDD, {100, 101, 105, 116, 117, 65535, 0});
    // 100 + bits for 101 (1), 105 (5), 116 (16); then 117 alone; then 65535 + bit for 0.
    const std::vector<std::uint8_t> expected{
        0x81, 205, 0x00, 0x05,                          // V=2 FMT=1, RTPFB, length 5 words after the first
        0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD,  // sender, media SSRC
        0x00, 100, 0x80, 0x11,                          // PID 100, BLP bits 0, 4, 15
        0x00, 117, 0x00, 0x00,                          // PID 117
        0xFF, 0xFF, 0x00, 0x01,                         // PID 65535, BLP bit 0 (sequence 0)
    };
    CHECK(nack == expected);
    CHECK(buildNack(1, 2, {}).empty());
}

TEST_CASE("RTCP feedback reads loss for our stream and REMB", "[rtc][video]") {
    const std::uint32_t ours = 0x01020304;
    std::vector<std::uint8_t> rtcp{
        // SR from the peer (sender info) with two report blocks: someone else's, then ours.
        0x82, 200, 0x00, 18,
        0x0A, 0x0B, 0x0C, 0x0D,                                                       // SR sender SSRC
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                   // sender info (20 bytes)
        0x09, 0x09, 0x09, 0x09, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // not ours
        0x01, 0x02, 0x03, 0x04, 26, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // ours: 26/256 lost
        // REMB: exp 4, mantissa 100000 -> 1,600,000 bps
        0x8F, 206, 0x00, 0x05,
        0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01, static_cast<std::uint8_t>(4 << 2 | (100000 >> 16)), static_cast<std::uint8_t>((100000 >> 8) & 0xFF),
        static_cast<std::uint8_t>(100000 & 0xFF),
        0x01, 0x02, 0x03, 0x04,
    };
    const auto f = parseRtcpFeedback(rtcp.data(), rtcp.size(), ours);
    REQUIRE(f.fractionLost);
    CHECK(*f.fractionLost == 26);
    REQUIRE(f.rembBps);
    CHECK(*f.rembBps == 1'600'000);

    const auto other = parseRtcpFeedback(rtcp.data(), rtcp.size(), 0x55555555);
    CHECK_FALSE(other.fractionLost);
    CHECK(parseRtcpFeedback(rtcp.data(), 10, ours).fractionLost == std::nullopt);  // truncated: ignored
}

TEST_CASE("the sending bitrate follows the peer's reports", "[rtc][video]") {
    SendBitrate rate(800, 150, 2500);
    CHECK(rate.maxSize() == std::pair{640, 480});
    int kbps = 0;
    for (int i = 0; i < 10; ++i) kbps = rate.onLoss(0);
    CHECK(kbps > 1500);
    CHECK(rate.maxSize() == std::pair{1280, 720});

    const int before = kbps;
    kbps = rate.onLoss(128);  // 50% loss: back off by a quarter
    CHECK(kbps == before * 3 / 4);
    for (int i = 0; i < 6; ++i) kbps = rate.onLoss(128);
    CHECK(kbps >= 150);
    CHECK(rate.maxSize() == std::pair{640, 480});

    for (int i = 0; i < 40; ++i) kbps = rate.onLoss(0);
    CHECK(kbps == 2500);
    kbps = rate.onRemb(1'000'000);  // the peer can only take 1 Mbps
    CHECK(kbps == 950);
    CHECK(rate.onLoss(0) == 950);
    CHECK(rate.onLoss(13) == 950);  // 5% loss: hold
}
