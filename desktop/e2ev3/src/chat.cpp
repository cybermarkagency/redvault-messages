// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/chat.hpp"

#include <sodium.h>

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <tuple>

#include "fs_util.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/util/text.hpp"

namespace redvault::e2ev3 {

namespace {

constexpr std::int64_t kDay = 24LL * 3600 * 1000;
constexpr std::size_t kMaxText = 16 * 1024;
constexpr std::size_t kMaxEmoji = 32;
constexpr std::int64_t kMaxPast = 30 * kDay;
constexpr std::int64_t kMaxFuture = 5 * 60 * 1000;

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
    Json j = {{"t", op.t}, {"target", op.target}};
    if (op.t == "edit") j["body"] = op.body;
    if (op.t == "reaction") j["emoji"] = op.emoji;
    return j;
}

Json recordJson(const ChatRecord& r) {
    Json j = {{"conversationId", r.conversationId}, {"id", r.id}, {"from", r.from}, {"fromDeviceId", r.fromDeviceId}, {"ts", r.ts},
              {"arrivedAt", r.arrivedAt}, {"text", r.text}, {"direction", r.direction}, {"status", r.status}};
    if (r.file) j["file"] = fileJson(*r.file);
    if (r.op) j["op"] = opJson(*r.op);
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
    if (const auto op = j.find("op"); op != j.end() && op->is_object()) {
        r.op = ChatOp{stringOr(*op, "t"), stringOr(*op, "target"), stringOr(*op, "body"), stringOr(*op, "emoji")};
    }
    return r;
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

/// Envelopes fill 256-byte buckets, so their size shows less about what they hold. Spaces after the JSON, which every parser skips.
std::string padded(std::string json) {
    constexpr std::size_t kBucket = 256;
    json.append((json.size() / kBucket + 1) * kBucket - json.size(), ' ');
    return json;
}

std::string encodeEnvelope(const Envelope& e) {
    return padded(Json{{"v", e.v}, {"t", e.t}, {"id", e.id}, {"c", e.c}, {"ts", e.ts}, {"body", e.body}}.dump());
}

std::string encodeOp(const std::string& conversationId, const ChatOp& op, const std::string& id, std::int64_t ts) {
    if (op.t == "edit" && (op.body.empty() || op.body.size() > kMaxText)) throw EngineError("bad_text", "A message has to be between 1 and 16,384 bytes.");
    if (op.t == "reaction" && op.emoji.size() > kMaxEmoji) throw EngineError("bad_emoji", "That reaction is too long.");
    Json j = opJson(op);
    j["v"] = 1;
    j["id"] = id;
    j["c"] = conversationId;
    j["ts"] = ts;
    return padded(j.dump());
}

std::string encodeFile(const std::string& conversationId, const model::FileDescriptor& file, const std::string& id, std::int64_t ts) {
    return padded(Json{{"v", 1}, {"t", "file"}, {"id", id}, {"c", conversationId}, {"ts", ts}, {"file", fileJson(file)}}.dump());
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
    const std::string t = stringOr(e, "t");
    if (t != "text" && t != "file" && t != "reaction" && t != "edit" && t != "delete") return ignore("unknown_type");
    DecodedEnvelope d;
    d.id = stringOr(e, "id");
    if (!isUuid(d.id)) return ignore("bad_id");
    if (conversationId.empty() || stringOr(e, "c") != conversationId) return ignore("wrong_conversation");
    const std::int64_t now = resolveNow(nowMs);
    const auto claimed = e.find("ts");
    const double ts = claimed != e.end() && claimed->is_number() ? claimed->get<double>() : static_cast<double>(now);
    d.ts = static_cast<std::int64_t>(std::clamp(ts, static_cast<double>(now - kMaxPast), static_cast<double>(now + kMaxFuture)));

    const auto text = [&](const char* key, std::size_t max) -> std::optional<std::string> {
        const auto it = e.find(key);
        if (it == e.end() || !it->is_string() || it->get_ref<const std::string&>().size() > max) return std::nullopt;
        return it->get<std::string>();
    };
    if (t == "file") {
        const auto file = e.find("file");
        d.file = fileFrom(file != e.end() ? *file : Json(nullptr));
        if (!d.file) return ignore("bad_file");
        d.kind = Kind::Message;
        return d;
    }
    if (t == "text") {
        const auto body = text("body", kMaxText);
        if (!body) return ignore("bad_body");
        d.kind = Kind::Message;
        d.text = *body;
        return d;
    }
    ChatOp op;
    op.t = t;
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
    }
    d.kind = Kind::Op;
    d.op = std::move(op);
    return d;
}

// --- Fold ---

std::vector<ChatRecord> foldChat(const std::vector<ChatRecord>& records) {
    std::map<std::string, ChatRecord> messages;
    for (const ChatRecord& r : records) {
        if (!r.op) messages.emplace(r.id, r);
    }
    std::set<std::string> deleted;
    for (const ChatRecord& r : records) {
        if (!r.op || r.op->t != "delete") continue;
        const auto message = messages.find(r.op->target);
        if (message != messages.end() && message->second.from == r.from) deleted.insert(r.op->target);
    }
    std::map<std::string, const ChatRecord*> edits;
    std::map<std::pair<std::string, std::string>, const ChatRecord*> reactions;  // (target, user)
    for (const ChatRecord& r : records) {
        if (!r.op) continue;
        const auto message = messages.find(r.op->target);
        if (message == messages.end() || deleted.count(r.op->target)) continue;
        if (r.op->t == "edit" && r.from == message->second.from && !message->second.file) {
            const ChatRecord*& held = edits[r.op->target];
            if (!held || newer(r, *held)) held = &r;
        } else if (r.op->t == "reaction") {
            const ChatRecord*& held = reactions[{r.op->target, r.from}];
            if (!held || newer(r, *held)) held = &r;
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
    for (const std::string& id : deleted) {
        ChatRecord& message = messages.at(id);
        message.text.clear();
        message.file.reset();
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
}

void ChatHistory::save() const {
    Json records = Json::array();
    for (const ChatRecord& r : records_) records.push_back(recordJson(r));
    const std::string plain = Json{{"records", records}, {"pins", pins_}}.dump();
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
    eraseDeleted(record.conversationId);  // before the write, so a deleted message's text is never put on disk again
    save();
    return true;
}

void ChatHistory::eraseDeleted(const std::string& conversationId) {
    std::set<std::string> gone;
    for (const ChatRecord& r : records_) {
        if (r.conversationId != conversationId || !r.op || r.op->t != "delete") continue;
        for (const ChatRecord& message : records_) {
            if (message.conversationId == conversationId && !message.op && message.id == r.op->target && message.from == r.from) gone.insert(message.id);
        }
    }
    if (gone.empty()) return;
    std::erase_if(records_, [&](const ChatRecord& r) {
        return r.conversationId == conversationId && r.op && r.op->t != "delete" && gone.count(r.op->target) > 0;
    });
    for (ChatRecord& r : records_) {
        if (r.conversationId != conversationId || r.op || !gone.count(r.id)) continue;
        r.text.clear();
        r.file.reset();
    }
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

std::vector<ChatRecord> ChatHistory::list(const std::string& conversationId) const {
    return foldChat(records(conversationId));
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
    return device.must("rosterVerify", Json{{"groupId", conversationId}, {"chain", chain}});
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
        const Json claimed = device.server().signalClaim(conversationId, ask);
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
    if (!keyMessages.empty()) device.server().sendMessages(device.deviceId(), conversationId, keyMessages);
    const Json sealed = device.must("groupSignalEncrypt", Json{{"groupId", conversationId}, {"plaintext", plaintext}});
    Json fanout = Json::array();
    for (const Json& d : others) fanout.push_back({{"userId", d.at("userId")}, {"deviceId", d.at("deviceId")}});
    if (fanout.size() > 100) throw EngineError("group_too_large", "Device-key groups are limited to 100 member devices for now.");
    device.server().sendGroupMessage(device.deviceId(), conversationId, sealed.at("message").dump(), fanout);
    return static_cast<int>(fanout.size());
}

/// One group send: membership from verified lists, room keys to devices that lack them, then one fan-out.
int sendToGroup(V3Device& device, const std::string& conversationId, const std::string& plaintext) {
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
    if (!keyMessages.empty()) device.server().sendMessages(device.deviceId(), conversationId, keyMessages);
    const Json sealed = device.must("groupEncrypt", Json{{"groupId", conversationId}, {"plaintext", plaintext}});
    const Json body = {{"v", 1}, {"kind", "group"}, {"groupId", conversationId}, {"sessionId", sealed.at("sessionId")}, {"ciphertext", sealed.at("ciphertext")}};
    Json recipients = Json::array();
    for (const Json& d : members) {
        if (d.value("deviceId", std::string()) == device.deviceId()) continue;
        recipients.push_back({{"userId", d.at("userId")}, {"deviceId", d.at("deviceId")}});
    }
    if (recipients.size() > 100) throw EngineError("group_too_large", "Device-key groups are limited to 100 member devices for now.");
    device.server().sendGroupMessage(device.deviceId(), conversationId, body.dump(), recipients);
    return static_cast<int>(recipients.size());
}

/// An empty `contactUserId` means a group. `record` goes into the history as sent, or as failed when `keepFailed`.
SendOutcome deliver(V3Device& device, ChatHistory& history, ChatRecord record, const std::string& plaintext, const std::string& contactUserId,
                    bool keepFailed = true) {
    const std::string conversationId = record.conversationId;
    // A chat that uses device keys never falls back silently: no keys on their side means no send (9.8).
    if (!contactUserId.empty() && verifiedDevices(device, contactUserId, conversationId).devices.empty()) {
        throw EngineError("contact_has_no_device_keys", "They haven't set up device keys yet. Use the older protocol for this chat.");
    }
    int unreachable = 0;
    const auto attempt = [&] {
        return contactUserId.empty() ? sendToGroup(device, conversationId, plaintext)
                                     : sendText(device, conversationId, contactUserId, plaintext, &unreachable);
    };
    int delivered = 0;
    try {
        try {
            unreachable = 0;
            delivered = attempt();
        } catch (const api::ApiError& error) {
            if (!isStale(error)) throw;
            unreachable = 0;
            delivered = attempt();  // a list changed: refetch, verify, once more
        }
    } catch (...) {
        if (keepFailed) {
            record.status = "failed";
            history.add(record);
        }
        throw;
    }
    history.add(record);
    history.pin(conversationId);
    return SendOutcome{record.id, delivered, unreachable};
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

// --- Receiving ---

std::vector<ChatRecord> receiveChat(V3Device& device, ChatHistory& history, std::int64_t nowMs, ReceiveStats* stats) {
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
            known.devices = verifiedDevices(device, userId, conversationId).devices;
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
            if (!hold.count(code)) {
                handled.push_back(id);  // duplicate, not for this device, malformed: never going to open
            } else if (stats) {
                ++stats->held;
                ++stats->heldForKeys;
            }
            continue;
        }
        if (!isGroupBody && (stringOr(response, "kind", "message") == "roomKey" || stringOr(response, "kind", "message") == "senderKey")) {
            handled.push_back(id);
            continue;
        }
        DecodedEnvelope envelope = decodeEnvelope(stringOr(response, "plaintext"), conversationId, now);
        if (envelope.kind == DecodedEnvelope::Kind::Newer) continue;  // "update the app": kept
        if (envelope.kind == DecodedEnvelope::Kind::Ignore) {
            handled.push_back(id);  // another chat's message moved here, or a type this version doesn't show
            continue;
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
    std::stable_sort(fresh.begin(), fresh.end(), [](const ChatRecord& a, const ChatRecord& b) { return a.ts < b.ts; });
    return fresh;
}

}  // namespace redvault::e2ev3
