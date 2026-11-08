// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/send_queue.hpp"

#include <sodium.h>

#include <algorithm>

#include "fs_util.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/util/text.hpp"

namespace redvault::e2ev3 {

namespace {

thread_local const SendScope* tCurrentScope = nullptr;

std::string stringAt(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

Json itemJson(const QueuedSend& q) {
    return Json{{"id", q.id},
                {"kind", q.kind},
                {"senderDeviceId", q.senderDeviceId},
                {"conversationId", q.conversationId},
                {"messages", q.messages},
                {"body", q.body},
                {"recipients", q.recipients},
                {"recordId", q.recordId},
                {"carriesRecord", q.carriesRecord}};
}

QueuedSend itemFrom(const Json& j) {
    QueuedSend q;
    q.id = stringAt(j, "id");
    q.kind = stringAt(j, "kind");
    q.senderDeviceId = stringAt(j, "senderDeviceId");
    q.conversationId = stringAt(j, "conversationId");
    q.messages = j.value("messages", Json::array());
    q.body = stringAt(j, "body");
    q.recipients = j.value("recipients", Json::array());
    q.recordId = stringAt(j, "recordId");
    q.carriesRecord = j.value("carriesRecord", false);
    return q;
}

}  // namespace

bool waitsForConnection(const api::ApiError& error) {
    return error.status() == 0 || error.status() >= 500 || error.status() == 429 || error.status() == 401;
}

// --- SendQueue ---

SendQueue::SendQueue(std::filesystem::path file, crypto::Bytes key) : file_(std::move(file)), key_(std::move(key)) {
    crypto::ensureSodium();
    if (key_.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw std::invalid_argument("SendQueue needs a 32-byte key");
    load();
}

void SendQueue::load() {
    const auto blob = fsutil::readAll(file_);
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (!blob || blob->size() <= nonceLen) return;
    const auto* data = reinterpret_cast<const unsigned char*>(blob->data());
    std::string plain(blob->size() - nonceLen, '\0');
    unsigned long long plainLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()), &plainLen, nullptr, data + nonceLen,
                                                   blob->size() - nonceLen, nullptr, 0, data, key_.data()) != 0) {
        return;  // another device's key (the device was replaced): nothing it queued can be sent as this one
    }
    plain.resize(plainLen);
    const Json j = Json::parse(plain, nullptr, false);
    if (!j.is_object()) return;
    for (const Json& item : j.value("items", Json::array())) {
        if (item.is_object()) items_.push_back(itemFrom(item));
    }
}

void SendQueue::save() const {
    if (items_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(file_, ignored);
        return;
    }
    Json list = Json::array();
    for (const QueuedSend& q : items_) list.push_back(itemJson(q));
    const std::string plain = Json{{"items", list}}.dump();
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    std::string blob(nonceLen + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    randombytes_buf(blob.data(), nonceLen);
    unsigned long long cipherLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(blob.data()) + nonceLen, &cipherLen,
                                               reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nullptr, 0, nullptr,
                                               reinterpret_cast<const unsigned char*>(blob.data()), key_.data());
    blob.resize(nonceLen + cipherLen);
    std::error_code ignored;
    std::filesystem::create_directories(file_.parent_path(), ignored);
    fsutil::writeAtomic(file_, blob);
}

void SendQueue::push(QueuedSend item) {
    std::lock_guard lock(mutex_);
    if (item.id.empty()) item.id = util::randomUuid();
    items_.push_back(std::move(item));
    save();
}

void SendQueue::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto before = items_.size();
    std::erase_if(items_, [&](const QueuedSend& q) { return q.id == id; });
    if (items_.size() != before) save();
}

std::vector<QueuedSend> SendQueue::items() const {
    std::lock_guard lock(mutex_);
    return items_;
}

bool SendQueue::empty() const {
    std::lock_guard lock(mutex_);
    return items_.empty();
}

// --- SendScope ---

SendScope::SendScope(std::string recordId_, std::string carrierKind_)
    : previous_(tCurrentScope), recordId(std::move(recordId_)), carrierKind(std::move(carrierKind_)) {
    tCurrentScope = this;
}

SendScope::~SendScope() {
    tCurrentScope = previous_;
}

const SendScope* SendScope::current() {
    return tCurrentScope;
}

// --- QueueingV3Server ---

void QueueingV3Server::attach(std::shared_ptr<SendQueue> queue) {
    std::lock_guard lock(mutex_);
    queue_ = std::move(queue);
}

std::shared_ptr<SendQueue> QueueingV3Server::queue() const {
    std::lock_guard lock(mutex_);
    return queue_;
}

void QueueingV3Server::sendNow(const QueuedSend& item) {
    if (item.kind == "group") {
        inner_->sendGroupMessage(item.senderDeviceId, item.conversationId, item.body, item.recipients);
    } else {
        inner_->sendMessages(item.senderDeviceId, item.conversationId, item.messages);
    }
}

void QueueingV3Server::sendOrQueue(QueuedSend item) {
    const SendScope* scope = SendScope::current();
    const auto queue = this->queue();
    if (scope == nullptr || !queue) return sendNow(item);
    item.recordId = scope->recordId;
    item.carriesRecord = item.kind == scope->carrierKind;
    // Earlier requests still wait: this one goes behind them, so nothing overtakes.
    if (!queue->empty()) {
        queue->push(std::move(item));
        throw SendQueued();
    }
    try {
        sendNow(item);
    } catch (const api::ApiError& error) {
        // Only "tor_unavailable" is certain to have sent nothing (the gate opened no socket): the same ciphertext
        // can then go later without the server ever seeing it twice. Anything else is the caller's failure, as before.
        if (error.code() != "tor_unavailable") throw;
        queue->push(std::move(item));
        throw SendQueued();
    }
}

void QueueingV3Server::sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages) {
    QueuedSend item;
    item.kind = "messages";
    item.senderDeviceId = senderDeviceId;
    item.conversationId = conversationId;
    item.messages = messages;
    sendOrQueue(std::move(item));
}

void QueueingV3Server::sendGroupMessage(const std::string& senderDeviceId, const std::string& conversationId, const std::string& body,
                                        const Json& recipients) {
    QueuedSend item;
    item.kind = "group";
    item.senderDeviceId = senderDeviceId;
    item.conversationId = conversationId;
    item.body = body;
    item.recipients = recipients;
    sendOrQueue(std::move(item));
}

QueueingV3Server::Flushed QueueingV3Server::flush() {
    Flushed out;
    const auto queue = this->queue();
    if (!queue) return out;
    for (const QueuedSend& item : queue->items()) {
        try {
            sendNow(item);
        } catch (const api::ApiError& error) {
            if (waitsForConnection(error)) {
                out.waiting = true;
                return out;  // this one and everything after it wait, in order
            }
            // Refused for good (say, a device list changed meanwhile): this ciphertext can't be delivered.
            queue->remove(item.id);
            if (!item.recordId.empty()) out.refused.emplace_back(item.conversationId, item.recordId);
            continue;
        }
        // Removed before the record is marked sent: a crash in between re-sends the message under the same
        // envelope id (which the recipients' histories drop), never the same ciphertext twice.
        queue->remove(item.id);
        if (item.carriesRecord && !item.recordId.empty()) out.sent.emplace_back(item.conversationId, item.recordId);
    }
    return out;
}

}  // namespace redvault::e2ev3
