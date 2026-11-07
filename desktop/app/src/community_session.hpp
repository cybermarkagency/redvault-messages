#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <string>

#include "async.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/community_keys.hpp"

namespace redvault::app {

class ChatSession;

/**
 * Paid and free communities, the desktop counterpart of the web app's
 * screens/Communities.tsx, CommunityCreate.tsx, CreatorDashboard.tsx,
 * CommunityManage.tsx and CommunityReportDialog.tsx (docs/paid-communities.md).
 *
 * Everything shown is the server's JSON as is (the web app's DTO shapes), so
 * the QML reads the same fields the web screens do. Every amount comes from
 * the server; this app never decides a price, a commission or whether a
 * payment succeeded. Stripe (checkout, payouts onboarding, the customer
 * portal, the Express dashboard) opens in the system browser, and access
 * starts only once the server has Stripe's word for it.
 *
 * Also the key upkeep every member's app does (data/community_keys.hpp):
 * every 20 s it delivers the current key to members who are waiting and
 * rotates after someone left.
 */
class CommunitySession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    /// "discover", "mine", "memberships", "create", "creator", "community" or "manage".
    Q_PROPERTY(QString page READ page NOTIFY pageChanged)
    /// The community shown by "community" and "manage".
    Q_PROPERTY(QString communityId READ communityId NOTIFY pageChanged)
    Q_PROPERTY(QVariantMap config READ config NOTIFY configChanged)
    Q_PROPERTY(QString configError READ configError NOTIFY configChanged)
    /// [{value, label}] for the category pickers.
    Q_PROPERTY(QVariantList categories READ categories NOTIFY configChanged)
    // --- Lists ---
    Q_PROPERTY(QVariantList discover READ discover NOTIFY discoverChanged)
    Q_PROPERTY(bool discoverLoaded READ discoverLoaded NOTIFY discoverChanged)
    Q_PROPERTY(QVariantList mine READ mine NOTIFY mineChanged)
    Q_PROPERTY(bool mineLoaded READ mineLoaded NOTIFY mineChanged)
    Q_PROPERTY(QVariantList memberships READ memberships NOTIFY membershipsChanged)
    Q_PROPERTY(bool membershipsLoaded READ membershipsLoaded NOTIFY membershipsChanged)
    /// Back from Stripe's checkout: the subscription being confirmed (asked again every 3 s), or empty.
    Q_PROPERTY(QString confirming READ confirming NOTIFY membershipsChanged)
    /// Receipts shown, by subscription id.
    Q_PROPERTY(QVariantMap receipts READ receipts NOTIFY membershipsChanged)
    // --- Creator ---
    /// The CreatorDto, or null before becoming a creator (undefined until loaded: see creatorLoaded).
    Q_PROPERTY(QVariant creator READ creator NOTIFY creatorChanged)
    Q_PROPERTY(bool creatorLoaded READ creatorLoaded NOTIFY creatorChanged)
    Q_PROPERTY(QVariantMap dashboard READ dashboard NOTIFY dashboardChanged)
    Q_PROPERTY(bool dashboardLoaded READ dashboardLoaded NOTIFY dashboardChanged)
    // --- One community ---
    /// The CommunityPageDto of `communityId`; empty until it has loaded.
    Q_PROPERTY(QVariantMap community READ community NOTIFY communityChanged)
    Q_PROPERTY(QString communityError READ communityError NOTIFY communityChanged)
    Q_PROPERTY(QVariantList members READ members NOTIFY manageChanged)
    Q_PROPERTY(QString memberFilter READ memberFilter NOTIFY manageChanged)
    Q_PROPERTY(QVariantList invitations READ invitations NOTIFY manageChanged)
    /// A link just made: shown once, as only its hash is kept.
    Q_PROPERTY(QString createdInvite READ createdInvite NOTIFY manageChanged)
    Q_PROPERTY(QVariantList reports READ reports NOTIFY manageChanged)
    /// Goes up when this app uploads a picture, so screens showing it fetch the new one ("image://community/...").
    Q_PROPERTY(int imageRevision READ imageRevision NOTIFY imagesChanged)
    // --- Feedback ---
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// What just happened ("Saved.", "Report sent. Thank you.").
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)

public:
    CommunitySession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                     std::shared_ptr<data::CommunityKeyStore> keys, ChatSession* chats, QString webAppBase,
                     QObject* parent = nullptr);
    ~CommunitySession() override;

    QString page() const { return page_; }
    QString communityId() const { return QString::fromStdString(communityId_); }
    QVariantMap config() const { return config_; }
    QString configError() const { return configError_; }
    QVariantList categories() const;
    QVariantList discover() const { return discover_; }
    bool discoverLoaded() const { return discoverLoaded_; }
    QVariantList mine() const { return mine_; }
    bool mineLoaded() const { return mineLoaded_; }
    QVariantList memberships() const { return memberships_; }
    bool membershipsLoaded() const { return membershipsLoaded_; }
    QString confirming() const { return confirming_; }
    QVariantMap receipts() const { return receipts_; }
    QVariant creator() const { return creator_; }
    bool creatorLoaded() const { return creatorLoaded_; }
    QVariantMap dashboard() const { return dashboard_; }
    bool dashboardLoaded() const { return dashboardLoaded_; }
    QVariantMap community() const { return community_; }
    QString communityError() const { return communityError_; }
    QVariantList members() const { return members_; }
    QString memberFilter() const { return memberFilter_; }
    QVariantList invitations() const { return invitations_; }
    QString createdInvite() const { return createdInvite_; }
    QVariantList reports() const { return reports_; }
    int imageRevision() const { return imageRevision_; }
    bool busy() const { return busy_; }
    QString notice() const { return notice_; }
    QString error() const { return error_; }

    /// Shows a page (and loads what it needs). `communityId` for "community" and "manage"; `invite` is a link's token.
    Q_INVOKABLE void open(const QString& page, const QString& communityId = {}, const QString& invite = {});
    Q_INVOKABLE void clearMessages();

    // --- Discover, mine, memberships ---
    Q_INVOKABLE void search(const QString& query, const QString& category);
    Q_INVOKABLE void refreshMine();
    Q_INVOKABLE void refreshMemberships();
    Q_INVOKABLE void cancelMembership(const QString& subscriptionId);
    Q_INVOKABLE void resumeMembership(const QString& subscriptionId);
    /// Stripe's customer portal (payment method), in the browser.
    Q_INVOKABLE void openPortal(const QString& subscriptionId);
    Q_INVOKABLE void toggleReceipts(const QString& subscriptionId);
    /// Asks the server (which asks Stripe) about the checkout being confirmed, now.
    Q_INVOKABLE void checkConfirmation();

    // --- One community ---
    Q_INVOKABLE void reloadCommunity();
    Q_INVOKABLE void join();
    Q_INVOKABLE void leave();
    /**
     * Starts Stripe Checkout in the browser, with the renewal terms and 18+
     * as ticked (the server refuses without them). Access starts only once
     * Stripe confirms the payment to the server; Memberships keeps asking.
     */
    Q_INVOKABLE void subscribe(bool termsAccepted, bool ageConfirmed);
    /// Opens the community's chat (once the chat list has it).
    Q_INVOKABLE void openChat();
    /**
     * A report. For a message, `share` is the reporter's own choice to
     * disclose its text; nothing else of it leaves this computer.
     * `targetType` is "community" or "content".
     */
    Q_INVOKABLE void report(const QString& communityId, const QString& targetType, const QString& targetUserId, const QString& category,
                            const QString& details, const QString& messageId, const QString& messageText, bool share);

    // --- Create ---
    /// `fields` as the web app's create form: kind, accessType, name, description, category, visibility, price (dollars), rules,
    /// previewText, memberLimit, joinApproval, slowModeSeconds. Epoch 0's key is made here, sealed to you.
    Q_INVOKABLE void create(const QVariantMap& fields);

    // --- Creator ---
    Q_INVOKABLE void loadCreator();
    Q_INVOKABLE void becomeCreator(const QString& handle, const QString& displayName, const QString& bio, const QString& link);
    Q_INVOKABLE void saveCreatorProfile(const QString& displayName, const QString& bio, const QString& link);
    /// A PNG, JPEG or WebP up to 300 KB.
    Q_INVOKABLE void setCreatorPicture(const QUrl& file);
    Q_INVOKABLE void setPaidMemberships(bool enabled);
    /// Stripe-hosted onboarding, in the browser. RedVault never sees identity or bank details.
    Q_INVOKABLE void startOnboarding(const QString& country);
    Q_INVOKABLE void refreshOnboarding();
    Q_INVOKABLE void openStripeDashboard();
    Q_INVOKABLE void loadDashboard();

    // --- Manage ---
    Q_INVOKABLE void publish();
    Q_INVOKABLE void unpublish();
    Q_INVOKABLE void closeCommunity();
    /// One round of key upkeep now (deliver keys to waiting members, rotate if due).
    Q_INVOKABLE void deliverKeysNow();
    /// `fields` as the web app's settings form; sent with the version last loaded (optimistic locking).
    Q_INVOKABLE void saveSettings(const QVariantMap& fields);
    /// "avatar" (300 KB) or "banner" (800 KB).
    Q_INVOKABLE void setCommunityImage(const QString& kind, const QUrl& file);
    Q_INVOKABLE void setPrice(const QString& dollars);
    Q_INVOKABLE void loadMembers(const QString& status);
    Q_INVOKABLE void setMemberRole(const QString& userId, const QString& role);
    Q_INVOKABLE void setMemberCanPost(const QString& userId, bool canPost);
    Q_INVOKABLE void removeMember(const QString& userId, bool ban, const QString& reason);
    Q_INVOKABLE void unban(const QString& userId);
    Q_INVOKABLE void decideRequest(const QString& userId, bool approve);
    Q_INVOKABLE void loadInvitations();
    /// `maxUses` 0 = unlimited; `expiresInDays` 0 = never.
    Q_INVOKABLE void createInvitation(int maxUses, int expiresInDays);
    Q_INVOKABLE void revokeInvitation(const QString& invitationId);
    Q_INVOKABLE void loadReports();
    Q_INVOKABLE void resolveReport(const QString& reportId, const QString& status);

    // --- Pictures and links ---
    /**
     * Where a community's "avatar" or "banner" is served, for the "community"
     * image provider; with the invite token when it's the community opened
     * through an invite link. Empty for anything else.
     */
    Q_INVOKABLE QString imagePath(const QString& kind, const QString& communityId) const;
    Q_INVOKABLE QString creatorAvatarPath(const QString& creatorId) const;
    /// A community link (<site>/?community=<id>&invite=<token>) from anywhere, as pasted into Discover.
    Q_INVOKABLE bool isCommunityLink(const QString& text) const;
    /// A community link to this app's own site, as clicked in a chat (others open in the browser).
    Q_INVOKABLE bool isOwnCommunityLink(const QString& text) const;
    /// Opens the community a link points at, with its invitation, on the Communities page.
    Q_INVOKABLE void openLink(const QString& text);

    // --- Helpers for the screens ---
    /// "$10.00" from minor units.
    Q_INVOKABLE QString formatMoney(double amountMinor, const QString& currency = QStringLiteral("usd")) const;
    /// "12%" / "12.5%" from basis points.
    Q_INVOKABLE QString formatRate(int bps) const;
    /// "19 Oct 2026", or "—".
    Q_INVOKABLE QString formatDate(const QString& iso) const;
    /// Dollars typed by a person → cents, or -1 if it isn't a plain amount (like the web app's parseDollars).
    Q_INVOKABLE int parseDollars(const QString& text) const;
    /// RedVault's share of a price in cents, rounded half up, from the commission in the config.
    Q_INVOKABLE int commissionFor(int cents) const;
    Q_INVOKABLE QString categoryLabel(const QString& category) const;
    Q_INVOKABLE void copyText(const QString& text);

signals:
    void pageChanged();
    void configChanged();
    void discoverChanged();
    void mineChanged();
    void membershipsChanged();
    void creatorChanged();
    void dashboardChanged();
    void communityChanged();
    void manageChanged();
    void busyChanged();
    void messagesChanged();
    void imagesChanged();
    /// Show the Chats page: a community's chat is being opened there.
    void showChatRequested();
    /// Show the Communities page (a community link was opened elsewhere).
    void showRequested();
    /// A report went out (`error` empty) or didn't.
    void reported(const QString& error);
    void sessionExpired();

private:
    template <typename Work, typename Done>
    void act(Work work, Done done, const QString& fallback = {});
    void setBusy(bool busy);
    void setMessages(const QString& error, const QString& notice);
    void fail(const Failure& failure, const QString& fallback = {});
    void runUpkeep(bool reportResult);
    void pollConfirmation();
    void loadConfig();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    std::shared_ptr<data::CommunityKeyStore> keys_;
    QPointer<ChatSession> chats_;
    QString webAppBase_;
    QThreadPool pool_;
    QTimer upkeepTimer_;
    QTimer confirmTimer_;
    bool upkeepRunning_ = false;
    int confirmTries_ = 0;

    QString page_ = QStringLiteral("discover");
    std::string communityId_;
    std::string invite_;
    int communityGeneration_ = 0;

    QVariantMap config_;
    QString configError_;
    QVariantList discover_;
    bool discoverLoaded_ = false;
    int discoverGeneration_ = 0;
    QVariantList mine_;
    bool mineLoaded_ = false;
    QVariantList memberships_;
    bool membershipsLoaded_ = false;
    QString confirming_;
    QVariantMap receipts_;
    QVariant creator_;
    bool creatorLoaded_ = false;
    QVariantMap dashboard_;
    bool dashboardLoaded_ = false;
    QVariantMap community_;
    QString communityError_;
    QVariantList members_;
    QString memberFilter_ = QStringLiteral("active");
    QVariantList invitations_;
    QString createdInvite_;
    QVariantList reports_;
    int imageRevision_ = 0;
    bool busy_ = false;
    QString notice_;
    QString error_;
};

}  // namespace redvault::app
