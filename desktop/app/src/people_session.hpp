#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

#include "async.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/friend_repository.hpp"

namespace redvault::app {

/**
 * Friends and friend requests (Android's ui/friends): look someone up by
 * handle, send a request, and answer or cancel pending ones. Polls pending
 * requests and the friends list every 5 s while signed in.
 */
class PeopleSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(QVariantList incoming READ incoming NOTIFY requestsChanged)
    Q_PROPERTY(QVariantList outgoing READ outgoing NOTIFY requestsChanged)
    Q_PROPERTY(QVariantList friends READ friends NOTIFY friendsChanged)
    Q_PROPERTY(QVariantMap found READ found NOTIFY foundChanged)
    /// userIds you've blocked (they can't message or call you).
    Q_PROPERTY(QStringList blocked READ blocked NOTIFY blockedChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    PeopleSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                  QObject* parent = nullptr);
    ~PeopleSession() override;

    QVariantList incoming() const { return incoming_; }
    QVariantList outgoing() const { return outgoing_; }
    QVariantList friends() const { return friends_; }
    QVariantMap found() const { return found_; }
    QStringList blocked() const { return blocked_; }
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QString notice() const { return notice_; }

    Q_INVOKABLE void refresh();
    /// Finds someone by handle: a read-only preview, nothing is created server-side.
    Q_INVOKABLE void lookup(const QString& handle);
    Q_INVOKABLE void clearLookup();
    Q_INVOKABLE void sendRequest(const QString& handle);
    Q_INVOKABLE void accept(const QString& requestId);
    Q_INVOKABLE void deny(const QString& requestId);
    Q_INVOKABLE void cancel(const QString& requestId);
    Q_INVOKABLE void clearMessages();
    Q_INVOKABLE void block(const QString& userId);
    Q_INVOKABLE void unblock(const QString& userId);
    /// Sends a report to the server's moderators (the reason is all they see besides the account).
    Q_INVOKABLE void report(const QString& userId, const QString& reason);
    Q_INVOKABLE void refreshBlocked();

signals:
    void requestsChanged();
    void friendsChanged();
    void foundChanged();
    void blockedChanged();
    void busyChanged();
    void errorChanged();
    void noticeChanged();
    /// Accepting a request created this chat; the chat list should show and open it.
    void chatCreated(const QString& chatId);
    void sessionExpired();

private:
    template <typename Work, typename Done>
    void action(Work work, Done done);
    void setBusy(bool busy);
    void setError(const QString& error);
    void setNotice(const QString& notice);
    std::optional<QVariantMap> findRequest(const QString& requestId) const;

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    std::shared_ptr<data::FriendRepository> friendRepo_;
    std::shared_ptr<data::GroupRepository> groupRepo_;
    QThreadPool pool_;
    QTimer timer_;
    bool refreshing_ = false;
    bool busy_ = false;
    QVariantList incoming_;
    QVariantList outgoing_;
    QVariantList friends_;
    QVariantMap found_;
    QStringList blocked_;
    QString error_;
    QString notice_;
};

}  // namespace redvault::app
