#pragma once

#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/data/auth_repository.hpp"

Q_DECLARE_METATYPE(redvault::api::live::LiveEvent)

namespace redvault::app {

/**
 * The live channel (GET /api/live/events, a long poll on its own thread)
 * and what rides on it: call signaling (re-emitted for CallController),
 * typing indicators, and contacts' online status. The desktop counterpart
 * of the web app's state/LiveContext.tsx.
 *
 * Both typing and online status are opt-in and reciprocal: typing is only
 * sent, and others' only shown, while typing indicators are on here; the
 * server only reports who's online while you share your own status.
 */
class LiveSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(bool callsEnabled READ callsEnabled NOTIFY callsEnabledChanged)
    Q_PROPERTY(bool groupCallsEnabled READ groupCallsEnabled NOTIFY groupCallsEnabledChanged)
    Q_PROPERTY(QStringList online READ online NOTIFY onlineChanged)
    Q_PROPERTY(bool typingIndicators READ typingIndicators WRITE setTypingIndicators NOTIFY typingIndicatorsChanged)
    Q_PROPERTY(int typingRevision READ typingRevision NOTIFY typingChanged)

public:
    LiveSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, QObject* parent = nullptr);
    ~LiveSession() override;

    bool callsEnabled() const { return callsEnabled_; }
    bool groupCallsEnabled() const { return groupCallsEnabled_; }
    QStringList online() const { return online_; }
    bool typingIndicators() const { return typingIndicators_; }
    void setTypingIndicators(bool on);
    int typingRevision() const { return typingRevision_; }

    Q_INVOKABLE bool isOnline(const QString& userId) const { return online_.contains(userId); }
    /// userIds typing in this chat right now.
    Q_INVOKABLE QStringList typingIn(const QString& chatId) const;
    /// Call on every edit of the message box; throttled, and a no-op unless typing indicators are on.
    Q_INVOKABLE void notifyTyping(const QString& chatId);
    /// Whether this account shares its online status (the server's setting, from AccountSession).
    void setPresenceEnabled(bool on);

signals:
    void callsEnabledChanged();
    void groupCallsEnabledChanged();
    void onlineChanged();
    void typingIndicatorsChanged();
    void typingChanged();
    void liveEvent(const redvault::api::live::LiveEvent& event);

private:
    void deliver(api::live::LiveEvents events);
    void refreshPresence();
    void pruneTyping();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);
    QThreadPool pool_;
    QTimer presenceTimer_;
    QTimer typingTimer_;
    bool callsEnabled_ = false;
    bool groupCallsEnabled_ = false;
    bool typingIndicators_ = false;
    bool presenceEnabled_ = false;
    QStringList online_;
    std::map<QString, std::map<QString, qint64>> typing_;  // chatId -> userId -> shown until (ms)
    std::map<QString, qint64> lastTypingSent_;
    int typingRevision_ = 0;
};

}  // namespace redvault::app
