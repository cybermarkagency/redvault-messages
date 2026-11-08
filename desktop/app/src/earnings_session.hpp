// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "async.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/api/earnings_api.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/model/earnings.hpp"

namespace redvault::app {

class ChatSession;

namespace earnings_detail {
/// An exclusive post as this account sees it, opened when it can be (defined in earnings_session.cpp).
struct Opened;
}  // namespace earnings_detail

/**
 * Paid messages, tips and exclusive posts: the desktop counterpart of the web
 * app's components/earnings and screens/CreatorEarnings.tsx.
 *
 * Paying happens on Stripe's page in the browser; this app then asks the
 * server until IT says the purchase is paid, which it only does after the
 * provider's verified webhook. Nothing here decides a price or claims a
 * payment. Messages stay end-to-end encrypted either way. Posts are
 * encrypted with a key per post (crypto/post_crypto.hpp); while the app is
 * open, a creator's copy seals that key to each confirmed buyer.
 */
class EarningsSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    /// Paid messages, tips and posts are switched on on this server.
    Q_PROPERTY(bool enabled READ enabled NOTIFY configChanged)
    /// Buying works from this desktop app (the server says where it does).
    Q_PROPERTY(bool canBuy READ canBuy NOTIFY configChanged)
    Q_PROPERTY(bool postMedia READ postMedia NOTIFY configChanged)
    /// The tip buttons: [{minor, label}].
    Q_PROPERTY(QVariantList tipPresets READ tipPresets NOTIFY configChanged)
    /// RedVault's share of each, in percent, and the tip range: {paidMessage, tip, post, tipMin, tipMax}.
    Q_PROPERTY(QVariantMap rates READ rates NOTIFY configChanged)
    // --- Paid messages in the open 1:1 chat ---
    /// {required, mode, price, purchasable, paid, safetyAvailable}; required false when sending is free.
    Q_PROPERTY(QVariantMap quote READ quote NOTIFY quoteChanged)
    /// The next send is the free safety contact.
    Q_PROPERTY(bool safety READ safety WRITE setSafety NOTIFY quoteChanged)
    // --- A payment underway (one at a time) ---
    Q_PROPERTY(bool paying READ paying NOTIFY payChanged)
    /// "paid_message", "tip" or "exclusive_post": what's being paid for.
    Q_PROPERTY(QString payContext READ payContext NOTIFY payChanged)
    Q_PROPERTY(QString payStatus READ payStatus NOTIFY payChanged)
    Q_PROPERTY(QString payError READ payError NOTIFY payChanged)
    /// What `payError` is about (a payContext value), and the post or person being paid.
    Q_PROPERTY(QString payErrorContext READ payErrorContext NOTIFY payChanged)
    Q_PROPERTY(QString payTarget READ payTarget NOTIFY payChanged)
    /// The last finished payment's product (cleared by clearPayment), for a "thank you".
    Q_PROPERTY(QString paidProduct READ paidProduct NOTIFY payChanged)
    // --- Your side, as a creator ---
    /// {available, creatorReady, mode, priceMinor, priceText, tipsEnabled, acceptsMessageRequests, exemptCount}.
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(bool chargesForMessages READ chargesForMessages NOTIFY settingsChanged)
    /// {loaded, available, creatorReady, products: [...], gross, fees, net, transactions: [...], awaitingKeys, balance, payouts: [...]}.
    Q_PROPERTY(QVariantMap dashboard READ dashboard NOTIFY dashboardChanged)
    Q_PROPERTY(QVariantList tips READ tips NOTIFY dashboardChanged)
    /// Your exclusive posts and the ones you bought: [post].
    Q_PROPERTY(QVariantList myPosts READ myPosts NOTIFY postsChanged)
    Q_PROPERTY(QVariantList library READ library NOTIFY postsChanged)
    Q_PROPERTY(bool libraryLoaded READ libraryLoaded NOTIFY postsChanged)
    /// "", "encrypting" or "uploading".
    Q_PROPERTY(QString publishStage READ publishStage NOTIFY postsChanged)
    /// The creator profile and payout setup: {loaded, exists, handle, displayName, paymentsEnabled, status, requirementsDue, ready, countries: [{code, name}], termsVersion}.
    Q_PROPERTY(QVariantMap creator READ creator NOTIFY creatorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY messagesChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)

public:
    EarningsSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                    QObject* parent = nullptr);
    ~EarningsSession() override;

    bool enabled() const { return config_.enabled; }
    bool canBuy() const;
    bool postMedia() const { return config_.postMedia; }
    QVariantList tipPresets() const;
    QVariantMap rates() const;
    QVariantMap quote() const;
    bool safety() const { return safety_; }
    void setSafety(bool on);
    bool paying() const { return !payContext_.isEmpty(); }
    QString payContext() const { return payContext_; }
    QString payStatus() const { return payStatus_; }
    QString payError() const { return payError_; }
    QString payErrorContext() const { return payErrorContext_; }
    QString payTarget() const { return payTarget_; }
    QString paidProduct() const { return paidProduct_; }
    QVariantMap settings() const;
    bool chargesForMessages() const { return settings_.paidMessagesMode != "off"; }
    QVariantMap dashboard() const { return dashboard_; }
    QVariantList tips() const { return tips_; }
    QVariantList myPosts() const;
    QVariantList library() const;
    bool libraryLoaded() const { return libraryLoaded_; }
    QString publishStage() const { return publishStage_; }
    QVariantMap creator() const;
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QString notice() const { return notice_; }

    /// The server's switches and your settings.
    Q_INVOKABLE void refresh();
    /// What sending to the open chat costs now.
    Q_INVOKABLE void refreshQuote();
    /// What the next send needs: {ok, entitlementId, safetyContact}; ok false means pay first.
    Q_INVOKABLE QVariantMap sendOptions() const;

    // --- Paying (Stripe's page opens in the browser) ---
    Q_INVOKABLE void payForMessage();
    /// A voluntary tip; `amountMinor` from tipPresets or parseAmount.
    Q_INVOKABLE void tip(const QString& creatorUserId, int amountMinor, bool showName);
    Q_INVOKABLE void buyPost(const QString& postId);
    /// Stops waiting here (a payment finished on Stripe's page still counts and shows up later).
    Q_INVOKABLE void cancelPayment();
    Q_INVOKABLE void clearPayment();
    /// "7.50" → 750, or -1 when it isn't an amount.
    Q_INVOKABLE int parseAmount(const QString& text) const;
    Q_INVOKABLE QString formatAmount(int minor) const;

    // --- Someone's profile ---
    /// What they offer (answers with offerLoaded): {available, handle, paidMessages, tips, posts: [post]}.
    Q_INVOKABLE void loadOffer(const QString& userId);
    Q_INVOKABLE bool isExempt(const QString& userId) const;
    /// Lets someone message you free (or charges them again).
    Q_INVOKABLE void setExempt(const QString& userId, bool exempt);
    /// Decrypts a post's photo or video; answers with postMediaReady or postMediaFailed.
    Q_INVOKABLE void loadPostMedia(const QString& postId);

    // --- Your dashboard ---
    Q_INVOKABLE void loadDashboard();
    Q_INVOKABLE void loadTips();
    Q_INVOKABLE void loadMyPosts();
    Q_INVOKABLE void loadLibrary();
    /// `mode` "off", "first_contact" or "per_message"; `priceText` like "3.00".
    Q_INVOKABLE void saveMessageSettings(const QString& mode, const QString& priceText);
    Q_INVOKABLE void setTipsEnabled(bool on);
    /// Checks a picked file before publishing: {kind: "image"|"video", name, error}.
    Q_INVOKABLE QVariantMap checkPostFile(const QUrl& file) const;
    /// Encrypts here and publishes; `file` optional.
    Q_INVOKABLE void publishPost(const QString& text, const QString& teaser, const QString& priceText, const QUrl& file);
    Q_INVOKABLE void unpublishPost(const QString& postId);
    /// A full refund of one of your sales.
    Q_INVOKABLE void refund(const QString& purchaseId);

    // --- Creator profile and payouts (shared with paid communities) ---
    Q_INVOKABLE void loadCreator();
    Q_INVOKABLE void becomeCreator(const QString& handle, const QString& displayName);
    /// Opens Stripe's onboarding in the browser.
    Q_INVOKABLE void startPayoutSetup(const QString& country);
    Q_INVOKABLE void refreshPayoutSetup();
    Q_INVOKABLE void openStripeDashboard();
    /// Seals post keys to confirmed buyers now (it also happens every minute while the app is open).
    Q_INVOKABLE void deliverKeys();
    Q_INVOKABLE void clearMessages();

signals:
    void configChanged();
    void quoteChanged();
    void payChanged();
    void settingsChanged();
    void dashboardChanged();
    void postsChanged();
    void creatorChanged();
    void messagesChanged();
    void offerLoaded(const QString& userId, const QVariantMap& offer);
    void postMediaReady(const QString& postId, const QString& url, bool video);
    void postMediaFailed(const QString& postId);
    /// A post went up: the form clears.
    void published();
    void sessionExpired();

private:
    using Opened = earnings_detail::Opened;
    using OpenedPtr = std::shared_ptr<Opened>;

    void onChatChanged();
    void startPayment(api::earnings::StartPurchase input, const QString& context, const std::string& target);
    void poll();
    void finishPayment(const QString& product);
    void failPayment(const QString& message);
    /// Fetches posts and opens what this account can read (on the pool), then calls back on this thread.
    void loadPosts(std::function<std::vector<api::earnings::Post>(api::ApiClient&)> fetch, std::function<void(std::vector<OpenedPtr>)> done);
    QVariantMap postMap(const Opened& post) const;
    void setBusy(bool busy);
    void setMessages(const QString& error, const QString& notice);
    void fail(const Failure& failure, const QString& fallback);
    void dropTempFiles();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    ChatSession* chats_;
    QThreadPool pool_;

    api::earnings::Config config_;
    api::earnings::Settings settings_;
    bool settingsLoaded_ = false;

    std::string chatId_;
    std::string chatUserId_;
    std::optional<api::earnings::Quote> quote_;
    bool safety_ = false;
    int quoteGeneration_ = 0;

    QString payContext_;
    QString payStatus_;
    QString payError_;
    QString payErrorContext_;
    QString payTarget_;
    QString paidProduct_;
    std::string pendingPurchaseId_;
    std::int64_t pendingStartedMs_ = 0;
    std::function<void()> onPaid_;
    QTimer pollTimer_;
    /// A start that got no answer is retried with the same key, so it can't open a second checkout.
    std::string retryTarget_;
    std::string retryKey_;

    QVariantMap dashboard_;
    QVariantList tips_;
    std::vector<OpenedPtr> myPosts_;
    std::vector<OpenedPtr> library_;
    bool libraryLoaded_ = false;
    /// Every post opened this session, for its media.
    std::map<std::string, OpenedPtr> known_;
    QString publishStage_;

    std::optional<api::earnings::Creator> creator_;
    bool creatorLoaded_ = false;
    api::earnings::CreatorSetup creatorSetup_;

    bool busy_ = false;
    QString error_;
    QString notice_;

    QTimer keyTimer_;
    bool delivering_ = false;
    QStringList tempFiles_;
};

}  // namespace redvault::app
