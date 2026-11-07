#include "live_session.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <chrono>
#include <thread>

#include "async.hpp"
#include "prefs.hpp"

namespace redvault::app {

namespace {
constexpr qint64 kTypingShownMs = 6000;
constexpr qint64 kTypingSendEveryMs = 3000;
constexpr int kPresenceEveryMs = 20000;
}  // namespace

LiveSession::LiveSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)) {
    qRegisterMetaType<api::live::LiveEvent>();
    pool_.setMaxThreadCount(2);
    typingIndicators_ = prefs::typingIndicators(identity_->userId);

    presenceTimer_.setInterval(kPresenceEveryMs);
    connect(&presenceTimer_, &QTimer::timeout, this, &LiveSession::refreshPresence);
    typingTimer_.setInterval(1000);
    connect(&typingTimer_, &QTimer::timeout, this, &LiveSession::pruneTyping);

    // The long poll gets a thread of its own: each request waits up to 25 s.
    // It holds only shared pointers, so it can outlive this object and just
    // stops at its next turn once `stop_` is set.
    QPointer<LiveSession> self(this);
    std::thread([api = api_, stop = stop_, self] {
        std::optional<std::int64_t> cursor;
        int failures = 0;
        while (!*stop) {
            try {
                auto result = api::live::events(*api, cursor);
                cursor = result.cursor;
                failures = 0;
                if (*stop) return;
                QMetaObject::invokeMethod(
                    qApp,
                    [self, result = std::move(result)]() mutable {
                        if (self) self->deliver(std::move(result));
                    },
                    Qt::QueuedConnection);
            } catch (const api::ApiError& e) {
                // Signed out, or a server without the live channel: stop quietly.
                if (e.status() == 401 || e.status() == 404) return;
                ++failures;
            } catch (...) {
                ++failures;
            }
            if (failures > 0) {
                const auto wait = std::chrono::milliseconds(std::min(30000, 2000 * failures));
                for (auto waited = std::chrono::milliseconds(0); waited < wait && !*stop; waited += std::chrono::milliseconds(200)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        }
    }).detach();
}

LiveSession::~LiveSession() {
    *stop_ = true;
    pool_.waitForDone(2000);
}

void LiveSession::deliver(api::live::LiveEvents result) {
    if (callsEnabled_ != result.callsEnabled) {
        callsEnabled_ = result.callsEnabled;
        emit callsEnabledChanged();
    }
    if (groupCallsEnabled_ != result.groupCallsEnabled) {
        groupCallsEnabled_ = result.groupCallsEnabled;
        emit groupCallsEnabledChanged();
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool typingChangedNow = false;
    for (auto& event : result.events) {
        if (event.type == "typing") {
            if (!typingIndicators_ || !event.conversationId || !event.userId) continue;
            typing_[QString::fromStdString(*event.conversationId)][QString::fromStdString(*event.userId)] = now + kTypingShownMs;
            typingChangedNow = true;
            continue;
        }
        emit liveEvent(event);
    }
    if (typingChangedNow) {
        ++typingRevision_;
        emit typingChanged();
        if (!typingTimer_.isActive()) typingTimer_.start();
    }
}

QStringList LiveSession::typingIn(const QString& chatId) const {
    QStringList out;
    const auto it = typing_.find(chatId);
    if (it == typing_.end()) return out;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& [userId, until] : it->second) {
        if (until > now) out.append(userId);
    }
    return out;
}

void LiveSession::pruneTyping() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (auto it = typing_.begin(); it != typing_.end();) {
        for (auto u = it->second.begin(); u != it->second.end();) {
            if (u->second <= now) {
                u = it->second.erase(u);
                changed = true;
            } else {
                ++u;
            }
        }
        it = it->second.empty() ? typing_.erase(it) : std::next(it);
    }
    if (typing_.empty()) typingTimer_.stop();
    if (changed) {
        ++typingRevision_;
        emit typingChanged();
    }
}

void LiveSession::notifyTyping(const QString& chatId) {
    if (!typingIndicators_ || chatId.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastTypingSent_[chatId] < kTypingSendEveryMs) return;
    lastTypingSent_[chatId] = now;
    auto api = api_;
    const std::string id = chatId.toStdString();
    runInBackground(pool_, this, [api, id] { api::live::sendTyping(*api, id); }, [] {}, [](const Failure&) {});
}

void LiveSession::setTypingIndicators(bool on) {
    if (on == typingIndicators_) return;
    typingIndicators_ = on;
    prefs::setFlag(identity_->userId, "typingIndicators", on);
    if (!on) {
        typing_.clear();
        ++typingRevision_;
        emit typingChanged();
    }
    emit typingIndicatorsChanged();
}

void LiveSession::setPresenceEnabled(bool on) {
    presenceEnabled_ = on;
    if (on) {
        refreshPresence();
        presenceTimer_.start();
    } else {
        presenceTimer_.stop();
        if (!online_.isEmpty()) {
            online_.clear();
            emit onlineChanged();
        }
    }
}

void LiveSession::refreshPresence() {
    if (!presenceEnabled_) return;
    auto api = api_;
    runInBackground(
        pool_, this, [api] { return api::live::presence(*api); },
        [this](std::vector<std::string> ids) {
            QStringList next;
            for (const auto& id : ids) next.append(QString::fromStdString(id));
            next.sort();
            if (next != online_) {
                online_ = next;
                emit onlineChanged();
            }
        },
        [](const Failure&) {});
}

}  // namespace redvault::app
