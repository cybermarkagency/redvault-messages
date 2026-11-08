// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "people_session.hpp"

#include "prefs.hpp"

#include <utility>

namespace redvault::app {

namespace {

constexpr int kPollMs = 5'000;

QString qs(const std::string& text) {
    return QString::fromStdString(text);
}

QVariantList toList(const std::vector<data::FriendRequestView>& requests) {
    QVariantList out;
    for (const auto& r : requests) {
        out.append(QVariantMap{{QStringLiteral("id"), qs(r.id)},
                               {QStringLiteral("userId"), qs(r.userId)},
                               {QStringLiteral("handle"), qs(r.handle)},
                               {QStringLiteral("avatarUrl"), qs(r.avatarUrl.value_or(""))}});
    }
    return out;
}

struct Snapshot {
    data::FriendRequests requests;
    std::vector<api::FriendDto> friends;
};

}  // namespace

PeopleSession::PeopleSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                             QObject* parent)
    : QObject(parent),
      api_(std::move(api)),
      identity_(std::move(identity)),
      friendRepo_(std::make_shared<data::FriendRepository>(api_, prefs::contactKeyPinStore())),
      groupRepo_(std::make_shared<data::GroupRepository>(api_)) {
    pool_.setMaxThreadCount(2);
    timer_.setInterval(kPollMs);
    connect(&timer_, &QTimer::timeout, this, &PeopleSession::refresh);
    timer_.start();
    refresh();
    refreshBlocked();
}

PeopleSession::~PeopleSession() {
    timer_.stop();
    pool_.clear();
    pool_.waitForDone();
}

void PeopleSession::refresh() {
    if (refreshing_) return;
    refreshing_ = true;
    auto friendRepo = friendRepo_;
    auto groupRepo = groupRepo_;
    runInBackground(
        pool_, this, [friendRepo, groupRepo]() { return Snapshot{friendRepo->pending(), groupRepo->friends()}; },
        [this](Snapshot snapshot) {
            refreshing_ = false;
            const auto incoming = toList(snapshot.requests.incoming);
            const auto outgoing = toList(snapshot.requests.outgoing);
            if (incoming != incoming_ || outgoing != outgoing_) {
                incoming_ = incoming;
                outgoing_ = outgoing;
                emit requestsChanged();
            }
            QVariantList friends;
            for (const auto& f : snapshot.friends) {
                friends.append(QVariantMap{{QStringLiteral("userId"), qs(f.userId)},
                                           {QStringLiteral("handle"), qs(f.username)},
                                           {QStringLiteral("avatarUrl"), qs(f.avatarUrl.value_or(""))}});
            }
            if (friends != friends_) {
                friends_ = friends;
                emit friendsChanged();
            }
        },
        [this](const Failure& failure) {
            refreshing_ = false;
            if (failure.status == 401) emit sessionExpired();
            // Otherwise quiet: the chat list shows connection problems already.
        });
}

template <typename Work, typename Done>
void PeopleSession::action(Work work, Done done) {
    if (busy_) return;
    setBusy(true);
    setError({});
    setNotice({});
    runInBackground(
        pool_, this, std::move(work),
        [this, done = std::move(done)](auto&&... result) mutable {
            setBusy(false);
            done(std::forward<decltype(result)>(result)...);
            refresh();
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(failure.message);
        });
}

void PeopleSession::lookup(const QString& handle) {
    const QString trimmed = handle.trimmed();
    if (trimmed.isEmpty()) return;
    auto friendRepo = friendRepo_;
    const std::string h = trimmed.toStdString();
    const std::string self = identity_->userId;
    found_.clear();
    emit foundChanged();
    action([friendRepo, h, self]() { return friendRepo->lookup(h, self); },
           [this](data::ContactPreview preview) {
               bool isFriend = false;
               for (const auto& f : friends_) {
                   if (f.toMap().value(QStringLiteral("userId")).toString() == qs(preview.userId)) isFriend = true;
               }
               bool pending = false;
               for (const auto& r : outgoing_) {
                   if (r.toMap().value(QStringLiteral("userId")).toString() == qs(preview.userId)) pending = true;
               }
               found_ = QVariantMap{{QStringLiteral("userId"), qs(preview.userId)},
                                    {QStringLiteral("handle"), qs(preview.handle)},
                                    {QStringLiteral("avatarUrl"), qs(preview.avatarUrl.value_or(""))},
                                    {QStringLiteral("isSelf"), preview.isSelf},
                                    {QStringLiteral("deleted"), preview.deleted},
                                    {QStringLiteral("isFriend"), isFriend},
                                    {QStringLiteral("pending"), pending}};
               emit foundChanged();
           });
}

void PeopleSession::clearLookup() {
    found_.clear();
    emit foundChanged();
    clearMessages();
}

void PeopleSession::sendRequest(const QString& handle) {
    auto friendRepo = friendRepo_;
    const std::string h = handle.toStdString();
    action([friendRepo, h]() { friendRepo->sendRequest(h); },
           [this, handle]() {
               found_.clear();
               emit foundChanged();
               setNotice(QStringLiteral("Friend request sent to ") + handle + QStringLiteral("."));
           });
}

std::optional<QVariantMap> PeopleSession::findRequest(const QString& requestId) const {
    for (const auto& r : incoming_) {
        const auto map = r.toMap();
        if (map.value(QStringLiteral("id")).toString() == requestId) return map;
    }
    return std::nullopt;
}

void PeopleSession::accept(const QString& requestId) {
    const auto request = findRequest(requestId);
    if (!request) return;
    const data::FriendRequestView view{requestId.toStdString(),
                                       request->value(QStringLiteral("userId")).toString().toStdString(),
                                       request->value(QStringLiteral("handle")).toString().toStdString(), std::nullopt};
    auto friendRepo = friendRepo_;
    auto identity = identity_;
    const QString handle = qs(view.handle);
    action([friendRepo, identity, view]() { return friendRepo->accept(*identity, view); },
           [this, handle](std::string chatId) {
               setNotice(QStringLiteral("You and ") + handle + QStringLiteral(" are now friends."));
               emit chatCreated(qs(chatId));
           });
}

void PeopleSession::deny(const QString& requestId) {
    auto friendRepo = friendRepo_;
    const std::string id = requestId.toStdString();
    action([friendRepo, id]() { friendRepo->deny(id); }, []() {});
}

void PeopleSession::cancel(const QString& requestId) {
    auto friendRepo = friendRepo_;
    const std::string id = requestId.toStdString();
    action([friendRepo, id]() { friendRepo->cancel(id); }, []() {});
}

void PeopleSession::refreshBlocked() {
    auto api = api_;
    runInBackground(
        pool_, this,
        [api]() {
            QStringList ids;
            for (const auto& b : api->listBlocked()) ids.append(qs(b.userId));
            return ids;
        },
        [this](QStringList ids) {
            if (ids == blocked_) return;
            blocked_ = std::move(ids);
            emit blockedChanged();
        },
        [](const Failure&) {});
}

void PeopleSession::block(const QString& userId) {
    auto api = api_;
    const std::string id = userId.toStdString();
    action([api, id]() { api->block({id}); },
           [this]() {
               setNotice(QStringLiteral("Blocked. They can't message or call you."));
               refreshBlocked();
           });
}

void PeopleSession::unblock(const QString& userId) {
    auto api = api_;
    const std::string id = userId.toStdString();
    action([api, id]() { api->unblock(id); },
           [this]() {
               setNotice(QStringLiteral("Unblocked."));
               refreshBlocked();
           });
}

void PeopleSession::report(const QString& userId, const QString& reason) {
    if (reason.trimmed().isEmpty()) return setError(QStringLiteral("Say what happened."));
    auto api = api_;
    const std::string id = userId.toStdString();
    const std::string text = reason.trimmed().left(1000).toStdString();
    action([api, id, text]() { api->submitReport({id, text}); },
           [this]() { setNotice(QStringLiteral("Reported. Thanks: the moderators will look at it.")); });
}

void PeopleSession::clearMessages() {
    setError({});
    setNotice({});
}

void PeopleSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void PeopleSession::setError(const QString& error) {
    if (error_ == error) return;
    error_ = error;
    emit errorChanged();
}

void PeopleSession::setNotice(const QString& notice) {
    if (notice_ == notice) return;
    notice_ = notice;
    emit noticeChanged();
}

}  // namespace redvault::app
