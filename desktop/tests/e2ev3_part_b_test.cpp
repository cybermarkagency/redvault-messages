// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "redvault/e2ev3/chat.hpp"

// Part B on device keys (docs/signal-integration/09-part-b-calls.md): the call
// and callkey envelopes against the cases both apps share (Android loads the
// same file), their round trip, and the call gate's rules on their own. The
// engine-backed set-up tests are in e2ev3_test.cpp.

namespace e2 = redvault::e2ev3;
using e2::Json;

namespace {

Json partB() {
    std::ifstream in(REDVAULT_V3_CALL_VECTORS, std::ios::binary);
    REQUIRE(in.good());
    return Json::parse(in);
}

const std::string kChat = "11111111-1111-4111-8111-111111111111";
const std::string kCall = "cccccccc-0000-4000-8000-000000000001";
const std::string kDevice = "dddddddd-0000-4000-8000-000000000001";

std::string accepted(const e2::DecodedEnvelope& d) {
    switch (d.kind) {
        case e2::DecodedEnvelope::Kind::Call:
            return "call";
        case e2::DecodedEnvelope::Kind::CallKey:
            return "callkey";
        case e2::DecodedEnvelope::Kind::Ignore:
            return "ignore";
        default:
            return "other";
    }
}

e2::CallMessage signalFrom(const std::string& user, const std::string& device, const std::string& step, std::int64_t ts,
                           const std::string& conversation = kChat) {
    e2::CallMessage m;
    m.conversationId = conversation;
    m.fromUserId = user;
    m.fromDeviceId = device;
    m.ts = ts;
    m.signal = e2::CallSignal{kCall, step, "voice", "v=0\r\n", "*"};
    return m;
}

e2::CallMessage keyFrom(const std::string& user, std::int64_t epoch) {
    e2::CallMessage m;
    m.conversationId = kChat;
    m.fromUserId = user;
    m.fromDeviceId = kDevice;
    m.key = e2::CallKey{kCall, epoch, nullptr};
    return m;
}

}  // namespace

TEST_CASE("part B vectors: every decode case, with now = 200000", "[e2ev3][content][partb]") {
    const Json v = partB();
    const std::int64_t now = v.at("now").get<std::int64_t>();
    const std::string conversation = v.at("conversation").get<std::string>();
    REQUIRE(v.at("decode").size() == 16);
    for (const Json& c : v.at("decode")) {
        INFO(c.at("name").get<std::string>());
        const auto d = e2::decodeEnvelope(c.at("envelope").dump(), conversation, now);
        REQUIRE(accepted(d) == c.at("accept").get<std::string>());
        const Json& e = c.at("envelope");
        if (d.kind == e2::DecodedEnvelope::Kind::Call) {
            REQUIRE(d.call);
            CHECK(d.call->callId == e.at("callId").get<std::string>());
            CHECK(d.call->step == e.at("step").get<std::string>());
            CHECK(d.call->kind == e.at("kind").get<std::string>());
            CHECK(d.call->sdp == e.at("sdp").get<std::string>());
            CHECK(d.call->to == e.at("to").get<std::string>());
            CHECK(d.ts == e.at("ts").get<std::int64_t>());
        }
        if (d.kind == e2::DecodedEnvelope::Kind::CallKey) {
            REQUIRE(d.callKey);
            REQUIRE(d.callKey->key);
            CHECK(d.callKey->key->size() == 32);
            CHECK(d.callKey->epoch == e.at("epoch").get<std::int64_t>());
            CHECK(d.callKey->callId == e.at("callId").get<std::string>());
        }
        // The same envelope bound to another chat is never a call.
        CHECK(e2::decodeEnvelope(c.at("envelope").dump(), "99999999-9999-4999-8999-999999999999", now).kind == e2::DecodedEnvelope::Kind::Ignore);
    }
}

TEST_CASE("part B envelopes round-trip, and what wouldn't decode can't be made", "[e2ev3][content][partb]") {
    const e2::CallSignal offer{kCall, "offer", "video", "v=0\r\na=fingerprint:sha-256 AA\r\n", "*"};
    const std::string id = "bbbbbbbb-0000-4000-8000-000000000009";
    const auto plain = e2::encodeCall(kChat, offer, id, 150000);
    CHECK(plain.size() % 256 == 0);
    const auto back = e2::decodeEnvelope(plain, kChat, 200000);
    REQUIRE(back.kind == e2::DecodedEnvelope::Kind::Call);
    CHECK(back.id == id);
    CHECK(back.call->sdp == offer.sdp);
    CHECK(back.call->to == "*");

    const redvault::crypto::Bytes raw(32, 0x5a);
    const e2::CallKey key{kCall, 3, std::make_shared<const redvault::crypto::SecretBytes>(redvault::crypto::SecretBytes::copyOf(raw))};
    const auto keyBack = e2::decodeEnvelope(e2::encodeCallKey(kChat, key, id, 150000), kChat, 200000);
    REQUIRE(keyBack.kind == e2::DecodedEnvelope::Kind::CallKey);
    CHECK(keyBack.callKey->epoch == 3);
    CHECK(keyBack.callKey->key->view()[31] == 0x5a);

    CHECK_THROWS_AS(e2::encodeCall(kChat, e2::CallSignal{kCall, "ring", "video", "v=0", "*"}, id, 1), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeCall(kChat, e2::CallSignal{kCall, "offer", "video", "", "*"}, id, 1), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeCall(kChat, e2::CallSignal{kCall, "offer", "video", std::string(49153, 'a'), "*"}, id, 1), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeCall(kChat, e2::CallSignal{kCall, "offer", "video", "v=0", "everyone"}, id, 1), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeCallKey(kChat, e2::CallKey{kCall, 0, key.key}, id, 1), e2::EngineError);
    // 49,152 UTF-16 units exactly is fine; a character outside the BMP counts as two.
    CHECK_NOTHROW(e2::encodeCall(kChat, e2::CallSignal{kCall, "offer", "video", std::string(49152, 'a'), "*"}, id, 1));
    std::string wide;
    for (int i = 0; i < 24577; ++i) wide += "\xF0\x9F\x91\x8B";
    CHECK_THROWS_AS(e2::encodeCall(kChat, e2::CallSignal{kCall, "offer", "video", wide, "*"}, id, 1), e2::EngineError);
    // A missing ts is refused for a call (its freshness needs one).
    Json noTs = Json::parse(plain);
    noTs.erase("ts");
    CHECK(e2::decodeEnvelope(noTs.dump(), kChat, 200000).kind == e2::DecodedEnvelope::Kind::Ignore);
}

TEST_CASE("part B call gate: the right party, step, conversation and device", "[e2ev3][partb][gate]") {
    const std::int64_t now = 1000000;
    e2::CallGate gate;
    gate.expectCall(kCall, kChat, "alice", e2::CallGate::Role::Callee);
    std::vector<std::string> refused;
    // An answer isn't for a callee; another chat; another sender.
    CHECK(gate.admit({signalFrom("alice", "dev-a", "answer", now)}, now, &refused).empty());
    CHECK(gate.admit({signalFrom("alice", "dev-a", "offer", now, "99999999-9999-4999-8999-999999999999")}, now, &refused).empty());
    CHECK(gate.admit({signalFrom("mallory", "dev-m", "offer", now)}, now, &refused).empty());
    CHECK(refused == std::vector<std::string>{"wrong_step", "wrong_conversation", "wrong_sender"});
    // A restart before the call is set up, then the offer, then a second offer, then a restart from another device.
    refused.clear();
    CHECK(gate.admit({signalFrom("alice", "dev-a", "restart-offer", now)}, now, &refused).empty());
    CHECK(gate.admit({signalFrom("alice", "dev-a", "offer", now)}, now).size() == 1);
    CHECK(gate.admit({signalFrom("alice", "dev-a2", "offer", now)}, now, &refused).empty());
    CHECK(gate.admit({signalFrom("alice", "dev-a2", "restart-offer", now)}, now, &refused).empty());
    CHECK(gate.admit({signalFrom("alice", "dev-a", "restart-offer", now)}, now).size() == 1);
    CHECK(refused == std::vector<std::string>{"wrong_device", "already_set_up", "wrong_device"});
    // A room key never counts for a one-to-one call, and a call signal never for a group call.
    refused.clear();
    CHECK(gate.admit({keyFrom("alice", 1)}, now, &refused).empty());
    gate.expectGroupCall("cccccccc-0000-4000-8000-000000000002", kChat, {"alice"});
    auto groupSignal = signalFrom("alice", "dev-a", "offer", now);
    groupSignal.signal->callId = "cccccccc-0000-4000-8000-000000000002";
    CHECK(gate.admit({groupSignal}, now, &refused).empty());
    CHECK(refused == std::vector<std::string>{"unknown_call", "unknown_call"});
    // Forgotten: nothing more is used for it.
    gate.forget(kCall);
    CHECK_FALSE(gate.knows(kCall));
}
