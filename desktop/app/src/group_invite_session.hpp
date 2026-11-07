#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/api/group_invite_api.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/data/auth_repository.hpp"

namespace redvault::app {

class ChatSession;

/**
 * Group invite links, the desktop counterpart of the web app's
 * screens/GroupInviteLink.tsx and JoinGroup.tsx (crypto in
 * core/crypto/group_invite_crypto.hpp, byte-compatible with the web app):
 *
 *   - Group info > Invite link, for the group load() was given: every member
 *     can copy the link; the owner and admins turn it on or off, reset it,
 *     choose "Approve new members", and answer requests to join.
 *   - Joining: a pasted link (Chats > Join a group with a link, or Calls >
 *     Join a call link) or one clicked in a chat opens the join screen, with
 *     the group's name and size, and Join group / Ask to join / Cancel request.
 *
 * The link's secret never leaves this computer: only the auth token it
 * derives and what it encrypted go to the server.
 */
class GroupInviteSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    // --- Group info > Invite link ---
    /// "loading", "none" (no link yet) or "on".
    Q_PROPERTY(QString linkState READ linkState NOTIFY linkChanged)
    /// The link to share; empty when it couldn't be opened on this computer.
    Q_PROPERTY(QString linkUrl READ linkUrl NOTIFY linkChanged)
    /// An admin lets in each person who opens the link.
    Q_PROPERTY(bool approval READ approval NOTIFY linkChanged)
    /// Owner or admin of the loaded group.
    Q_PROPERTY(bool canManage READ canManage NOTIFY linkChanged)
    /// Waiting to join, oldest first: [{userId, handle, avatarUrl}]. Owners and admins only.
    Q_PROPERTY(QVariantList requests READ requests NOTIFY requestsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // --- Joining ---
    /// "paste" (asking for a link), "opening", "ready" (the preview is shown) or "error".
    Q_PROPERTY(QString joinState READ joinState NOTIFY joinChanged)
    Q_PROPERTY(QString joinName READ joinName NOTIFY joinChanged)
    Q_PROPERTY(int joinMemberCount READ joinMemberCount NOTIFY joinChanged)
    Q_PROPERTY(bool joinApproval READ joinApproval NOTIFY joinChanged)
    /// Already in the group.
    Q_PROPERTY(bool joinMember READ joinMember NOTIFY joinChanged)
    /// Asked to join, waiting for an admin.
    Q_PROPERTY(bool joinRequested READ joinRequested NOTIFY joinChanged)
    Q_PROPERTY(QString joinError READ joinError NOTIFY joinChanged)
    Q_PROPERTY(bool joinBusy READ joinBusy NOTIFY joinChanged)

public:
    GroupInviteSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                       QString webAppBase, QObject* parent = nullptr);
    ~GroupInviteSession() override;

    QString linkState() const { return linkState_; }
    QString linkUrl() const { return linkUrl_; }
    bool approval() const { return approval_; }
    bool canManage() const;
    QVariantList requests() const { return requests_; }
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QString joinState() const { return join_.state; }
    QString joinName() const { return join_.name; }
    int joinMemberCount() const { return join_.memberCount; }
    bool joinApproval() const { return join_.approval; }
    bool joinMember() const { return join_.member; }
    bool joinRequested() const { return join_.requested; }
    QString joinError() const { return join_.error; }
    bool joinBusy() const { return join_.busy; }

    // --- Group info > Invite link (the group `chatId`) ---
    Q_INVOKABLE void load(const QString& chatId);
    /// Turns the link on, asking new members to be approved (as the web app starts it).
    Q_INVOKABLE void createLink();
    /// A new link with the same approval setting: the current one stops working.
    Q_INVOKABLE void resetLink();
    Q_INVOKABLE void setApproval(bool on);
    /// The link stops working, and pending requests are dropped.
    Q_INVOKABLE void turnOff();
    /// Lets them in, sealing them their own copy of the group key.
    Q_INVOKABLE void approve(const QString& userId);
    Q_INVOKABLE void decline(const QString& userId);
    Q_INVOKABLE void copyLink();
    Q_INVOKABLE void clearError();

    // --- Joining ---
    /// The shape of a group invite link (…?invite=<id>#<secret>), in a paste or a whole message.
    Q_INVOKABLE bool isInviteLink(const QString& text) const;
    /// A group invite link on this app's own site: clicked in a chat, it opens here rather than in a browser.
    Q_INVOKABLE bool isOwnInviteLink(const QString& text) const;
    /// Shows the join screen asking for a link.
    Q_INVOKABLE void startPaste();
    /// Opens the join screen for a link. Returns what's wrong with it, or empty when it's opening.
    Q_INVOKABLE QString openInvite(const QString& text);
    /// Join group (with the key sealed to yourself) or Ask to join.
    Q_INVOKABLE void join();
    Q_INVOKABLE void cancelRequest();
    /// Already a member: opens the group.
    Q_INVOKABLE void openGroup();
    Q_INVOKABLE void closeJoin();

signals:
    void linkChanged();
    void requestsChanged();
    void busyChanged();
    void errorChanged();
    void joinChanged();
    /// Show the join screen.
    void showJoin();
    /// Joined (or already in): the group is opening in Chats, so the join screen can close.
    void joined();
    void copied();
    void sessionExpired();

private:
    struct Join {
        QString state;
        std::string linkId;
        std::shared_ptr<crypto::SecretBytes> secret;
        std::shared_ptr<crypto::SecretBytes> groupKey;
        std::string conversationId;
        QString name;
        int memberCount = 0;
        bool approval = false;
        bool member = false;
        bool requested = false;
        QString error;
        bool busy = false;
    };

    /// Shows a link (or none) from the server: opens its secret with the group key.
    void showLink(const std::string& chatId, const std::optional<api::invites::Link>& link);
    void loadRequests();
    /// A change to the loaded group's link: `work` returns the link the server now has (an empty map for none).
    template <typename Work>
    void linkAction(Work work, const QString& failure);
    void setBusy(bool busy);
    void setError(const QString& error);
    void setJoin(Join join);
    void failed(int status);

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    ChatSession* chats_;
    QString webAppBase_;
    QThreadPool pool_;

    std::string chatId_;
    // Bumped by load(): answers for a group no longer shown are dropped.
    int generation_ = 0;
    QString linkState_ = QStringLiteral("loading");
    QString linkUrl_;
    bool approval_ = false;
    std::shared_ptr<crypto::SecretBytes> linkSecret_;
    QVariantList requests_;
    bool busy_ = false;
    QString error_;
    Join join_;
    int joinGeneration_ = 0;
};

}  // namespace redvault::app
