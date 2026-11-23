// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/chat.hpp"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <tuple>

#include "batches.hpp"
#include "fs_util.hpp"
#include "redvault/e2ev3/community.hpp"
#include "redvault/e2ev3/stories.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/util/text.hpp"

namespace redvault::e2ev3 {

namespace {

constexpr std::int64_t kDay = 24LL * 3600 * 1000;
constexpr std::size_t kMaxText = 16 * 1024;
constexpr std::size_t kMaxEmoji = 32;
constexpr std::int64_t kMaxPast = 30 * kDay;
constexpr std::int64_t kMaxFuture = 5 * 60 * 1000;
// Part A (docs/signal-integration/08-part-a-chat-features.md). Lengths in characters are UTF-16 code units (textLength).
constexpr int kMaxExp = 2419200;  // four weeks, in seconds
constexpr std::size_t kMaxPreview = 200;
constexpr std::size_t kMaxQuestion = 300;
constexpr std::size_t kMaxOption = 100;
constexpr std::size_t kMinOptions = 2;
constexpr std::size_t kMaxOptions = 10;
constexpr std::size_t kMaxChoices = 10;
constexpr std::size_t kMaxReceiptIds = 100;
// Part D (docs/signal-integration/12-part-d-stories.md).
constexpr std::size_t kMaxStoryPreview = 80;
constexpr std::size_t kMaxStickerId = 64;
constexpr std::int64_t kMaxStoryRev = 1000000000;

std::int64_t resolveNow(std::int64_t nowMs) {
    return nowMs > 0 ? nowMs : util::nowMillis();
}

std::string stringOr(const Json& json, const char* key, const std::string& fallback = {}) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool isUuid(const std::string& text) {
    static const std::regex pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return std::regex_match(text, pattern);
}

Json fileJson(const model::FileDescriptor& file) {
    return Json::parse(model::file_message::serialize(file).substr(model::file_message::kPrefix.size()));
}

/// The same strict check as a protocol-1 descriptor: everything in it came from the other participant.
std::optional<model::FileDescriptor> fileFrom(const Json& json) {
    return model::file_message::parse(model::file_message::kPrefix + json.dump(-1, ' ', false, Json::error_handler_t::replace));
}

Json opJson(const ChatOp& op) {
    Json j = {{"t", op.t}};
    if (op.t == "receipt") {
        j["targets"] = op.targets;
        j["status"] = op.status;
        return j;
    }
    j["target"] = op.target;
    if (op.t == "edit") j["body"] = op.body;
    if (op.t == "reaction") j["emoji"] = op.emoji;
    if (op.t == "vote") j["choices"] = op.choices;
    if (op.t == "pin") j["pinned"] = op.pinned;
    return j;
}

ChatOp opFrom(const Json& j) {
    ChatOp op{stringOr(j, "t"), stringOr(j, "target"), stringOr(j, "body"), stringOr(j, "emoji")};
    if (const auto it = j.find("choices"); it != j.end() && it->is_array()) {
        for (const Json& c : *it) {
            if (c.is_number_integer()) op.choices.push_back(c.get<int>());
        }
    }
    if (const auto it = j.find("pinned"); it != j.end() && it->is_boolean()) op.pinned = it->get<bool>();
    if (const auto it = j.find("targets"); it != j.end() && it->is_array()) {
        for (const Json& t : *it) {
            if (t.is_string()) op.targets.push_back(t.get<std::string>());
        }
    }
    op.status = stringOr(j, "status");
    return op;
}

bool validStoryReply(const model::StoryReplyRef& r) {
    static const std::set<std::string> kKinds = {"text", "image", "video", "audio"};
    return isUuid(r.authorId) && isUuid(r.storyId) && kKinds.count(r.kind) && textLength(r.preview) <= kMaxStoryPreview;
}

Json storyReplyJson(const model::StoryReplyRef& r) {
    return Json{{"authorId", r.authorId}, {"storyId", r.storyId}, {"kind", r.kind}, {"preview", r.preview}, {"reaction", r.reaction}};
}

/// Every field required, with the right type; nothing when anything is off.
std::optional<model::StoryReplyRef> storyReplyFrom(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    const auto field = [&](const char* key) -> const Json* {
        const auto it = j.find(key);
        return it == j.end() || !it->is_string() ? nullptr : &*it;
    };
    const Json* author = field("authorId");
    const Json* story = field("storyId");
    const Json* kind = field("kind");
    const Json* preview = field("preview");
    const auto reaction = j.find("reaction");
    if (!author || !story || !kind || !preview || reaction == j.end() || !reaction->is_boolean()) return std::nullopt;
    model::StoryReplyRef r{author->get<std::string>(), story->get<std::string>(), kind->get<std::string>(), preview->get<std::string>(),
                           reaction->get<bool>()};
    if (!validStoryReply(r)) return std::nullopt;
    return r;
}

Json recordJson(const ChatRecord& r) {
    Json j = {{"conversationId", r.conversationId}, {"id", r.id}, {"from", r.from}, {"fromDeviceId", r.fromDeviceId}, {"ts", r.ts},
              {"arrivedAt", r.arrivedAt}, {"text", r.text}, {"direction", r.direction}, {"status", r.status}};
    if (r.file) j["file"] = fileJson(*r.file);
    if (r.op) j["op"] = opJson(*r.op);
    if (!r.to.empty()) j["to"] = r.to;
    // Part A, only when set: a record without them is stored as before.
    if (!r.replyTo.empty()) j["reply"] = Json{{"target", r.replyTo}, {"preview", r.replyPreview}};
    if (r.forwarded) j["fwd"] = true;
    if (r.exp > 0) j["exp"] = r.exp;
    if (r.poll) j["poll"] = Json{{"options", r.poll->options}, {"multi", r.poll->multi}};
    if (r.storyReply) j["storyReply"] = storyReplyJson(*r.storyReply);
    return j;
}

ChatRecord recordFrom(const Json& j) {
    ChatRecord r;
    r.conversationId = stringOr(j, "conversationId");
    r.id = stringOr(j, "id");
    r.from = stringOr(j, "from");
    r.fromDeviceId = stringOr(j, "fromDeviceId");
    r.ts = j.value("ts", static_cast<std::int64_t>(0));
    r.arrivedAt = j.value("arrivedAt", static_cast<std::int64_t>(0));
    r.text = stringOr(j, "text");
    r.direction = stringOr(j, "direction");
    r.status = stringOr(j, "status");
    // Records written before reactions, edits, deletes and files have neither field.
    if (const auto file = j.find("file"); file != j.end()) r.file = fileFrom(*file);
    if (const auto op = j.find("op"); op != j.end() && op->is_object()) r.op = opFrom(*op);
    r.to = stringOr(j, "to");
    if (const auto reply = j.find("reply"); reply != j.end() && reply->is_object()) {
        r.replyTo = stringOr(*reply, "target");
        r.replyPreview = stringOr(*reply, "preview");
    }
    r.forwarded = j.value("fwd", false);
    r.exp = j.value("exp", 0);
    if (const auto poll = j.find("poll"); poll != j.end() && poll->is_object()) {
        ChatPoll p;
        for (const Json& o : poll->value("options", Json::array())) {
            if (o.is_string()) p.options.push_back(o.get<std::string>());
        }
        p.multi = poll->value("multi", false);
        r.poll = std::move(p);
    }
    if (const auto story = j.find("storyReply"); story != j.end()) r.storyReply = storyReplyFrom(*story);
    return r;
}

/// The (ts, id) order every device agrees on: is `a` before `b`?
bool before(const ChatRecord& a, const ChatRecord& b) {
    return std::tie(a.ts, a.id) < std::tie(b.ts, b.id);
}

bool validChoices(const std::vector<int>& choices) {
    if (choices.size() > kMaxChoices) return false;
    std::set<int> seen;
    for (const int c : choices) {
        if (c < 0 || c > 9 || !seen.insert(c).second) return false;
    }
    return true;
}

bool validPoll(const std::string& question, const std::vector<std::string>& options) {
    const std::size_t q = textLength(question);
    if (q < 1 || q > kMaxQuestion || options.size() < kMinOptions || options.size() > kMaxOptions) return false;
    std::set<std::string> seen;
    for (const std::string& o : options) {
        const std::size_t n = textLength(o);
        if (n < 1 || n > kMaxOption || !seen.insert(o).second) return false;
    }
    return true;
}

MessageExtras extrasOf(const ChatRecord& r) {
    return MessageExtras{r.replyTo, r.replyPreview, r.forwarded, r.exp, r.storyReply};
}

void checkExtras(const MessageExtras& x) {
    if (!x.replyTo.empty() && !isUuid(x.replyTo)) throw EngineError("bad_reply", "That reply doesn't point at a message.");
    if (textLength(x.replyPreview) > kMaxPreview) throw EngineError("bad_reply", "A reply's preview has to be at most 200 characters.");
    if (x.exp < 0 || x.exp > kMaxExp) throw EngineError("bad_timer", "A disappearing time has to be between 1 second and 4 weeks.");
    if (x.storyReply && !validStoryReply(*x.storyReply)) throw EngineError("bad_story_reply", "That reply doesn't point at a story.");
}

void putExtras(Json& j, const MessageExtras& x) {
    checkExtras(x);
    if (!x.replyTo.empty()) j["reply"] = Json{{"target", x.replyTo}, {"preview", x.replyPreview}};
    if (x.forwarded) j["fwd"] = true;
    if (x.exp > 0) j["exp"] = x.exp;
    if (x.storyReply) j["storyReply"] = storyReplyJson(*x.storyReply);
}

/// Later wins: by the sender's clock, then by id, so every device settles on the same answer whatever order things arrived in.
bool newer(const ChatRecord& a, const ChatRecord& b) {
    return std::tie(a.ts, a.id) > std::tie(b.ts, b.id);
}

bool earlierInChat(const ChatRecord& a, const ChatRecord& b) {
    return std::tie(a.ts, a.arrivedAt, a.id) < std::tie(b.ts, b.arrivedAt, b.id);
}

bool isStale(const api::ApiError& error) {
    return error.code() == "device_list_stale";
}

}  // namespace

// --- Envelope ---

std::size_t textLength(const std::string& utf8) {
    std::size_t n = 0;
    for (const unsigned char c : utf8) {
        if ((c & 0xC0) == 0x80) continue;  // a continuation byte
        n += c >= 0xF0 ? 2 : 1;  // outside the BMP: a surrogate pair
    }
    return n;
}

std::string previewText(const std::string& text) {
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t width = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        const std::size_t cost = c >= 0xF0 ? 2 : 1;
        if (units + cost > kMaxPreview) break;
        units += cost;
        i += width;
    }
    return text.substr(0, std::min(i, text.size()));
}

/// Envelopes fill 256-byte buckets, so their size shows less about what they hold. Spaces after the JSON, which every parser skips.
std::string padded(std::string json) {
    constexpr std::size_t kBucket = 256;
    json.append((json.size() / kBucket + 1) * kBucket - json.size(), ' ');
    return json;
}

std::string encodeEnvelope(const Envelope& e) {
    Json j{{"v", e.v}, {"t", e.t}, {"id", e.id}, {"c", e.c}, {"ts", e.ts}, {"body", e.body}};
    putExtras(j, e.extras);
    return padded(j.dump());
}

std::string encodeOp(const std::string& conversationId, const ChatOp& op, const std::string& id, std::int64_t ts) {
    if (op.t == "edit" && (op.body.empty() || op.body.size() > kMaxText)) throw EngineError("bad_text", "A message has to be between 1 and 16,384 bytes.");
    if (op.t == "reaction" && op.emoji.size() > kMaxEmoji) throw EngineError("bad_emoji", "That reaction is too long.");
    if ((op.t == "vote" || op.t == "end" || op.t == "pin") && !isUuid(op.target)) throw EngineError("bad_target", "That doesn't point at a message.");
    if (op.t == "vote" && !validChoices(op.choices)) throw EngineError("bad_vote", "A vote picks up to 10 different options.");
    if (op.t == "receipt") {
        if (op.targets.empty() || op.targets.size() > kMaxReceiptIds) throw EngineError("bad_receipt", "A receipt covers 1 to 100 messages.");
        for (const std::string& target : op.targets) {
            if (!isUuid(target)) throw EngineError("bad_receipt", "A receipt covers messages by their ids.");
        }
        if (op.status != "delivered" && op.status != "read") throw EngineError("bad_receipt", "A receipt says delivered or read.");
    }
    Json j = opJson(op);
    j["v"] = 1;
    j["id"] = id;
    j["c"] = conversationId;
    j["ts"] = ts;
    return padded(j.dump());
}

std::string encodeFile(const std::string& conversationId, const model::FileDescriptor& file, const std::string& id, std::int64_t ts,
                       const MessageExtras& extras) {
    Json j{{"v", 1}, {"t", "file"}, {"id", id}, {"c", conversationId}, {"ts", ts}, {"file", fileJson(file)}};
    MessageExtras x = extras;
    x.storyReply.reset();  // a story reply is a text
    putExtras(j, x);
    return padded(j.dump());
}

std::string encodePoll(const std::string& conversationId, const std::string& question, const std::vector<std::string>& options, bool multi,
                       const std::string& id, std::int64_t ts, const MessageExtras& extras) {
    if (!validPoll(question, options)) {
        throw EngineError("bad_poll", "A poll needs a question (up to 300 characters) and 2 to 10 different options (up to 100 characters each).");
    }
    Json j{{"v", 1}, {"t", "poll"}, {"id", id}, {"c", conversationId}, {"ts", ts}, {"question", question}, {"options", options}, {"multi", multi}};
    MessageExtras x = extras;
    x.storyReply.reset();
    putExtras(j, x);
    return padded(j.dump());
}

namespace {

/// Part B (docs/signal-integration/09-part-b-calls.md): `d` has the id and conversation checked already. Every field
/// is required: null, absent, the wrong type or a number like 2.0 where an integer belongs and the envelope is ignored.
DecodedEnvelope decodeCallEnvelope(const Json& e, const std::string& t, DecodedEnvelope d, std::int64_t now) {
    const auto ignore = [](const char* reason) {
        DecodedEnvelope out;
        out.reason = reason;
        return out;
    };
    const auto ts = e.find("ts");
    if (ts == e.end() || !ts->is_number_integer()) return ignore("bad_ts");
    const auto sent = ts->is_number_unsigned() && ts->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)
                          ? INT64_MAX
                          : ts->get<std::int64_t>();
    d.ts = std::clamp(sent, now - kMaxPast, now + kMaxFuture);
    const std::string callId = stringOr(e, "callId");
    if (!isUuid(callId)) return ignore("bad_call_id");
    if (t == "call") {
        // Refused when more than 120 s old: an offer replayed later rings nobody.
        if (now - sent > kCallMaxAgeMs) return ignore("stale_call");
        static const std::set<std::string> kSteps = {"offer", "answer", "restart-offer", "restart-answer"};
        CallSignal call;
        call.callId = callId;
        call.step = stringOr(e, "step");
        if (!kSteps.count(call.step)) return ignore("bad_step");
        call.kind = stringOr(e, "kind");
        if (call.kind != "voice" && call.kind != "video") return ignore("bad_kind");
        const auto sdp = e.find("sdp");
        if (sdp == e.end() || !sdp->is_string()) return ignore("bad_sdp");
        const auto& sdpText = sdp->get_ref<const std::string&>();
        // A quick bound in bytes first (4 bytes at most per unit), so a huge string isn't walked.
        if (sdpText.empty() || sdpText.size() > 4 * kCallMaxSdp || textLength(sdpText) > kCallMaxSdp) return ignore("bad_sdp");
        call.sdp = sdpText;
        call.to = stringOr(e, "to");
        if (call.to != "*" && !isUuid(call.to)) return ignore("bad_to");
        d.kind = DecodedEnvelope::Kind::Call;
        d.call = std::move(call);
        return d;
    }
    const auto epoch = e.find("epoch");
    if (epoch == e.end() || !epoch->is_number_integer()) return ignore("bad_epoch");
    if (epoch->is_number_unsigned() && epoch->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) return ignore("bad_epoch");
    if (epoch->get<std::int64_t>() < 1) return ignore("bad_epoch");
    const auto key = e.find("key");
    if (key == e.end() || !key->is_string() || key->get_ref<const std::string&>().size() > 64) return ignore("bad_key");
    crypto::Bytes raw;
    try {
        raw = crypto::base64Decode(key->get_ref<const std::string&>());
    } catch (const std::exception&) {
        return ignore("bad_key");
    }
    if (raw.size() != 32) {
        sodium_memzero(raw.data(), raw.size());
        return ignore("bad_key");
    }
    CallKey callKey;
    callKey.callId = callId;
    callKey.epoch = epoch->get<std::int64_t>();
    callKey.key = std::make_shared<const crypto::SecretBytes>(crypto::SecretBytes::copyOf(raw));
    sodium_memzero(raw.data(), raw.size());
    d.kind = DecodedEnvelope::Kind::CallKey;
    d.callKey = std::move(callKey);
    return d;
}

/// Part D: `d` has the id checked already; the item's conversation must be the story itself. Every field is required:
/// null, absent, the wrong type or a number like 9.5 where an integer belongs and the envelope is ignored.
DecodedEnvelope decodeStoryEnvelope(const Json& e, const std::string& t, DecodedEnvelope d, const std::string& conversationId) {
    const auto ignore = [](const char* reason) {
        DecodedEnvelope out;
        out.reason = reason;
        return out;
    };
    const std::string storyId = stringOr(e, "storyId");
    if (!isUuid(storyId)) return ignore("bad_story_id");
    if (storyId != conversationId || stringOr(e, "c") != storyId) return ignore("wrong_conversation");
    if (t == "storykey") {
        const auto expires = e.find("expiresAt");
        if (expires == e.end() || !expires->is_number_integer()) return ignore("bad_expires");
        if (expires->is_number_unsigned() && expires->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) return ignore("bad_expires");
        if (expires->get<std::int64_t>() < 1) return ignore("bad_expires");
        const auto key = e.find("key");
        if (key == e.end() || !key->is_string() || key->get_ref<const std::string&>().size() > 64) return ignore("bad_key");
        crypto::Bytes raw;
        try {
            raw = crypto::base64Decode(key->get_ref<const std::string&>());
        } catch (const std::exception&) {
            return ignore("bad_key");
        }
        if (raw.size() != crypto::stories::kKeyBytes) {
            sodium_memzero(raw.data(), raw.size());
            return ignore("bad_key");
        }
        StoryKeyEnvelope out;
        out.storyId = storyId;
        out.expiresAt = expires->get<std::int64_t>();
        out.key = std::make_shared<const crypto::SecretBytes>(crypto::SecretBytes::copyOf(raw));
        sodium_memzero(raw.data(), raw.size());
        d.kind = DecodedEnvelope::Kind::StoryKey;
        d.storyKey = std::move(out);
        return d;
    }
    const auto sticker = e.find("stickerId");
    if (sticker == e.end() || !sticker->is_string()) return ignore("bad_sticker");
    const std::string stickerId = sticker->get<std::string>();
    if (stickerId.empty() || stickerId.size() > kMaxStickerId) return ignore("bad_sticker");
    for (const char c : stickerId) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return ignore("bad_sticker");
    }
    const auto rev = e.find("rev");
    if (rev == e.end() || !rev->is_number_integer()) return ignore("bad_rev");
    if (rev->is_number_unsigned() && rev->get<std::uint64_t>() > static_cast<std::uint64_t>(kMaxStoryRev)) return ignore("bad_rev");
    if (rev->get<std::int64_t>() < 1 || rev->get<std::int64_t>() > kMaxStoryRev) return ignore("bad_rev");
    const auto final = e.find("final");
    if (final == e.end() || !final->is_boolean()) return ignore("bad_final");
    const auto payload = e.find("payload");
    if (payload == e.end() || !payload->is_object() || payload->dump().size() > kMaxStoryAnswerPayload) return ignore("bad_payload");
    StoryAnswerEnvelope out;
    out.storyId = storyId;
    out.stickerId = stickerId;
    out.rev = rev->get<std::int64_t>();
    out.final = final->get<bool>();
    out.payload = *payload;
    d.kind = DecodedEnvelope::Kind::StoryAnswer;
    d.storyAnswer = std::move(out);
    return d;
}

}  // namespace

std::string encodeCall(const std::string& conversationId, const CallSignal& signal, const std::string& id, std::int64_t ts) {
    static const std::set<std::string> kSteps = {"offer", "answer", "restart-offer", "restart-answer"};
    if (!isUuid(signal.callId)) throw EngineError("bad_call", "That isn't a call.");
    if (!kSteps.count(signal.step)) throw EngineError("bad_call", "That isn't a step of a call.");
    if (signal.kind != "voice" && signal.kind != "video") throw EngineError("bad_call", "A call is a voice or a video call.");
    if (signal.sdp.empty() || textLength(signal.sdp) > kCallMaxSdp) throw EngineError("bad_call", "The call's setup is too long.");
    if (signal.to != "*" && !isUuid(signal.to)) throw EngineError("bad_call", "A call goes to one device or to every device.");
    const Json j{{"v", 1},           {"t", "call"},           {"id", id},         {"c", conversationId}, {"ts", ts},
                 {"callId", signal.callId}, {"step", signal.step}, {"kind", signal.kind}, {"sdp", signal.sdp}, {"to", signal.to}};
    return padded(j.dump());
}

std::string encodeCallKey(const std::string& conversationId, const CallKey& key, const std::string& id, std::int64_t ts) {
    if (!isUuid(key.callId)) throw EngineError("bad_call", "That isn't a call.");
    if (key.epoch < 1) throw EngineError("bad_call_key", "A call key's epoch starts at 1.");
    if (!key.key || key.key->size() != 32) throw EngineError("bad_call_key", "A call key is 32 bytes.");
    const Json j{{"v", 1},          {"t", "callkey"},       {"id", id},
                 {"c", conversationId}, {"ts", ts},            {"callId", key.callId},
                 {"epoch", key.epoch},  {"key", crypto::base64Encode(key.key->view())}};
    return padded(j.dump());
}

DecodedEnvelope decodeEnvelope(const std::string& plaintext, const std::string& conversationId, std::int64_t nowMs) {
    using Kind = DecodedEnvelope::Kind;
    const auto ignore = [](const char* reason) {
        DecodedEnvelope d;
        d.reason = reason;
        return d;
    };
    const Json e = Json::parse(plaintext, nullptr, false);
    if (e.is_discarded()) return ignore("not_json");
    if (!e.is_object()) return ignore("not_an_envelope");
    const auto v = e.find("v");
    const double version = v != e.end() && v->is_number() ? v->get<double>() : 0;
    if (version != 1) {
        if (version <= 1) return ignore("bad_version");
        DecodedEnvelope d;
        d.kind = Kind::Newer;
        return d;
    }
    static const std::set<std::string> kTypes = {"text", "file", "reaction", "edit", "delete", "poll", "vote", "end", "pin", "receipt",
                                                 "call", "callkey", "storykey", "storyanswer"};
    const std::string t = stringOr(e, "t");
    if (!kTypes.count(t)) return ignore("unknown_type");
    DecodedEnvelope d;
    d.id = stringOr(e, "id");
    if (!isUuid(d.id)) return ignore("bad_id");
    if (conversationId.empty() || stringOr(e, "c") != conversationId) return ignore("wrong_conversation");
    const std::int64_t now = resolveNow(nowMs);
    const auto claimed = e.find("ts");
    const double ts = claimed != e.end() && claimed->is_number() ? claimed->get<double>() : static_cast<double>(now);
    d.ts = static_cast<std::int64_t>(std::clamp(ts, static_cast<double>(now - kMaxPast), static_cast<double>(now + kMaxFuture)));

    if (t == "call" || t == "callkey") return decodeCallEnvelope(e, t, std::move(d), now);
    if (t == "storykey" || t == "storyanswer") return decodeStoryEnvelope(e, t, std::move(d), conversationId);

    const auto text = [&](const char* key, std::size_t max) -> std::optional<std::string> {
        const auto it = e.find(key);
        if (it == e.end() || !it->is_string() || it->get_ref<const std::string&>().size() > max) return std::nullopt;
        return it->get<std::string>();
    };
    // reply, fwd and exp on a text, file or poll: any of them malformed and the whole envelope is ignored.
    const auto extras = [&]() -> bool {
        if (const auto reply = e.find("reply"); reply != e.end()) {
            if (!reply->is_object()) return false;
            const auto target = reply->find("target");
            if (target == reply->end() || !target->is_string() || !isUuid(target->get<std::string>())) return false;
            d.extras.replyTo = target->get<std::string>();
            // The preview is required (it may be empty): a device that never had the original still shows what it answers.
            const auto preview = reply->find("preview");
            if (preview == reply->end() || !preview->is_string() || textLength(preview->get_ref<const std::string&>()) > kMaxPreview) return false;
            d.extras.replyPreview = preview->get<std::string>();
        }
        if (const auto fwd = e.find("fwd"); fwd != e.end()) {
            if (!fwd->is_boolean()) return false;
            d.extras.forwarded = fwd->get<bool>();
        }
        if (const auto exp = e.find("exp"); exp != e.end()) {
            if (!exp->is_number_integer()) return false;
            const auto seconds = exp->get<std::int64_t>();
            if (seconds < 1 || seconds > kMaxExp) return false;
            d.extras.exp = static_cast<int>(seconds);
        }
        return true;
    };
    if (t == "file") {
        const auto file = e.find("file");
        d.file = fileFrom(file != e.end() ? *file : Json(nullptr));
        if (!d.file) return ignore("bad_file");
        if (!extras()) return ignore("bad_extras");
        d.kind = Kind::Message;
        return d;
    }
    if (t == "text") {
        const auto body = text("body", kMaxText);
        if (!body) return ignore("bad_body");
        if (!extras()) return ignore("bad_extras");
        // Part D: a reply or reaction to a story. Malformed and the whole envelope is ignored, like the other extras.
        if (const auto story = e.find("storyReply"); story != e.end()) {
            d.extras.storyReply = storyReplyFrom(*story);
            if (!d.extras.storyReply) return ignore("bad_story_reply");
        }
        d.kind = Kind::Message;
        d.text = *body;
        return d;
    }
    if (t == "poll") {
        const auto question = e.find("question");
        const auto options = e.find("options");
        const auto multi = e.find("multi");
        if (question == e.end() || !question->is_string() || options == e.end() || !options->is_array() || multi == e.end() ||
            !multi->is_boolean()) {
            return ignore("bad_poll");
        }
        ChatPoll poll;
        for (const Json& o : *options) {
            if (!o.is_string()) return ignore("bad_poll");
            poll.options.push_back(o.get<std::string>());
        }
        if (!validPoll(question->get<std::string>(), poll.options)) return ignore("bad_poll");
        poll.multi = multi->get<bool>();
        if (!extras()) return ignore("bad_extras");
        d.kind = Kind::Message;
        d.text = question->get<std::string>();
        d.poll = std::move(poll);
        return d;
    }
    ChatOp op;
    op.t = t;
    if (t == "receipt") {
        const auto targets = e.find("targets");
        if (targets == e.end() || !targets->is_array() || targets->empty() || targets->size() > kMaxReceiptIds) return ignore("bad_targets");
        for (const Json& target : *targets) {
            if (!target.is_string() || !isUuid(target.get<std::string>())) return ignore("bad_targets");
            // The same id twice is merged.
            if (std::find(op.targets.begin(), op.targets.end(), target.get<std::string>()) == op.targets.end()) op.targets.push_back(target.get<std::string>());
        }
        op.status = stringOr(e, "status");
        if (op.status != "delivered" && op.status != "read") return ignore("bad_status");
        d.kind = Kind::Op;
        d.op = std::move(op);
        return d;
    }
    op.target = stringOr(e, "target");
    if (!isUuid(op.target)) return ignore("bad_target");
    if (t == "edit") {
        const auto body = text("body", kMaxText);
        if (!body || body->empty()) return ignore("bad_body");
        op.body = *body;
    } else if (t == "reaction") {
        const auto emoji = text("emoji", kMaxEmoji);
        if (!emoji) return ignore("bad_emoji");
        op.emoji = *emoji;
    } else if (t == "vote") {
        const auto choices = e.find("choices");
        if (choices == e.end() || !choices->is_array()) return ignore("bad_choices");
        for (const Json& c : *choices) {
            if (!c.is_number_integer()) return ignore("bad_choices");
            const auto value = c.get<std::int64_t>();
            if (value < 0 || value > 9) return ignore("bad_choices");
            op.choices.push_back(static_cast<int>(value));
        }
        if (!validChoices(op.choices)) return ignore("bad_choices");
    } else if (t == "pin") {
        const auto pinned = e.find("pinned");
        if (pinned == e.end() || !pinned->is_boolean()) return ignore("bad_pinned");
        op.pinned = pinned->get<bool>();
    }
    d.kind = Kind::Op;
    d.op = std::move(op);
    return d;
}

// --- Fold ---

std::optional<std::int64_t> expiryOf(const ChatRecord& record) {
    if (record.op || record.exp <= 0) return std::nullopt;
    return record.ts + static_cast<std::int64_t>(record.exp) * 1000;
}

std::vector<ChatRecord> foldChat(const std::vector<ChatRecord>& records, std::optional<std::int64_t> nowMs) {
    std::map<std::string, ChatRecord> messages;
    for (const ChatRecord& r : records) {
        if (r.op) continue;
        const auto expires = expiryOf(r);
        if (nowMs && expires && *expires <= *nowMs) continue;  // gone: left out, and every op on it with it
        messages.emplace(r.id, r);
    }
    std::set<std::string> deleted;
    for (const ChatRecord& r : records) {
        if (!r.op || r.op->t != "delete") continue;
        const auto message = messages.find(r.op->target);
        if (message != messages.end() && message->second.from == r.from) deleted.insert(r.op->target);
    }
    std::map<std::string, const ChatRecord*> edits;
    std::map<std::pair<std::string, std::string>, const ChatRecord*> reactions;  // (target, user)
    std::map<std::string, const ChatRecord*> pins;
    std::map<std::pair<std::string, std::string>, bool> receipts;  // (target, user) -> read
    std::map<std::string, std::vector<const ChatRecord*>> votes;  // poll -> its votes
    std::map<std::string, const ChatRecord*> ends;  // poll -> its author's first end
    const auto latest = [](const ChatRecord*& held, const ChatRecord& r) {
        if (!held || newer(r, *held)) held = &r;
    };
    for (const ChatRecord& r : records) {
        if (!r.op) continue;
        if (r.op->t == "receipt") {
            for (const std::string& target : r.op->targets) {
                const auto message = messages.find(target);
                if (message == messages.end() || deleted.count(target) || message->second.from == r.from) continue;
                bool& read = receipts[{target, r.from}];
                read = read || r.op->status == "read";
            }
            continue;
        }
        const auto message = messages.find(r.op->target);
        if (message == messages.end() || deleted.count(r.op->target)) continue;
        const ChatRecord& m = message->second;
        if (r.op->t == "edit" && r.from == m.from && !m.file && !m.poll) {
            latest(edits[r.op->target], r);
        } else if (r.op->t == "reaction") {
            latest(reactions[{r.op->target, r.from}], r);
        } else if (r.op->t == "pin") {
            latest(pins[r.op->target], r);
        } else if (r.op->t == "vote" && m.poll) {
            votes[r.op->target].push_back(&r);
        } else if (r.op->t == "end" && m.poll && r.from == m.from) {
            const ChatRecord*& held = ends[r.op->target];
            if (!held || before(r, *held)) held = &r;
        }
    }
    for (const auto& [target, record] : edits) {
        ChatRecord& message = messages.at(target);
        message.text = record->op->body;
        message.edited = true;
    }
    for (const auto& [key, record] : reactions) {
        if (!record->op->emoji.empty()) messages.at(key.first).reactions.push_back({key.second, record->op->emoji});
    }
    for (const auto& [target, record] : pins) messages.at(target).pinned = record->op->pinned;
    for (const auto& [key, read] : receipts) {
        messages.at(key.first).receipts.push_back({key.second, read ? "read" : "delivered"});
    }
    for (auto& [id, message] : messages) {
        if (!message.poll) continue;
        ChatPoll& poll = *message.poll;
        poll.votes.assign(poll.options.size(), 0);
        poll.ballots.clear();
        const auto end = ends.find(id);
        poll.ended = end != ends.end();
        // Each voter's latest valid vote from before the end; an invalid one leaves the one before it standing.
        std::map<std::string, const ChatRecord*> counted;
        for (const ChatRecord* vote : votes[id]) {
            if (poll.ended && !before(*vote, *end->second)) continue;
            const auto& choices = vote->op->choices;
            const bool fits = std::all_of(choices.begin(), choices.end(), [&](int c) { return c >= 0 && static_cast<std::size_t>(c) < poll.options.size(); });
            if (!fits || (!poll.multi && choices.size() > 1)) continue;
            latest(counted[vote->from], *vote);
        }
        for (const auto& [user, vote] : counted) {
            if (vote->op->choices.empty()) continue;
            for (const int c : vote->op->choices) ++poll.votes[static_cast<std::size_t>(c)];
            poll.ballots.emplace_back(user, vote->op->choices);
        }
    }
    for (auto& [id, message] : messages) message.expiresAt = expiryOf(message);
    for (const std::string& id : deleted) {
        // Nothing of what it said is shown: no text, file, poll, quote or pin.
        ChatRecord& message = messages.at(id);
        message.text.clear();
        message.file.reset();
        message.poll.reset();
        message.replyTo.clear();
        message.replyPreview.clear();
        message.forwarded = false;
        message.pinned = false;
        message.deleted = true;
    }
    std::vector<ChatRecord> out;
    out.reserve(messages.size());
    for (auto& [id, message] : messages) out.push_back(std::move(message));
    std::sort(out.begin(), out.end(), earlierInChat);
    return out;
}

// --- History ---

ChatHistory::ChatHistory(std::filesystem::path file, crypto::Bytes key) : file_(std::move(file)), key_(std::move(key)) {
    crypto::ensureSodium();
    load();
}

void ChatHistory::load() {
    const auto blob = fsutil::readAll(file_);
    if (!blob || blob->size() <= crypto_aead_xchacha20poly1305_ietf_NPUBBYTES) return;
    const auto* data = reinterpret_cast<const unsigned char*>(blob->data());
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    std::string plain(blob->size() - nonceLen, '\0');
    unsigned long long plainLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()), &plainLen, nullptr, data + nonceLen,
                                                   blob->size() - nonceLen, nullptr, 0, data, key_.data()) != 0) {
        return;  // another key (the device was replaced): the history is unreadable, so it starts empty
    }
    plain.resize(plainLen);
    const Json j = Json::parse(plain, nullptr, false);
    if (!j.is_object()) return;
    for (const Json& r : j.value("records", Json::array())) records_.push_back(recordFrom(r));
    for (const Json& p : j.value("pins", Json::array())) {
        if (p.is_string()) pins_.push_back(p.get<std::string>());
    }
    const Json joined = j.value("joined", Json::object());
    if (joined.is_object()) {
        for (auto it = joined.begin(); it != joined.end(); ++it) {
            if (it.value().is_number_integer()) joined_[it.key()] = it.value().get<std::int64_t>();
        }
    }
}

void ChatHistory::save() const {
    Json records = Json::array();
    for (const ChatRecord& r : records_) records.push_back(recordJson(r));
    Json j{{"records", records}, {"pins", pins_}};
    if (!joined_.empty()) j["joined"] = joined_;
    const std::string plain = j.dump();
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    std::string blob(nonceLen + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    randombytes_buf(blob.data(), nonceLen);
    unsigned long long cipherLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(blob.data()) + nonceLen, &cipherLen,
                                               reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nullptr, 0, nullptr,
                                               reinterpret_cast<const unsigned char*>(blob.data()), key_.data());
    blob.resize(nonceLen + cipherLen);
    fsutil::writeAtomic(file_, blob);
}

bool ChatHistory::add(const ChatRecord& record) {
    std::lock_guard lock(mutex_);
    for (const ChatRecord& r : records_) {
        if (r.conversationId == record.conversationId && r.id == record.id) return false;
    }
    records_.push_back(record);
    eraseGone(record.conversationId);  // before the write, so a deleted or expired message's text is never put on disk again
    save();
    return true;
}

void ChatHistory::setClock(std::function<std::int64_t()> clock) {
    std::lock_guard lock(mutex_);
    clock_ = std::move(clock);
}

std::int64_t ChatHistory::now() const {
    return clock_ ? clock_() : util::nowMillis();
}

bool ChatHistory::eraseGone(const std::string& conversationId) {
    const std::int64_t at = now();
    std::set<std::string> gone;
    std::set<std::string> expired;
    for (const ChatRecord& r : records_) {
        if (r.conversationId != conversationId || r.op) continue;
        const auto expires = expiryOf(r);
        if (at > 0 && expires && *expires <= at) {
            gone.insert(r.id);
            expired.insert(r.id);
        }
    }
    for (const ChatRecord& r : records_) {
        if (r.conversationId != conversationId || !r.op || r.op->t != "delete") continue;
        for (const ChatRecord& message : records_) {
            if (message.conversationId == conversationId && !message.op && message.id == r.op->target && message.from == r.from) gone.insert(message.id);
        }
    }
    if (gone.empty()) return false;
    const std::size_t before = records_.size();
    // An own message that disappeared before it could leave never goes: nothing was sent, nothing to keep.
    std::erase_if(records_, [&](const ChatRecord& r) {
        return r.conversationId == conversationId && !r.op && r.status == "queued" && expired.count(r.id) > 0;
    });
    // The ops that refer to them go too; a receipt keeps only the messages that are still there.
    for (ChatRecord& r : records_) {
        if (r.conversationId != conversationId || !r.op || r.op->t != "receipt") continue;
        std::erase_if(r.op->targets, [&](const std::string& target) { return gone.count(target) > 0; });
    }
    std::erase_if(records_, [&](const ChatRecord& r) {
        if (r.conversationId != conversationId || !r.op) return false;
        if (r.op->t == "receipt") return r.op->targets.empty();
        return r.op->t != "delete" && gone.count(r.op->target) > 0;
    });
    bool changed = records_.size() != before;
    for (ChatRecord& r : records_) {
        if (r.conversationId != conversationId || r.op || !gone.count(r.id)) continue;
        if (r.text.empty() && !r.file && !r.poll && r.replyPreview.empty()) continue;
        r.text.clear();
        r.file.reset();
        r.poll.reset();
        r.replyPreview.clear();
        changed = true;
    }
    return changed;
}

bool ChatHistory::eraseExpired() {
    std::lock_guard lock(mutex_);
    std::set<std::string> conversations;
    for (const ChatRecord& r : records_) conversations.insert(r.conversationId);
    bool changed = false;
    for (const std::string& c : conversations) changed = eraseGone(c) || changed;
    if (changed) save();
    return changed;
}

std::optional<std::int64_t> ChatHistory::nextExpiry() const {
    std::lock_guard lock(mutex_);
    const std::int64_t at = now();
    std::optional<std::int64_t> next;
    for (const ChatRecord& r : records_) {
        const auto expires = expiryOf(r);
        if (!expires) continue;
        const bool erased = r.text.empty() && !r.file && !r.poll;
        if (erased && *expires <= at) continue;
        if (!next || *expires < *next) next = expires;
    }
    return next;
}

bool ChatHistory::receiptSent(const std::string& conversationId, const std::string& messageId, const std::string& selfUserId,
                              const std::string& status) const {
    std::lock_guard lock(mutex_);
    for (const ChatRecord& r : records_) {
        if (r.conversationId != conversationId || !r.op || r.op->t != "receipt" || r.from != selfUserId) continue;
        if (status == "read" && r.op->status != "read") continue;
        if (std::find(r.op->targets.begin(), r.op->targets.end(), messageId) != r.op->targets.end()) return true;
    }
    return false;
}

bool ChatHistory::setStatus(const std::string& conversationId, const std::string& id, const std::string& status) {
    std::lock_guard lock(mutex_);
    for (ChatRecord& r : records_) {
        if (r.conversationId == conversationId && r.id == id) {
            r.status = status;
            save();
            return true;
        }
    }
    return false;
}

bool ChatHistory::remove(const std::string& conversationId, const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(records_.begin(), records_.end(), [&](const ChatRecord& r) { return r.conversationId == conversationId && r.id == id; });
    if (it == records_.end()) return false;
    records_.erase(it);
    save();
    return true;
}

std::vector<ChatRecord> ChatHistory::records(const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    std::vector<ChatRecord> out;
    for (const ChatRecord& r : records_) {
        if (r.conversationId == conversationId) out.push_back(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const ChatRecord& a, const ChatRecord& b) {
        return a.ts != b.ts ? a.ts < b.ts : a.arrivedAt < b.arrivedAt;
    });
    return out;
}

std::vector<ChatRecord> ChatHistory::queued() const {
    std::lock_guard lock(mutex_);
    std::vector<ChatRecord> out;
    for (const ChatRecord& r : records_) {
        if (r.status == "queued") out.push_back(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const ChatRecord& a, const ChatRecord& b) {
        return a.arrivedAt != b.arrivedAt ? a.arrivedAt < b.arrivedAt : a.ts < b.ts;
    });
    return out;
}

std::vector<ChatRecord> ChatHistory::list(const std::string& conversationId) const {
    std::int64_t at = 0;
    {
        std::lock_guard lock(mutex_);
        at = now();
    }
    return foldChat(records(conversationId), at > 0 ? std::optional<std::int64_t>(at) : std::nullopt);
}

void ChatHistory::markJoined(const std::string& conversationId, std::int64_t atMs) {
    std::lock_guard lock(mutex_);
    if (joined_.count(conversationId)) return;
    joined_[conversationId] = atMs;
    save();
}

std::optional<std::int64_t> ChatHistory::joinedAt(const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    const auto it = joined_.find(conversationId);
    if (it == joined_.end()) return std::nullopt;
    return it->second;
}

bool ChatHistory::pinned(const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    return std::find(pins_.begin(), pins_.end(), conversationId) != pins_.end();
}

void ChatHistory::pin(const std::string& conversationId) {
    std::lock_guard lock(mutex_);
    if (std::find(pins_.begin(), pins_.end(), conversationId) != pins_.end()) return;
    pins_.push_back(conversationId);
    save();
}

void ChatHistory::unpin(const std::string& conversationId) {
    std::lock_guard lock(mutex_);
    pins_.erase(std::remove(pins_.begin(), pins_.end(), conversationId), pins_.end());
    save();
}

// --- Sending ---

namespace {

ChatRecord outgoing(const V3Device& device, const std::string& conversationId, std::int64_t now) {
    ChatRecord r;
    r.conversationId = conversationId;
    r.id = util::randomUuid();
    r.from = device.userId();
    r.fromDeviceId = device.deviceId();
    r.ts = now;
    r.arrivedAt = now;
    r.direction = "out";
    r.status = "sent";
    return r;
}

Envelope newEnvelope(const ChatRecord& record, const std::string& text) {
    if (text.empty() || text.size() > kMaxText) throw EngineError("bad_text", "A message has to be between 1 and 16,384 bytes.");
    Envelope e;
    e.id = record.id;
    e.c = record.conversationId;
    e.ts = record.ts;
    e.body = text;
    return e;
}

/// Every listed device of every active member, from lists the engine verified. Includes this account's own devices.
Json verifiedMemberDevices(V3Device& device, const std::string& conversationId) {
    const Json view = device.server().conversationDeviceLists(conversationId);
    Json devices = Json::array();
    for (const Json& member : view.value("members", Json::array())) {
        const auto list = member.find("deviceList");
        if (list == member.end() || list->is_null()) continue;
        const Json checked = device.op("verifyDeviceList", Json{{"deviceList", *list}});
        if (!checked.value("ok", false)) {
            throw EngineError(stringOr(checked, "error", "bad_device_list"), stringOr(checked, "message", "a member's device list doesn't verify"));
        }
        for (const Json& d : checked.value("devices", Json::array())) devices.push_back(d);
    }
    return devices;
}

/// One Olm message for the room key to `recipient`, claiming a one-time key when there is no session yet.
Json shareKey(V3Device& device, const std::string& conversationId, const Json& recipient) {
    Json answer = device.op("shareRoomKey", Json{{"groupId", conversationId}, {"recipient", recipient}});
    if (!answer.value("ok", false) && stringOr(answer, "error") == "no_session") {
        const Json claimed = device.server().claimKeys(conversationId, Json::array({Json{{"userId", recipient.at("userId")}, {"deviceId", recipient.at("deviceId")}}}));
        const Json& c = claimed.at(0);
        const Json key = c.contains("oneTimeKey") && !c.at("oneTimeKey").is_null() ? c.at("oneTimeKey") : c.value("fallbackKey", Json(nullptr));
        Json fields = {{"groupId", conversationId}, {"recipient", recipient}};
        if (!key.is_null()) fields["oneTimeKey"] = key;
        answer = device.op("shareRoomKey", fields);
    }
    return requireOk(answer, "shareRoomKey");
}

}  // namespace

namespace {

/// The group's signed roster (M6): verifies the stored chain and returns the latest verify answer,
/// or null when the group has none yet.
Json verifyRosters(V3Device& device, const std::string& conversationId) {
    const Json chain = device.server().rosters(conversationId, 0);
    if (!chain.is_array() || chain.empty()) return nullptr;
    const Json fields = {{"groupId", conversationId}, {"chain", chain}};
    Json answer = device.op("rosterVerify", fields);
    if (!answer.value("ok", false) && answer.value("error", std::string()) == "unknown_issuer") {
        // A roster signed by someone whose list this device hasn't checked yet (a community's owner or staff, who
        // may never have written here): verify each issuer's list, then the chain once more.
        std::set<std::string> issuers;
        for (const Json& r : chain) {
            if (r.is_object() && r.contains("issuer") && r.at("issuer").is_string()) issuers.insert(r.at("issuer").get<std::string>());
        }
        for (const std::string& issuer : issuers) {
            if (issuer != device.userId()) verifiedDevices(device, issuer, conversationId);
        }
        answer = device.op("rosterVerify", fields);
    }
    return requireOk(std::move(answer), "rosterVerify");
}

/// Makes the roster match the conversation's active members, signing the first or the next epoch
/// when it doesn't. Another member's concurrent change wins; this device verifies it and tries once more.
void syncRoster(V3Device& device, const std::string& conversationId, std::vector<std::string> members) {
    std::sort(members.begin(), members.end());
    for (int attempt = 0; attempt < 2; ++attempt) {
        const Json verified = verifyRosters(device, conversationId);
        Json made;
        if (verified.is_null()) {
            made = device.must("rosterCreate", Json{{"groupId", conversationId}, {"members", members}});
        } else if (verified.value("members", Json::array()).get<std::vector<std::string>>() != members) {
            made = device.must("rosterUpdate", Json{{"groupId", conversationId}, {"members", members}});
        } else {
            return;
        }
        try {
            device.server().postRoster(conversationId, made.at("roster"));
            return;
        } catch (const api::ApiError& error) {
            if (error.code() != "stale_roster" || attempt == 1) throw;
        }
    }
}

/// B1: a group goes over Signal only when this device and every other member device speak it.
bool groupUsesSignal(V3Device& device, const Json& members) {
    if (!engineHasSignal()) return false;
    Json others = Json::array();
    for (const Json& d : members) {
        if (d.value("deviceId", std::string()) != device.deviceId()) others.push_back(Json{{"identity", d}});
    }
    if (others.empty()) return false;
    const Json answer = device.op("chatProtocols", Json{{"recipients", others}});
    if (!answer.value("ok", false) || !answer.value("thisDeviceSignal", false)) return false;
    for (const Json& p : answer.value("protocols", Json::array())) {
        if (p.value("protocol", std::string()) != "rv-signal-v1") return false;
    }
    return true;
}

/// A Signal group send (M6): roster in line, sender key to devices that lack it, then one fan-out.
int sendToSignalGroup(V3Device& device, const std::string& conversationId, const std::string& plaintext, const Json& members) {
    std::vector<std::string> accounts;
    for (const Json& d : members) {
        const std::string user = d.value("userId", std::string());
        if (std::find(accounts.begin(), accounts.end(), user) == accounts.end()) accounts.push_back(user);
    }
    syncRoster(device, conversationId, accounts);
    Json recipients = Json::array();
    std::vector<Json> others;
    for (const Json& d : members) {
        if (d.value("deviceId", std::string()) == device.deviceId()) continue;
        others.push_back(d);
        recipients.push_back(Json{{"identity", d}});
    }
    Json shared = device.must("groupSignalShare", Json{{"groupId", conversationId}, {"recipients", recipients}});
    // Devices without a Signal session yet: claim their bundles and share once more.
    Json ask = Json::array();
    std::vector<std::size_t> need;
    for (std::size_t i = 0; i < others.size(); ++i) {
        if (shared.at("results").at(i).value("error", std::string()) == "no_signal_session") {
            need.push_back(i);
            ask.push_back({{"userId", others[i].at("userId")}, {"deviceId", others[i].at("deviceId")}});
        }
    }
    Json results = shared.at("results");
    if (!need.empty()) {
        const Json claimed = detail::claimInBatches(ask, [&](const Json& b) { return device.server().signalClaim(conversationId, b); });
        Json retry = Json::array();
        for (std::size_t k = 0; k < need.size(); ++k) {
            const Json& c = claimed.at(k);
            retry.push_back(Json{{"identity", others[need[k]]},
                                 {"signalBundle", Json{{"certificate", c.value("certificate", Json(nullptr))}, {"bundle", c.value("bundle", Json(nullptr))}}}});
        }
        const Json again = device.must("groupSignalShare", Json{{"groupId", conversationId}, {"recipients", retry}});
        for (std::size_t k = 0; k < need.size(); ++k) results[need[k]] = again.at("results").at(k);
    }
    Json keyMessages = Json::array();
    for (std::size_t i = 0; i < others.size(); ++i) {
        const Json& r = results.at(i);
        if (r.contains("message")) {
            keyMessages.push_back({{"userId", others[i].at("userId")}, {"deviceId", others[i].at("deviceId")}, {"body", r.at("message").dump()}});
        }
    }
    // Part C2: at most 100 devices per request, in order (sender keys first, then the one message).
    if (!keyMessages.empty()) {
        detail::sendInBatches(keyMessages, [&](const Json& b) { device.server().sendMessages(device.deviceId(), conversationId, b); });
    }
    const Json sealed = device.must("groupSignalEncrypt", Json{{"groupId", conversationId}, {"plaintext", plaintext}});
    Json fanout = Json::array();
    for (const Json& d : others) fanout.push_back({{"userId", d.at("userId")}, {"deviceId", d.at("deviceId")}});
    const std::string body = sealed.at("message").dump();
    // Every member device but this one, from lists just verified: marked allDevices (gap 1), split by account.
    detail::sendInBatches(
        fanout, [&](const Json& b) { device.server().sendGroupMessage(device.deviceId(), conversationId, body, b, true); }, false, nullptr,
        true);
    return static_cast<int>(fanout.size());
}

/// Whether an envelope is a reaction, edit, delete, vote, end, pin or receipt (not a text, file or poll).
bool isOpEnvelope(const std::string& plaintext) {
    const Json j = Json::parse(plaintext, nullptr, false);
    const std::string t = j.is_object() ? stringOr(j, "t") : std::string();
    return t != "text" && t != "file" && t != "poll";
}

/// One group send: membership from verified lists, room keys to devices that lack them, then one fan-out.
int sendToGroup(V3Device& device, const std::string& conversationId, const std::string& plaintext) {
    // Part C2: a community has its own rules (who is in the roster, who gets each message, who issues rosters).
    if (const auto community = communityOf(device, conversationId)) return sendToCommunity(device, *community, plaintext, isOpEnvelope(plaintext));
    const Json members = verifiedMemberDevices(device, conversationId);
    if (groupUsesSignal(device, members)) return sendToSignalGroup(device, conversationId, plaintext, members);
    device.must("groupSetMembers", Json{{"groupId", conversationId}, {"members", members}});
    const Json session = device.must("groupSession", Json{{"groupId", conversationId}});
    // The session key reaches each member device inside Olm, before the group message.
    Json keyMessages = Json::array();
    for (const Json& recipient : session.value("unshared", Json::array())) {
        const Json shared = shareKey(device, conversationId, recipient);
        keyMessages.push_back({{"userId", recipient.at("userId")}, {"deviceId", recipient.at("deviceId")}, {"body", shared.at("message").dump()}});
    }
    if (!keyMessages.empty()) {
        detail::sendInBatches(keyMessages, [&](const Json& b) { device.server().sendMessages(device.deviceId(), conversationId, b); });
    }
    const Json sealed = device.must("groupEncrypt", Json{{"groupId", conversationId}, {"plaintext", plaintext}});
    const Json body = {{"v", 1}, {"kind", "group"}, {"groupId", conversationId}, {"sessionId", sealed.at("sessionId")}, {"ciphertext", sealed.at("ciphertext")}};
    Json recipients = Json::array();
    for (const Json& d : members) {
        if (d.value("deviceId", std::string()) == device.deviceId()) continue;
        recipients.push_back({{"userId", d.at("userId")}, {"deviceId", d.at("deviceId")}});
    }
    const std::string sealedBody = body.dump();
    // Every member device but this one (room keys above went only to those without it): marked, split by account.
    detail::sendInBatches(
        recipients, [&](const Json& b) { device.server().sendGroupMessage(device.deviceId(), conversationId, sealedBody, b, true); }, false,
        nullptr, true);
    return static_cast<int>(recipients.size());
}

/// An empty `contactUserId` means a group. `record` goes into the history as sent, or as failed when `keepFailed`.
/// Tor down on the way (docs/tor-by-default.md): "queued" instead, and what the engine already encrypted waits in the
/// server's SendQueue. `fromQueue`: `record` is a queued one already in the history, whose status changes in place.
SendOutcome deliver(V3Device& device, ChatHistory& history, ChatRecord record, const std::string& plaintext, const std::string& contactUserId,
                    bool keepFailed = true, bool fromQueue = false) {
    const std::string conversationId = record.conversationId;
    // How it ended up: a status, or "" for not kept (a failed op).
    const auto keep = [&](const std::string& status) {
        if (fromQueue) {
            if (status.empty()) {
                history.remove(conversationId, record.id);
            } else {
                history.setStatus(conversationId, record.id, status);
            }
        } else if (!status.empty()) {
            record.status = status;
            if (status != "queued") record.to.clear();
            history.add(record);
        }
    };
    const auto waitForTor = [&] {
        if (!fromQueue) record.to = contactUserId;
        keep("queued");
        history.pin(conversationId);
    };
    int unreachable = 0;
    int delivered = 0;
    try {
        // Only this delivery's requests can be queued, and each knows its record (send_queue.hpp).
        SendScope scope(record.id, contactUserId.empty() ? "group" : "messages");
        // A chat that uses device keys never falls back silently: no keys on their side means no send (9.8).
        if (!contactUserId.empty() && verifiedDevices(device, contactUserId, conversationId).devices.empty()) {
            throw EngineError("contact_has_no_device_keys", "They haven't set up device keys yet. Use the older protocol for this chat.");
        }
        const auto attempt = [&] {
            return contactUserId.empty() ? sendToGroup(device, conversationId, plaintext)
                                         : sendText(device, conversationId, contactUserId, plaintext, &unreachable);
        };
        try {
            unreachable = 0;
            delivered = attempt();
        } catch (const api::ApiError& error) {
            if (!isStale(error)) throw;
            unreachable = 0;
            delivered = attempt();  // a list changed: refetch, verify, once more
        }
    } catch (const SendQueued&) {
        waitForTor();
        return SendOutcome{record.id, 0, unreachable, true};
    } catch (const api::ApiError& error) {
        // Refused at the Tor gate before anything left (a device list, a key claim): nothing encrypted is lost.
        if (error.code() == "tor_unavailable") {
            waitForTor();
            return SendOutcome{record.id, 0, unreachable, true};
        }
        // A queued one stays queued while the connection or the server is down (sendQueuedChat stops there, in order).
        keep(fromQueue && waitsForConnection(error) ? "queued" : keepFailed ? "failed" : "");
        throw;
    } catch (const EngineError& error) {
        // Refused before a message was made: nothing to retry (a queued one has to say it failed, though).
        if (error.code() == "contact_has_no_device_keys" && !fromQueue) throw;
        keep(keepFailed ? "failed" : "");
        throw;
    } catch (...) {
        keep(keepFailed ? "failed" : "");
        throw;
    }
    keep("sent");
    history.pin(conversationId);
    return SendOutcome{record.id, delivered, unreachable};
}

/// What a record says, as the envelope it travels in (the same id and time as when it was made).
std::string plaintextOf(const ChatRecord& record) {
    if (record.op) return encodeOp(record.conversationId, *record.op, record.id, record.ts);
    if (record.poll) {
        return encodePoll(record.conversationId, record.text, record.poll->options, record.poll->multi, record.id, record.ts, extrasOf(record));
    }
    if (record.file) return encodeFile(record.conversationId, *record.file, record.id, record.ts, extrasOf(record));
    Envelope e = newEnvelope(record, record.text);
    e.extras = extrasOf(record);
    return encodeEnvelope(e);
}

ChatRecord messageRecord(const V3Device& device, const std::string& conversationId, std::int64_t now, const OutgoingMessage& message) {
    ChatRecord record = outgoing(device, conversationId, now);
    record.text = message.text;
    record.file = message.file;
    if (message.poll) record.poll = ChatPoll{message.poll->options, message.poll->multi, {}, false, {}};
    record.replyTo = message.extras.replyTo;
    record.replyPreview = message.extras.replyPreview;
    record.forwarded = message.extras.forwarded;
    record.exp = message.extras.exp;
    // Part D: only a text replies to a story.
    if (!message.file && !message.poll) record.storyReply = message.extras.storyReply;
    return record;
}

ChatOp receiptOp(const std::vector<std::string>& messageIds, const std::string& status) {
    ChatOp op;
    op.t = "receipt";
    op.targets = messageIds;
    op.status = status;
    return op;
}

ChatRecord queueRecord(ChatHistory& history, ChatRecord record, const std::string& contactUserId) {
    record.status = "queued";
    record.to = contactUserId;
    plaintextOf(record);  // the same checks as a send now (an empty or too long text is refused here, not later)
    history.add(record);
    history.pin(record.conversationId);
    return record;
}

}  // namespace

SendOutcome sendChat(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                     const std::string& text, std::int64_t nowMs) {
    ChatRecord record = outgoing(device, conversationId, resolveNow(nowMs));
    record.text = text;
    return deliver(device, history, record, encodeEnvelope(newEnvelope(record, text)), contactUserId);
}

SendOutcome sendGroupChat(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& text, std::int64_t nowMs) {
    ChatRecord record = outgoing(device, conversationId, resolveNow(nowMs));
    record.text = text;
    return deliver(device, history, record, encodeEnvelope(newEnvelope(record, text)), {});
}

SendOutcome sendChatFile(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                         const model::FileDescriptor& file, std::int64_t nowMs) {
    ChatRecord record = outgoing(device, conversationId, resolveNow(nowMs));
    record.file = file;
    return deliver(device, history, record, encodeFile(conversationId, file, record.id, record.ts), contactUserId);
}

SendOutcome sendChatOp(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                       const ChatOp& op, std::int64_t nowMs) {
    ChatRecord record = outgoing(device, conversationId, resolveNow(nowMs));
    record.op = op;
    return deliver(device, history, record, encodeOp(conversationId, op, record.id, record.ts), contactUserId, false);
}

SendOutcome sendChatMessage(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                            const OutgoingMessage& message, std::int64_t nowMs) {
    const ChatRecord record = messageRecord(device, conversationId, resolveNow(nowMs), message);
    return deliver(device, history, record, plaintextOf(record), contactUserId);
}

SendOutcome forwardChatFile(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                            const model::FileDescriptor& original,
                            const std::function<model::FileDescriptor(const model::FileDescriptor&)>& reupload, int exp, bool queue,
                            std::int64_t nowMs) {
    const model::FileDescriptor fresh = reupload(original);
    // A descriptor names its object and holds its key: one in another chat would hand that chat the original.
    if (fresh.objectId == original.objectId || fresh.fileKey == original.fileKey) {
        throw EngineError("descriptor_reused", "A forwarded file has to be encrypted and uploaded again.");
    }
    OutgoingMessage message;
    message.file = fresh;
    message.extras.forwarded = true;
    message.extras.exp = exp;
    if (queue) {
        const ChatRecord record = queueChatMessage(device, history, conversationId, contactUserId, message, nowMs);
        return SendOutcome{record.id, 0, 0, true};
    }
    return sendChatMessage(device, history, conversationId, contactUserId, message, nowMs);
}

SendOutcome sendReceipt(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& authorUserId,
                        const std::vector<std::string>& messageIds, const std::string& status, std::int64_t nowMs) {
    if (authorUserId.empty() || authorUserId == device.userId()) throw EngineError("bad_receipt", "A receipt goes to the person who wrote the message.");
    // Pairwise to the author (and this account's other devices), in a group too: nobody else learns who read what.
    return sendChatOp(device, history, conversationId, authorUserId, receiptOp(messageIds, status), nowMs);
}

ChatRecord queueReceipt(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& authorUserId,
                        const std::vector<std::string>& messageIds, const std::string& status, std::int64_t nowMs) {
    if (authorUserId.empty() || authorUserId == device.userId()) throw EngineError("bad_receipt", "A receipt goes to the person who wrote the message.");
    return queueChatOp(device, history, conversationId, authorUserId, receiptOp(messageIds, status), nowMs);
}

// --- Receipts ---

void ReceiptBatcher::add(const std::string& conversationId, const std::string& authorUserId, const std::string& messageId,
                         const std::string& status) {
    if (status != "delivered" && status != "read") return;
    auto& list = pending_[conversationId];
    for (Pending& p : list) {
        if (p.messageId != messageId) continue;
        if (status == "read") p.status = "read";  // read covers delivered
        return;
    }
    list.push_back(Pending{messageId, authorUserId, status});
}

std::vector<ReceiptBatcher::Batch> ReceiptBatcher::due(std::int64_t nowMs) {
    std::vector<Batch> out;
    for (auto it = pending_.begin(); it != pending_.end();) {
        const auto last = lastSent_.find(it->first);
        if (last != lastSent_.end() && nowMs - last->second < kIntervalMs) {
            ++it;
            continue;
        }
        std::vector<Batch> batches;
        std::vector<Pending> left;
        for (const char* status : {"read", "delivered"}) {
            for (const Pending& p : it->second) {
                if (p.status != status) continue;
                auto batch = std::find_if(batches.begin(), batches.end(), [&](const Batch& b) { return b.authorUserId == p.authorUserId && b.status == status; });
                if (batch == batches.end()) {
                    batches.push_back(Batch{it->first, p.authorUserId, status, {}});
                    batch = std::prev(batches.end());
                }
                if (batch->ids.size() < kMaxIds) {
                    batch->ids.push_back(p.messageId);
                } else {
                    left.push_back(p);
                }
            }
        }
        lastSent_[it->first] = nowMs;
        for (Batch& b : batches) out.push_back(std::move(b));
        if (left.empty()) {
            it = pending_.erase(it);
        } else {
            it->second = std::move(left);
            ++it;
        }
    }
    return out;
}

std::optional<std::int64_t> ReceiptBatcher::nextDue() const {
    std::optional<std::int64_t> next;
    for (const auto& [conversation, list] : pending_) {
        const auto last = lastSent_.find(conversation);
        const std::int64_t at = last == lastSent_.end() ? 0 : last->second + kIntervalMs;
        if (!next || at < *next) next = at;
    }
    return next;
}

// --- Waiting for Tor ---

ChatRecord queueChatMessage(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                            const OutgoingMessage& message, std::int64_t nowMs) {
    return queueRecord(history, messageRecord(device, conversationId, resolveNow(nowMs), message), contactUserId);
}

ChatRecord queueChat(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                     const std::string& text, std::int64_t nowMs) {
    ChatRecord record = outgoing(device, conversationId, resolveNow(nowMs));
    record.text = text;
    return queueRecord(history, record, contactUserId);
}

ChatRecord queueChatFile(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                         const model::FileDescriptor& file, std::int64_t nowMs) {
    ChatRecord record = outgoing(device, conversationId, resolveNow(nowMs));
    record.file = file;
    return queueRecord(history, record, contactUserId);
}

ChatRecord queueChatOp(V3Device& device, ChatHistory& history, const std::string& conversationId, const std::string& contactUserId,
                       const ChatOp& op, std::int64_t nowMs) {
    ChatRecord record = outgoing(device, conversationId, resolveNow(nowMs));
    record.op = op;
    return queueRecord(history, record, contactUserId);
}

bool hasQueuedChat(const ChatHistory& history, const QueueingV3Server& server) {
    const auto queue = server.queue();
    return (queue && !queue->empty()) || !history.queued().empty();
}

QueueDrain sendQueuedChat(V3Device& device, ChatHistory& history, QueueingV3Server& server) {
    QueueDrain out;
    // First what the engine already encrypted, exactly as it was.
    const auto flushed = server.flush();
    for (const auto& [conversationId, id] : flushed.sent) {
        if (history.setStatus(conversationId, id, "sent")) ++out.sent;
    }
    for (const auto& [conversationId, id] : flushed.refused) {
        // Its message can't be delivered as encrypted; a message shows as failed (Retry encrypts it afresh), an op goes.
        for (const ChatRecord& r : history.records(conversationId)) {
            if (r.id != id || r.status != "queued") continue;
            if (r.op) {
                history.remove(conversationId, id);
            } else {
                history.setStatus(conversationId, id, "failed");
            }
            ++out.failed;
        }
    }
    if (flushed.waiting) {
        out.waiting = true;
        return out;
    }
    // Then what waited before it was encrypted: encrypted now, once, oldest first.
    for (const ChatRecord& record : history.queued()) {
        std::string plaintext;
        try {
            plaintext = plaintextOf(record);
        } catch (const std::exception&) {
            record.op ? (void)history.remove(record.conversationId, record.id) : (void)history.setStatus(record.conversationId, record.id, "failed");
            ++out.failed;
            continue;
        }
        try {
            const auto sent = deliver(device, history, record, plaintext, record.to, !record.op.has_value(), true);
            if (sent.queued) {
                out.waiting = true;
                return out;
            }
            ++out.sent;
        } catch (const api::ApiError& error) {
            // Down again: it stays queued (deliver kept it so), and so does everything after it.
            if (waitsForConnection(error)) {
                out.waiting = true;
                return out;
            }
            ++out.failed;
        } catch (const std::exception&) {
            ++out.failed;  // shown as failed (or a failed op dropped), as a send made now would be
        }
    }
    return out;
}

// --- Receiving ---

namespace {

/// A Signal error from a pairwise message that a new session may cure (gap 2): the session fell out of step.
bool brokenSession(const std::string& code) {
    return code == "decrypt_failed" || code == "no_signal_session" || code == "unknown_prekey";
}

/**
 * Gap 2 (docs/signal-integration/13-every-device-and-session-reset.md): sets this device's session with `peer` aside
 * and sends that one device a refresh, which starts a new session it switches to. At most once per peer device every
 * ten minutes (the engine answers reset: false otherwise). Unmarked and never queued. Throws on any failure.
 */
void healSession(V3Device& device, const Json& peer, const std::string& conversationId) {
    const Json reset = device.must("signalResetSession", Json{{"peer", peer}});
    if (!reset.value("reset", false)) return;
    const Json address = {{"userId", peer.at("userId")}, {"deviceId", peer.at("deviceId")}};
    const Json claimed = device.server().signalClaim(conversationId, Json::array({address}));
    if (!claimed.is_array() || claimed.empty()) return;
    const Json& c = claimed.at(0);
    const Json refreshed = device.must(
        "signalRefresh",
        Json{{"peer", peer}, {"claimed", Json{{"certificate", c.value("certificate", Json(nullptr))}, {"bundle", c.value("bundle", Json(nullptr))}}}});
    NoQueueScope unqueued;
    Json message = address;
    message["body"] = refreshed.at("message").dump();
    device.server().sendMessages(device.deviceId(), conversationId, Json::array({message}));
}

}  // namespace

std::vector<ChatRecord> receiveChat(V3Device& device, ChatHistory& history, std::int64_t nowMs, ReceiveStats* stats,
                                    std::vector<CallMessage>* calls, StoryStore* stories) {
    // Errors about the sender's list, not the message: the item waits until the person accepts the change.
    // rv-signal-v1: a device's Signal identity changed or doesn't match its signed list. Held, never dropped,
    // so the person can compare safety numbers first (docs/signal-integration/, M5).
    static const std::set<std::string> hold = {"account_key_changed", "rollback", "equivocation", "uncertified_account_key", "bad_device_list",
                                               "identity_changed", "certificate_mismatch", "untrusted_identity"};
    const std::int64_t now = resolveNow(nowMs);
    const Json items = device.server().inbox(device.deviceId());
    std::vector<ChatRecord> fresh;
    Json handled = Json::array();

    struct Known {
        Json devices = Json::array();
        std::string error;
    };
    std::map<std::string, Known> lists;
    auto senderList = [&](const std::string& userId, const std::string& conversationId) -> const Known& {
        const std::string key = userId + "/" + conversationId;
        auto found = lists.find(key);
        if (found != lists.end()) return found->second;
        Known known;
        try {
            try {
                known.devices = verifiedDevices(device, userId, conversationId).devices;
            } catch (const api::ApiError& error) {
                // Part D: an item addressed with a story id (no conversation shares it): the sender's list as a friend's.
                if (error.status() != 404 || conversationId.empty()) throw;
                known.devices = verifiedDevices(device, userId).devices;
            }
        } catch (const EngineError& error) {
            known.error = error.code();
        } catch (const api::ApiError& error) {
            known.error = error.code().empty() ? "unknown_sender" : error.code();
        }
        return lists.emplace(key, std::move(known)).first->second;
    };
    auto joinGroup = [&](const std::string& conversationId) {
        device.must("groupSetMembers", Json{{"groupId", conversationId}, {"members", verifiedMemberDevices(device, conversationId)}});
    };
    // Gap 2: sender devices whose Signal session with this one fell out of step, once each per pass.
    struct Broken {
        Json peer;
        std::string conversationId;
    };
    std::vector<Broken> broken;
    std::set<std::string> brokenSeen;

    for (const Json& item : items) {
        const std::string id = item.at("id").get<std::string>();
        const std::string senderUserId = item.at("senderUserId").get<std::string>();
        const std::string senderDeviceId = item.at("senderDeviceId").get<std::string>();
        const std::string conversationId = stringOr(item, "conversationId");
        const Json parsed = Json::parse(stringOr(item, "body"), nullptr, false);

        const Known& known = senderList(senderUserId, conversationId);
        const Json* sender = nullptr;
        for (const Json& d : known.devices) {
            if (d.value("deviceId", std::string()) == senderDeviceId) sender = &d;
        }
        if (!sender) {
            if (!known.error.empty() && hold.count(known.error)) {  // kept: the contact's keys changed
                if (stats) {
                    ++stats->held;
                    ++stats->heldForKeys;
                }
                continue;
            }
            handled.push_back(id);  // unknown sender: never decryptable
            continue;
        }

        Json response;
        bool isGroupBody = parsed.is_object() && stringOr(parsed, "kind") == "group";
        bool signalGroup = false;  // M6: already decrypted below; skip the Megolm and Olm paths
        // M6: a Signal group message. The roster is verified first; a key or roster that hasn't arrived waits two days.
        if (parsed.is_object() && stringOr(parsed, "protocol") == "rv-signal-group-v1") {
            const Json fields = {{"sender", *sender}, {"message", parsed}};
            response = device.op("groupSignalDecrypt", fields);
            const std::string code = stringOr(response, "error");
            if (!response.value("ok", false) && code == "roster_unknown") {
                try {
                    verifyRosters(device, stringOr(parsed, "groupId"));
                    response = device.op("groupSignalDecrypt", fields);
                } catch (const std::exception&) {
                }
            }
            const std::string after = stringOr(response, "error");
            if (!response.value("ok", false) && (after == "no_sender_key" || after == "roster_unknown")) {
                const auto day = util::parseIsoMillis(stringOr(item, "createdOn") + "T00:00:00.000Z");
                if (!day || now - *day < 2 * kDay) {
                    if (stats) ++stats->held;
                    continue;
                }
            }
            isGroupBody = true;
            signalGroup = true;
        } else if (!parsed.is_object()) {
            handled.push_back(id);
            continue;
        }
        if (signalGroup) {
            // decrypted above
        } else if (isGroupBody) {
            const Json fields = {{"groupId", stringOr(parsed, "groupId")}, {"sessionId", stringOr(parsed, "sessionId")}, {"ciphertext", stringOr(parsed, "ciphertext")}};
            response = device.op("groupDecrypt", fields);
            if (!response.value("ok", false) && (stringOr(response, "error") == "unknown_group")) {
                try {
                    joinGroup(stringOr(parsed, "groupId"));
                    response = device.op("groupDecrypt", fields);
                } catch (const std::exception&) {
                }
            }
            if (!response.value("ok", false) && stringOr(response, "error") == "unknown_session") {
                // The room key may still come: hold for two days from the item's date.
                const auto day = util::parseIsoMillis(stringOr(item, "createdOn") + "T00:00:00.000Z");
                if (!day || now - *day < 2 * kDay) continue;
            }
        } else {
            response = device.op("decrypt", Json{{"sender", *sender}, {"message", parsed}});
            // M6: a Signal sender key for a roster epoch this device hasn't verified yet: verify, then once more.
            if (!response.value("ok", false) && stringOr(response, "error") == "roster_unknown" && !conversationId.empty()) {
                try {
                    verifyRosters(device, conversationId);
                    response = device.op("decrypt", Json{{"sender", *sender}, {"message", parsed}});
                } catch (const std::exception&) {
                }
            }
            const std::string code = stringOr(response, "error");
            if (!response.value("ok", false) && (code == "not_a_member" || code == "unknown_group") && !conversationId.empty()) {
                try {
                    joinGroup(conversationId);
                    response = device.op("decrypt", Json{{"sender", *sender}, {"message", parsed}});
                } catch (const std::exception&) {
                }
            }
        }
        if (!response.value("ok", false)) {
            const std::string code = stringOr(response, "error", "decrypt_failed");
            if (!isGroupBody && !hold.count(code) && brokenSession(code) && stringOr(parsed, "protocol") == "rv-signal-v1" &&
                brokenSeen.insert(senderUserId + "/" + senderDeviceId).second) {
                broken.push_back(Broken{*sender, conversationId});  // acknowledged below as before; a new session after the pass
            }
            if (!hold.count(code)) {
                handled.push_back(id);  // duplicate, not for this device, malformed: never going to open
            } else if (stats) {
                ++stats->held;
                ++stats->heldForKeys;
            }
            continue;
        }
        if (!isGroupBody) {
            // Key shares; a session refresh (gap 2) or a payload type this version doesn't know: nothing to show.
            const std::string kind = stringOr(response, "kind", "message");
            if (kind == "roomKey" || kind == "senderKey" || kind == "refresh" || kind == "ignored") {
                handled.push_back(id);
                continue;
            }
        }
        DecodedEnvelope envelope = decodeEnvelope(stringOr(response, "plaintext"), conversationId, now);
        if (envelope.kind == DecodedEnvelope::Kind::Newer) continue;  // "update the app": kept
        if (envelope.kind == DecodedEnvelope::Kind::Ignore) {
            handled.push_back(id);  // another chat's message moved here, or a type this version doesn't show
            continue;
        }
        if (envelope.kind == DecodedEnvelope::Kind::StoryKey || envelope.kind == DecodedEnvelope::Kind::StoryAnswer) {
            // Part D: pairwise only (never a group fan-out). Kept in the story-key store before the item is acknowledged;
            // whether the sender really wrote the story is checked when the feed names its author.
            if (stories && !isGroupBody) {
                if (envelope.storyKey) {
                    stories->putKey(envelope.storyKey->storyId, senderUserId, *envelope.storyKey->key, envelope.storyKey->expiresAt,
                                    senderUserId == device.userId());
                } else if (envelope.storyAnswer) {
                    StoryStore::Answer a;
                    a.storyId = envelope.storyAnswer->storyId;
                    a.stickerId = envelope.storyAnswer->stickerId;
                    a.userId = senderUserId;
                    a.rev = envelope.storyAnswer->rev;
                    a.final = envelope.storyAnswer->final;
                    a.payload = envelope.storyAnswer->payload.dump();
                    a.at = envelope.ts;
                    stories->putAnswer(a);
                }
            }
            handled.push_back(id);
            continue;
        }
        if (envelope.kind == DecodedEnvelope::Kind::Call || envelope.kind == DecodedEnvelope::Kind::CallKey) {
            // Part B: acknowledged and handed out, never stored or folded. Only pairwise (never a group fan-out),
            // only from another account, and only when meant for this device or for every device.
            handled.push_back(id);
            if (!calls || isGroupBody || senderUserId == device.userId()) continue;
            if (envelope.call && envelope.call->to != "*" && envelope.call->to != device.deviceId()) continue;
            CallMessage message;
            message.conversationId = conversationId;
            message.fromUserId = senderUserId;
            message.fromDeviceId = senderDeviceId;
            message.ts = envelope.ts;
            message.signal = std::move(envelope.call);
            message.key = std::move(envelope.callKey);
            calls->push_back(std::move(message));
            continue;
        }
        // Part C2: in a community a text, file or poll is taken only from a Signal group message (the engine checked
        // its sender against the roster); pairwise items carry sender keys, call keys and ops. In a channel, the ops
        // that come pairwise from another account are a reader's reactions, votes and receipts, nothing else.
        if (const auto community = communityOf(device, conversationId); community && senderUserId != device.userId() && !isGroupBody) {
            const std::string t = envelope.op ? envelope.op->t : std::string();
            const bool readerOp = t == "reaction" || t == "vote" || t == "receipt";
            if (envelope.kind == DecodedEnvelope::Kind::Message || (community->channel() && !readerOp)) {
                handled.push_back(id);
                continue;
            }
        }
        ChatRecord record;
        record.conversationId = conversationId;
        record.id = envelope.id;
        record.from = senderUserId;
        record.fromDeviceId = senderDeviceId;
        record.ts = envelope.ts;
        record.arrivedAt = now;
        record.text = std::move(envelope.text);
        record.file = std::move(envelope.file);
        record.op = std::move(envelope.op);
        record.replyTo = envelope.extras.replyTo;
        record.replyPreview = envelope.extras.replyPreview;
        record.forwarded = envelope.extras.forwarded;
        record.exp = envelope.extras.exp;
        record.poll = std::move(envelope.poll);
        record.storyReply = std::move(envelope.extras.storyReply);
        const bool mine = senderUserId == device.userId();
        record.direction = mine ? "out" : "in";
        record.status = mine ? "sent" : "received";
        // Written to history before the item is acknowledged.
        if (history.add(record)) {
            history.pin(conversationId);
            fresh.push_back(record);
        }
        handled.push_back(id);
    }
    if (!handled.empty()) device.server().ack(device.deviceId(), handled);
    for (const Broken& b : broken) {
        try {
            healSession(device, b.peer, b.conversationId);
        } catch (...) {
            // Best effort: tried again the next time a message from that device fails (nothing logged, no ids).
        }
    }
    if (stories) stories->dropStaleDeliveries();
    std::stable_sort(fresh.begin(), fresh.end(), [](const ChatRecord& a, const ChatRecord& b) { return a.ts < b.ts; });
    return fresh;
}

// --- Calls over device keys (part B) ---

namespace {

/// One send that is never queued: a stale device list is fetched again once, like a chat send.
template <typename F>
int sendUnqueued(F&& attempt) {
    NoQueueScope unqueued;
    try {
        return attempt();
    } catch (const api::ApiError& error) {
        if (!isStale(error)) throw;
        return attempt();
    }
}

}  // namespace

int sendCallSignal(V3Device& device, const std::string& conversationId, const std::string& contactUserId, const CallSignal& signal,
                   std::int64_t nowMs) {
    if (contactUserId.empty() || contactUserId == device.userId()) throw EngineError("bad_call", "A call goes to the other person.");
    const std::string plaintext = encodeCall(conversationId, signal, util::randomUuid(), resolveNow(nowMs));
    return sendUnqueued([&] {
        std::vector<Json> recipients;
        for (const Json& d : verifiedDevices(device, contactUserId, conversationId).devices) {
            if (signal.to == "*" || d.value("deviceId", std::string()) == signal.to) recipients.push_back(d);
        }
        if (recipients.empty()) {
            throw EngineError(signal.to == "*" ? "contact_has_no_device_keys" : "unknown_device",
                              signal.to == "*" ? "They haven't set up device keys yet." : "The device that called isn't in their list any more.");
        }
        // The offer goes to every device of theirs (marked allDevices when all can be reached); an answer to one, never marked.
        FanOut options;
        options.allDevices = signal.to == "*";
        const int delivered = sendToDevices(device, conversationId, recipients, plaintext, nullptr, options);
        if (delivered == 0) throw EngineError("unreachable", "None of their devices could be reached.");
        return delivered;
    });
}

int sendCallKey(V3Device& device, const std::string& conversationId, const CallKey& key, const std::vector<std::string>& userIds,
                std::int64_t nowMs) {
    const std::string plaintext = encodeCallKey(conversationId, key, util::randomUuid(), resolveNow(nowMs));
    return sendUnqueued([&] {
        std::vector<Json> recipients;
        std::set<std::string> seen;
        for (const std::string& user : userIds) {
            if (user.empty() || user == device.userId() || !seen.insert(user).second) continue;
            for (const Json& d : verifiedDevices(device, user, conversationId).devices) recipients.push_back(d);
        }
        FanOut options;
        options.allDevices = true;  // every device of each account: marked when none is unreachable
        return sendToDevices(device, conversationId, recipients, plaintext, nullptr, options);
    });
}

void CallGate::expectCall(const std::string& callId, const std::string& conversationId, const std::string& peerUserId, Role role) {
    Entry entry;
    entry.conversationId = conversationId;
    entry.peerUserId = peerUserId;
    entry.role = role;
    calls_[callId] = std::move(entry);
}

void CallGate::expectGroupCall(const std::string& callId, const std::string& conversationId, std::vector<std::string> members) {
    auto& entry = calls_[callId];
    const std::int64_t newest = entry.group && entry.conversationId == conversationId ? entry.newestEpoch : 0;
    entry = Entry{};
    entry.conversationId = conversationId;
    entry.group = true;
    entry.members = std::move(members);
    entry.newestEpoch = newest;
}

void CallGate::setMembers(const std::string& callId, std::vector<std::string> members) {
    const auto it = calls_.find(callId);
    if (it != calls_.end() && it->second.group) it->second.members = std::move(members);
}

void CallGate::forget(const std::string& callId) {
    calls_.erase(callId);
    std::erase_if(held_, [&](const auto& h) {
        const CallMessage& m = h.second;
        return (m.signal && m.signal->callId == callId) || (m.key && m.key->callId == callId);
    });
}

std::string CallGate::peerDevice(const std::string& callId) const {
    const auto it = calls_.find(callId);
    return it == calls_.end() ? std::string() : it->second.peerDeviceId;
}

std::int64_t CallGate::newestEpoch(const std::string& callId) const {
    const auto it = calls_.find(callId);
    return it == calls_.end() ? 0 : it->second.newestEpoch;
}

std::string CallGate::check(const CallMessage& m, std::int64_t now) {
    const std::string& callId = m.signal ? m.signal->callId : m.key ? m.key->callId : std::string();
    if (callId.empty()) return "malformed";
    const auto it = calls_.find(callId);
    if (it == calls_.end()) return "unknown_call";
    Entry& entry = it->second;
    if (m.conversationId != entry.conversationId) return "wrong_conversation";
    if (m.signal) {
        if (entry.group) return "unknown_call";
        if (now - m.ts > kCallMaxAgeMs) return "stale_call";
        if (m.fromUserId != entry.peerUserId) return "wrong_sender";
        const std::string& step = m.signal->step;
        if (step == "offer" || step == "answer") {
            // The callee takes an offer, the caller an answer: each once, from one device.
            if (step != (entry.role == Role::Callee ? "offer" : "answer")) return "wrong_step";
            if (!entry.peerDeviceId.empty()) return "already_set_up";
            entry.peerDeviceId = m.fromDeviceId;
            return {};
        }
        // A restart only from the device the call was set up with.
        if (entry.peerDeviceId.empty() || m.fromDeviceId != entry.peerDeviceId) return "wrong_device";
        return {};
    }
    if (!entry.group) return "unknown_call";
    if (std::find(entry.members.begin(), entry.members.end(), m.fromUserId) == entry.members.end()) return "not_a_member";
    if (m.key->epoch < entry.newestEpoch) return "old_epoch";
    entry.newestEpoch = m.key->epoch;
    return {};
}

std::vector<CallMessage> CallGate::admit(std::vector<CallMessage> messages, std::int64_t nowMs, std::vector<std::string>* refused) {
    const std::int64_t now = resolveNow(nowMs);
    std::vector<std::pair<std::int64_t, CallMessage>> queue;
    // What was held goes first (it arrived first); what's too old by now is dropped.
    for (auto& h : held_) {
        if (now - h.first <= kHoldMs) {
            queue.push_back(std::move(h));
        } else if (refused) {
            refused->push_back("unknown_call");
        }
    }
    held_.clear();
    for (auto& m : messages) queue.emplace_back(now, std::move(m));
    std::vector<CallMessage> out;
    for (auto& [arrived, m] : queue) {
        const std::string why = check(m, now);
        if (why.empty()) {
            out.push_back(std::move(m));
        } else if (why == "unknown_call" && !calls_.count(m.signal ? m.signal->callId : m.key ? m.key->callId : std::string()) &&
                   held_.size() < kMaxHeld) {
            held_.emplace_back(arrived, std::move(m));  // may beat the live event that names its call
        } else if (refused) {
            refused->push_back(why);
        }
    }
    return out;
}

}  // namespace redvault::e2ev3
