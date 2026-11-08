// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "inbox_session.hpp"

#include <QDateTime>
#include <QLocale>

#include "async.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace {

constexpr int kPollMs = 60'000;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QString str(const api::Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? qs(it->get<std::string>()) : QString();
}

QString when(const QString& iso) {
    const auto ms = util::parseIsoMillis(iso.toStdString());
    if (!ms) return {};
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(*ms).toLocalTime(), QLocale::ShortFormat);
}

QVariantMap itemFrom(const api::Json& j) {
    QVariantList options;
    if (const auto it = j.find("options"); it != j.end() && it->is_array()) {
        for (const auto& o : *it) {
            if (o.is_object()) options.append(QVariantMap{{"id", str(o, "id")}, {"label", str(o, "label")}});
        }
    }
    return {
        {"id", str(j, "id")},
        {"kind", str(j, "kind")},
        {"body", str(j, "body")},
        {"allowReplies", j.value("allowReplies", false)},
        {"when", when(str(j, "createdAt"))},
        {"read", j.value("read", false)},
        {"options", options},
        {"myOptionId", str(j, "myOptionId")},
        {"replies", QVariantList{}},
        {"repliesLoaded", false},
    };
}

QVariantMap replyFrom(const api::Json& j) {
    return {{"id", str(j, "id")}, {"body", str(j, "body")}, {"when", when(str(j, "createdAt"))}};
}

}  // namespace

InboxSession::InboxSession(std::shared_ptr<api::ApiClient> api, QObject* parent) : QObject(parent), api_(std::move(api)) {
    pool_.setMaxThreadCount(1);
    timer_.setInterval(kPollMs);
    connect(&timer_, &QTimer::timeout, this, &InboxSession::refresh);
    timer_.start();
    refresh();
}

InboxSession::~InboxSession() {
    timer_.stop();
    pool_.clear();
    pool_.waitForDone();
}

int InboxSession::unread() const {
    int count = 0;
    for (const auto& item : items_) {
        if (!item.toMap().value("read").toBool()) ++count;
    }
    return count;
}

void InboxSession::refresh() {
    if (refreshing_) return;
    refreshing_ = true;
    auto api = api_;
    runInBackground(
        pool_, this, [api] { return api->call("GET", "announcements"); },
        [this](api::Json list) {
            refreshing_ = false;
            // Keep replies already fetched; everything else comes fresh.
            QVariantMap previous;
            for (const auto& item : items_) previous.insert(item.toMap().value("id").toString(), item);
            QVariantList fresh;
            if (list.is_array()) {
                for (const auto& j : list) {
                    if (!j.is_object()) continue;
                    QVariantMap item = itemFrom(j);
                    const QVariantMap old = previous.value(item.value("id").toString()).toMap();
                    if (old.value("repliesLoaded").toBool()) {
                        item["replies"] = old.value("replies");
                        item["repliesLoaded"] = true;
                    }
                    fresh.append(item);
                }
            }
            items_ = fresh;
            loaded_ = true;
            emit itemsChanged();
        },
        [this](const Failure& failure) {
            refreshing_ = false;
            // A server without the inbox (or a hiccup) just shows nothing new; only a lost session matters.
            if (failure.status == 401) emit sessionExpired();
        });
}

void InboxSession::markAllRead() {
    QStringList unreadIds;
    for (auto& item : items_) {
        QVariantMap map = item.toMap();
        if (map.value("read").toBool()) continue;
        unreadIds.append(map.value("id").toString());
        map["read"] = true;
        item = map;
    }
    if (unreadIds.isEmpty()) return;
    emit itemsChanged();
    auto api = api_;
    runInBackground(
        pool_, this,
        [api, unreadIds] {
            for (const auto& id : unreadIds) api->call("POST", "announcements/" + net::percentEncode(id.toStdString()) + "/read");
        },
        [] {}, [this](const Failure& failure) { fail(failure.status, {}); });
}

void InboxSession::loadReplies(const QString& id) {
    auto api = api_;
    runInBackground(
        pool_, this, [api, id] { return api->call("GET", "announcements/" + net::percentEncode(id.toStdString())); },
        [this, id](api::Json detail) {
            QVariantList replies;
            if (const auto it = detail.find("replies"); it != detail.end() && it->is_array()) {
                for (const auto& r : *it) {
                    if (r.is_object()) replies.append(replyFrom(r));
                }
            }
            update(id, QStringLiteral("replies"), replies);
            update(id, QStringLiteral("repliesLoaded"), true);
        },
        [this](const Failure& failure) { fail(failure.status, {}); });
}

void InboxSession::vote(const QString& id, const QString& optionId) {
    if (busy_) return;
    setBusy(true);
    setError({});
    auto api = api_;
    runInBackground(
        pool_, this,
        [api, id, optionId] {
            api->call("POST", "announcements/" + net::percentEncode(id.toStdString()) + "/vote",
                      api::Json{{"optionId", optionId.toStdString()}});
        },
        [this, id, optionId] {
            setBusy(false);
            update(id, QStringLiteral("myOptionId"), optionId);
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure.status, failure.code == QStringLiteral("already_voted") ? QStringLiteral("You've already voted in this poll.")
                                                                                 : QStringLiteral("Couldn't record your vote. Try again."));
        });
}

void InboxSession::reply(const QString& id, const QString& body) {
    const QString text = body.trimmed();
    if (text.isEmpty() || busy_) return;
    setBusy(true);
    setError({});
    auto api = api_;
    runInBackground(
        pool_, this,
        [api, id, text] {
            return api->call("POST", "announcements/" + net::percentEncode(id.toStdString()) + "/replies",
                             api::Json{{"body", text.toStdString()}});
        },
        [this, id](api::Json created) {
            setBusy(false);
            for (const auto& item : items_) {
                const QVariantMap map = item.toMap();
                if (map.value("id").toString() != id) continue;
                QVariantList replies = map.value("replies").toList();
                replies.append(replyFrom(created));
                update(id, QStringLiteral("replies"), replies);
            }
            emit replied(id);
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure.status, failure.status == 403 ? QStringLiteral("Replies are turned off for this message.")
                                                       : QStringLiteral("Couldn't send your reply. Try again."));
        });
}

void InboxSession::clearError() {
    setError({});
}

void InboxSession::update(const QString& id, const QString& key, const QVariant& value) {
    for (auto& item : items_) {
        QVariantMap map = item.toMap();
        if (map.value("id").toString() != id) continue;
        map[key] = value;
        item = map;
        emit itemsChanged();
        return;
    }
}

void InboxSession::fail(int status, const QString& message) {
    if (status == 401) {
        emit sessionExpired();
        return;
    }
    if (!message.isEmpty()) setError(message);
}

void InboxSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void InboxSession::setError(const QString& error) {
    if (error_ == error) return;
    error_ = error;
    emit errorChanged();
}

}  // namespace redvault::app
