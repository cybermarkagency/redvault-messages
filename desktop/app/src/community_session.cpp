// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "community_session.hpp"

#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

#include "chat_session.hpp"
#include "redvault/api/community_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/community_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace {

namespace community_api = api::community;
using Json = api::Json;

constexpr int kUpkeepMs = 20'000;
constexpr int kFirstUpkeepMs = 3'000;
constexpr int kConfirmMs = 3'000;
// About two minutes of asking after a checkout before leaving it to the Memberships page.
constexpr int kConfirmTries = 40;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

/// The server's JSON as QML sees it: objects become maps, arrays lists, null an invalid QVariant.
QVariant toVariant(const Json& j) {
    if (j.is_null()) return {};
    if (j.is_object() || j.is_array()) return QJsonDocument::fromJson(QByteArray::fromStdString(j.dump())).toVariant();
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) return qs(j.get<std::string>());
    return {};
}

std::optional<std::string> optionalText(const QString& text) {
    const QString trimmed = text.trimmed();
    return trimmed.isEmpty() ? std::nullopt : std::optional<std::string>(trimmed.toStdString());
}

/// A picked image as base64, or a UserFacingError when it's too big or unreadable.
std::string imageBase64(const QUrl& file, qint64 maxBytes) {
    QFile in(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!in.open(QIODevice::ReadOnly)) throw api::UserFacingError("Couldn't read that picture.");
    if (in.size() > maxBytes) {
        throw api::UserFacingError("Images can be at most " + std::to_string(maxBytes / 1024) + " KB.");
    }
    const QByteArray bytes = in.readAll();
    return crypto::base64Encode(crypto::ByteView(reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())));
}

/// Stripe and the server's own pages only ever arrive as http(s) links; anything else isn't opened.
void openInBrowser(const std::string& url) {
    const QUrl target(qs(url));
    if (target.scheme() == QStringLiteral("https") || target.scheme() == QStringLiteral("http")) QDesktopServices::openUrl(target);
}

const QMap<QString, QString>& categoryLabels() {
    static const QMap<QString, QString> labels{
        {"art_design", "Art & design"},
        {"business", "Business"},
        {"education", "Education"},
        {"entertainment", "Entertainment"},
        {"gaming", "Gaming"},
        {"health_fitness", "Health & fitness"},
        {"lifestyle", "Lifestyle"},
        {"music", "Music"},
        {"news", "News"},
        {"science_technology", "Science & technology"},
        {"sports", "Sports"},
        {"writing", "Writing"},
        {"other", "Other"},
    };
    return labels;
}

}  // namespace

CommunitySession::CommunitySession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                                   std::shared_ptr<data::CommunityKeyStore> keys, ChatSession* chats, QString webAppBase,
                                   QObject* parent)
    : QObject(parent),
      api_(std::move(api)),
      identity_(std::move(identity)),
      keys_(std::move(keys)),
      chats_(chats),
      webAppBase_(std::move(webAppBase)) {
    pool_.setMaxThreadCount(3);
    // Key upkeep: soon after signing in, then every 20 s, like the web app.
    upkeepTimer_.setInterval(kUpkeepMs);
    connect(&upkeepTimer_, &QTimer::timeout, this, [this] { runUpkeep(false); });
    upkeepTimer_.start();
    QTimer::singleShot(kFirstUpkeepMs, this, [this] { runUpkeep(false); });
    confirmTimer_.setInterval(kConfirmMs);
    connect(&confirmTimer_, &QTimer::timeout, this, &CommunitySession::pollConfirmation);
}

CommunitySession::~CommunitySession() {
    upkeepTimer_.stop();
    confirmTimer_.stop();
    pool_.clear();
    pool_.waitForDone();
}

// --- Plumbing ---

template <typename Work, typename Done>
void CommunitySession::act(Work work, Done done, const QString& fallback) {
    setBusy(true);
    setMessages({}, {});
    using Result = std::invoke_result_t<Work&>;
    auto onFail = [this, fallback](const Failure& failure) {
        setBusy(false);
        fail(failure, fallback);
    };
    if constexpr (std::is_void_v<Result>) {
        runInBackground(
            pool_, this, std::move(work),
            [this, done = std::move(done)]() mutable {
                setBusy(false);
                done();
            },
            std::move(onFail));
    } else {
        runInBackground(
            pool_, this, std::move(work),
            [this, done = std::move(done)](Result result) mutable {
                setBusy(false);
                done(std::move(result));
            },
            std::move(onFail));
    }
}

void CommunitySession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void CommunitySession::setMessages(const QString& error, const QString& notice) {
    if (error_ == error && notice_ == notice) return;
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

void CommunitySession::clearMessages() {
    setMessages({}, {});
}

void CommunitySession::fail(const Failure& failure, const QString& fallback) {
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    // The server's own words for an API error, as the web app shows err.message.
    setMessages(failure.code.isEmpty() && !fallback.isEmpty() ? fallback : failure.message, {});
}

void CommunitySession::open(const QString& page, const QString& communityId, const QString& invite) {
    const std::string id = communityId.toStdString();
    const bool other = id != communityId_;
    page_ = page;
    if (!communityId.isEmpty()) {
        communityId_ = id;
        // Reopening the same community (the page coming back into view) keeps the link's invitation.
        if (other || !invite.isEmpty()) invite_ = invite.toStdString();
    }
    if (other && !communityId.isEmpty()) {
        community_.clear();
        communityError_.clear();
        members_.clear();
        invitations_.clear();
        reports_.clear();
        createdInvite_.clear();
        memberFilter_ = QStringLiteral("active");
        emit communityChanged();
        emit manageChanged();
    }
    setMessages({}, {});
    emit pageChanged();
    if (config_.isEmpty()) loadConfig();
    if (page == QStringLiteral("discover") && !discoverLoaded_) search({}, {});
    if (page == QStringLiteral("mine")) refreshMine();
    if (page == QStringLiteral("memberships")) refreshMemberships();
    if (page == QStringLiteral("create")) loadCreator();
    if (page == QStringLiteral("creator")) {
        loadCreator();
        loadDashboard();
    }
    if (page == QStringLiteral("community") || page == QStringLiteral("manage")) reloadCommunity();
    if (page == QStringLiteral("manage")) loadMembers(memberFilter_);
}

void CommunitySession::loadConfig() {
    auto api = api_;
    runInBackground(
        pool_, this, [api] { return toVariant(community_api::config(*api)).toMap(); },
        [this](QVariantMap config) {
            config_ = std::move(config);
            configError_.clear();
            emit configChanged();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            configError_ = QStringLiteral("Communities aren't reachable right now.");
            emit configChanged();
        });
}

QVariantList CommunitySession::categories() const {
    QStringList values;
    for (const auto& c : config_.value(QStringLiteral("categories")).toList()) values.append(c.toString());
    if (values.isEmpty()) values = categoryLabels().keys();
    QVariantList out;
    for (const auto& value : values) out.append(QVariantMap{{"value", value}, {"label", categoryLabel(value)}});
    return out;
}

// --- Discover, mine, memberships ---

void CommunitySession::search(const QString& query, const QString& category) {
    const int generation = ++discoverGeneration_;
    auto api = api_;
    const std::string q = query.trimmed().toStdString();
    const std::string c = category.toStdString();
    runInBackground(
        pool_, this, [api, q, c] { return toVariant(community_api::discover(*api, q, c, 0)).toList(); },
        [this, generation](QVariantList list) {
            if (generation != discoverGeneration_) return;  // a newer search is out
            discover_ = std::move(list);
            discoverLoaded_ = true;
            emit discoverChanged();
        },
        [this, generation](const Failure& failure) {
            if (generation != discoverGeneration_) return;
            discoverLoaded_ = true;
            emit discoverChanged();
            fail(failure);
        });
}

void CommunitySession::refreshMine() {
    auto api = api_;
    runInBackground(
        pool_, this, [api] { return toVariant(community_api::mine(*api)).toList(); },
        [this](QVariantList list) {
            mine_ = std::move(list);
            mineLoaded_ = true;
            emit mineChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

void CommunitySession::refreshMemberships() {
    auto api = api_;
    runInBackground(
        pool_, this, [api] { return toVariant(community_api::memberships(*api)).toList(); },
        [this](QVariantList list) {
            // Newest first, as the web app sorts them.
            std::sort(list.begin(), list.end(), [](const QVariant& a, const QVariant& b) {
                return a.toMap().value("createdAt").toString() > b.toMap().value("createdAt").toString();
            });
            memberships_ = std::move(list);
            membershipsLoaded_ = true;
            emit membershipsChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

void CommunitySession::cancelMembership(const QString& subscriptionId) {
    auto api = api_;
    const std::string id = subscriptionId.toStdString();
    act([api, id] { community_api::cancelMembership(*api, id); },
        [this] {
            setMessages({}, QStringLiteral("Cancelled. You won't be charged again, and you keep access until the end of the period you paid for."));
            refreshMemberships();
        });
}

void CommunitySession::resumeMembership(const QString& subscriptionId) {
    auto api = api_;
    const std::string id = subscriptionId.toStdString();
    act([api, id] { community_api::resumeMembership(*api, id); },
        [this] {
            setMessages({}, QStringLiteral("Your membership renews again."));
            refreshMemberships();
        });
}

void CommunitySession::openPortal(const QString& subscriptionId) {
    auto api = api_;
    const std::string id = subscriptionId.toStdString();
    act([api, id] { return community_api::membershipPortal(*api, id); }, [](std::string url) { openInBrowser(url); });
}

void CommunitySession::toggleReceipts(const QString& subscriptionId) {
    if (receipts_.contains(subscriptionId)) {
        receipts_.remove(subscriptionId);
        emit membershipsChanged();
        return;
    }
    auto api = api_;
    const std::string id = subscriptionId.toStdString();
    runInBackground(
        pool_, this, [api, id] { return toVariant(community_api::receipts(*api, id)).toList(); },
        [this, subscriptionId](QVariantList list) {
            receipts_.insert(subscriptionId, list);
            emit membershipsChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

void CommunitySession::checkConfirmation() {
    if (confirming_.isEmpty()) return;
    pollConfirmation();
}

void CommunitySession::pollConfirmation() {
    if (confirming_.isEmpty()) {
        confirmTimer_.stop();
        return;
    }
    ++confirmTries_;
    auto api = api_;
    const std::string id = confirming_.toStdString();
    runInBackground(
        pool_, this, [api, id] { return toVariant(community_api::refreshMembership(*api, id)).toMap(); },
        [this](QVariantMap view) {
            const QString status = view.value("entitlementStatus").toString();
            if (status == QStringLiteral("pending") && confirmTries_ < kConfirmTries) return;
            // Confirmed (or not, or it's taking too long): the lists say which from here.
            confirming_.clear();
            confirmTimer_.stop();
            if (status == QStringLiteral("active")) {
                setMessages({}, QStringLiteral("Payment confirmed. Your app gets the community's key from a member who's online, then the chat opens."));
            }
            refreshMemberships();
            if (!communityId_.empty()) reloadCommunity();
            if (chats_) chats_->refreshNow();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            if (confirmTries_ >= kConfirmTries) {
                confirming_.clear();
                confirmTimer_.stop();
                emit membershipsChanged();
            }
        });
}

// --- One community ---

void CommunitySession::reloadCommunity() {
    if (communityId_.empty()) return;
    const int generation = ++communityGeneration_;
    auto api = api_;
    const std::string id = communityId_;
    const std::string invite = invite_;
    runInBackground(
        pool_, this, [api, id, invite] { return toVariant(community_api::get(*api, id, invite)).toMap(); },
        [this, generation](QVariantMap page) {
            if (generation != communityGeneration_) return;
            community_ = std::move(page);
            communityError_.clear();
            emit communityChanged();
        },
        [this, generation](const Failure& failure) {
            if (generation != communityGeneration_) return;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            communityError_ = failure.status == 404 ? QStringLiteral("This community doesn't exist, or you need an invite link to see it.")
                                                    : failure.message;
            emit communityChanged();
        });
}

void CommunitySession::join() {
    auto api = api_;
    const std::string id = communityId_;
    const std::string invite = invite_;
    const bool rejoin = community_.value("accessType").toString() == QStringLiteral("paid");
    act([api, id, invite] { community_api::join(*api, id, invite); },
        [this, rejoin] {
            setMessages({}, rejoin ? QStringLiteral("Welcome back.") : QStringLiteral("Joined. Waiting for the community's key…"));
            reloadCommunity();
            if (chats_) chats_->refreshNow();
        });
}

void CommunitySession::leave() {
    auto api = api_;
    const std::string id = communityId_;
    act([api, id] { community_api::leave(*api, id); },
        [this] {
            setMessages({}, QStringLiteral("You left the community."));
            reloadCommunity();
            if (chats_) chats_->refreshNow();
        });
}

void CommunitySession::subscribe(bool termsAccepted, bool ageConfirmed) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string invite = invite_;
    act([api, id, invite, termsAccepted, ageConfirmed] { return community_api::checkout(*api, id, termsAccepted, ageConfirmed, invite); },
        [this](community_api::Checkout checkout) {
            // Stripe's hosted checkout, in the browser. Access starts only when Stripe confirms the payment to the server.
            openInBrowser(checkout.checkoutUrl);
            confirming_ = qs(checkout.subscriptionId);
            confirmTries_ = 0;
            confirmTimer_.start();
            emit membershipsChanged();
            setMessages({}, QStringLiteral("Stripe's checkout opened in your browser. Your membership starts once Stripe confirms the payment; this page updates on its own."));
            reloadCommunity();
        });
}

void CommunitySession::openChat() {
    const QString conversationId = community_.value("conversationId").toString();
    if (conversationId.isEmpty() || !chats_) return;
    chats_->openWhenListed(conversationId.toStdString());
    emit showChatRequested();
}

void CommunitySession::report(const QString& communityId, const QString& targetType, const QString& targetUserId, const QString& category,
                              const QString& details, const QString& messageId, const QString& messageText, bool share) {
    community_api::ReportInput input;
    input.targetType = targetType.toStdString();
    input.targetUserId = optionalText(targetUserId);
    input.category = category.toStdString();
    input.details = details.trimmed().toStdString();
    // The text leaves this computer only if the reporter chose to share it.
    if (targetType == QStringLiteral("content") && share && !messageId.isEmpty()) {
        input.evidenceMessageId = messageId.toStdString();
        input.evidencePlaintext = messageText.left(8000).toStdString();
    }
    auto api = api_;
    const std::string id = communityId.toStdString();
    setBusy(true);
    runInBackground(
        pool_, this, [api, id, input] { community_api::report(*api, id, input); },
        [this] {
            setBusy(false);
            setMessages({}, QStringLiteral("Report sent. Thank you."));
            emit reported({});
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            emit reported(failure.message);
        });
}

// --- Create ---

void CommunitySession::create(const QVariantMap& fields) {
    community_api::CreateInput input;
    input.kind = fields.value("kind").toString().toStdString();
    input.accessType = fields.value("accessType").toString().toStdString();
    input.name = fields.value("name").toString().trimmed().toStdString();
    input.description = fields.value("description").toString().toStdString();
    input.category = fields.value("category").toString().toStdString();
    input.visibility = fields.value("visibility").toString().toStdString();
    input.rules = fields.value("rules").toString().toStdString();
    input.previewText = fields.value("previewText").toString().toStdString();
    input.joinApproval = input.accessType == "free" && fields.value("joinApproval").toBool();
    input.showMemberCount = true;
    input.slowModeSeconds = fields.value("slowModeSeconds").toInt();
    if (input.accessType == "paid") {
        const int cents = parseDollars(fields.value("price").toString());
        if (cents < 0) {
            setMessages(QStringLiteral("Enter a monthly price like 10 or 9.99."), {});
            return;
        }
        input.priceMinor = cents;
    }
    if (const QString limit = fields.value("memberLimit").toString().trimmed(); !limit.isEmpty()) input.memberLimit = limit.toInt();

    auto api = api_;
    auto identity = identity_;
    act(
        [api, identity, input]() mutable {
            // The community's first key is made here, sealed to you; the server never sees it. The id is
            // chosen here too, so epoch 0's copy is bound to this community like every later one.
            input.id = util::randomUuid();
            input.wrappedKey = crypto::community::newCommunityKey(input.id, identity->publicKey).wrappedKey;
            community_api::create(*api, input);
            return input.id;
        },
        [this](std::string id) {
            if (chats_) chats_->refreshNow();
            open(QStringLiteral("manage"), qs(id));
            setMessages({}, QStringLiteral("Created as a draft. Publish it when it's ready."));
        });
}

// --- Creator ---

void CommunitySession::loadCreator() {
    auto api = api_;
    runInBackground(
        pool_, this,
        [api] {
            Json mine = community_api::myCreator(*api).at("creator");
            // Back from Stripe's onboarding (or any visit while it's unfinished): ask Stripe how it went.
            if (mine.is_object()) {
                const Json& payments = mine.at("payments");
                if (payments.value("configured", false) && payments.value("status", std::string()) != "not_started" &&
                    !payments.value("ready", false)) {
                    try {
                        mine = community_api::refreshOnboarding(*api).at("creator");
                    } catch (const api::ApiError& e) {
                        if (e.status() == 401) throw;
                    }
                }
            }
            return toVariant(mine);
        },
        [this](QVariant creator) {
            creator_ = std::move(creator);
            creatorLoaded_ = true;
            emit creatorChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

void CommunitySession::becomeCreator(const QString& handle, const QString& displayName, const QString& bio, const QString& link) {
    auto api = api_;
    const std::string h = handle.trimmed().toLower().toStdString();
    const std::string name = displayName.trimmed().toStdString();
    const std::string about = bio.toStdString();
    const auto profileLink = optionalText(link);
    act([api, h, name, about, profileLink] { return toVariant(community_api::becomeCreator(*api, h, name, about, profileLink).at("creator")); },
        [this](QVariant creator) {
            creator_ = std::move(creator);
            creatorLoaded_ = true;
            emit creatorChanged();
            loadDashboard();
        });
}

void CommunitySession::saveCreatorProfile(const QString& displayName, const QString& bio, const QString& link) {
    auto api = api_;
    Json changes{{"displayName", displayName.trimmed().toStdString()}, {"bio", bio.toStdString()}};
    const auto profileLink = optionalText(link);
    changes["profileLink"] = profileLink ? Json(*profileLink) : Json(nullptr);
    act([api, changes] { return toVariant(community_api::updateCreator(*api, changes).at("creator")); },
        [this](QVariant creator) {
            creator_ = std::move(creator);
            emit creatorChanged();
            setMessages({}, QStringLiteral("Saved."));
        });
}

void CommunitySession::setCreatorPicture(const QUrl& file) {
    auto api = api_;
    act([api, file] { return toVariant(community_api::updateCreator(*api, Json{{"avatar", imageBase64(file, 300 * 1024)}}).at("creator")); },
        [this](QVariant creator) {
            creator_ = std::move(creator);
            emit creatorChanged();
            ++imageRevision_;
            emit imagesChanged();
            setMessages({}, QStringLiteral("Picture updated."));
        });
}

void CommunitySession::setPaidMemberships(bool enabled) {
    auto api = api_;
    act([api, enabled] { return toVariant(community_api::updateCreator(*api, Json{{"paidMembershipsEnabled", enabled}}).at("creator")); },
        [this](QVariant creator) {
            creator_ = std::move(creator);
            emit creatorChanged();
        });
}

void CommunitySession::startOnboarding(const QString& country) {
    auto api = api_;
    const std::string c = country.toStdString();
    act([api, c] { return community_api::startOnboarding(*api, c); },
        [this](std::string url) {
            openInBrowser(url);
            setMessages({}, QStringLiteral("Stripe opened in your browser. Come back here and choose Check status when you're done."));
        });
}

void CommunitySession::refreshOnboarding() {
    auto api = api_;
    act([api] { return toVariant(community_api::refreshOnboarding(*api).at("creator")); },
        [this](QVariant creator) {
            creator_ = std::move(creator);
            emit creatorChanged();
            setMessages({}, QStringLiteral("Status updated."));
        });
}

void CommunitySession::openStripeDashboard() {
    auto api = api_;
    act([api] { return community_api::dashboardLink(*api); }, [](std::string url) { openInBrowser(url); });
}

void CommunitySession::loadDashboard() {
    auto api = api_;
    runInBackground(
        pool_, this, [api] { return toVariant(community_api::creatorDashboard(*api)).toMap(); },
        [this](QVariantMap dashboard) {
            dashboard_ = std::move(dashboard);
            dashboardLoaded_ = true;
            emit dashboardChanged();
        },
        [this](const Failure& failure) {
            // Not a creator yet (404): nothing to show, and nothing wrong.
            if (failure.status == 404) return;
            fail(failure);
        });
}

// --- Manage ---

void CommunitySession::publish() {
    auto api = api_;
    const std::string id = communityId_;
    act([api, id] { return toVariant(community_api::publish(*api, id)).toMap(); },
        [this](QVariantMap page) {
            community_ = std::move(page);
            emit communityChanged();
            setMessages({}, QStringLiteral("Published."));
        });
}

void CommunitySession::unpublish() {
    auto api = api_;
    const std::string id = communityId_;
    act([api, id] { return toVariant(community_api::unpublish(*api, id)).toMap(); },
        [this](QVariantMap page) {
            community_ = std::move(page);
            emit communityChanged();
            setMessages({}, QStringLiteral("Unpublished: hidden and closed to new members."));
        });
}

void CommunitySession::closeCommunity() {
    auto api = api_;
    const std::string id = communityId_;
    const bool paid = community_.value("accessType").toString() == QStringLiteral("paid");
    act([api, id] { community_api::close(*api, id, "end_of_period"); },
        [this, paid] {
            setMessages({}, paid ? QStringLiteral("Closing: no more renewals.") : QStringLiteral("Closed."));
            reloadCommunity();
            if (chats_) chats_->refreshNow();
        });
}

void CommunitySession::deliverKeysNow() {
    runUpkeep(true);
}

void CommunitySession::runUpkeep(bool reportResult) {
    if (upkeepRunning_) {
        if (reportResult) setMessages({}, QStringLiteral("Already checking keys…"));
        return;
    }
    upkeepRunning_ = true;
    if (reportResult) setBusy(true);
    auto keys = keys_;
    runInBackground(
        pool_, this, [keys] { return keys->runMaintenance(); },
        [this, reportResult](data::CommunityKeyStore::Upkeep done) {
            upkeepRunning_ = false;
            if (reportResult) {
                setBusy(false);
                setMessages({}, QStringLiteral("Keys are up to date."));
            }
            if (done.rotated + done.enrolled > 0) {
                if (chats_) chats_->refreshNow();
                if (page_ == QStringLiteral("community") || page_ == QStringLiteral("manage")) reloadCommunity();
                if (page_ == QStringLiteral("manage")) loadMembers(memberFilter_);
            }
        },
        [this, reportResult](const Failure& failure) {
            upkeepRunning_ = false;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            if (reportResult) {
                setBusy(false);
                fail(failure);
            }
        });
}

void CommunitySession::saveSettings(const QVariantMap& fields) {
    Json changes = Json::object();
    for (const char* key : {"name", "description", "category", "rules", "previewText", "visibility"}) {
        if (fields.contains(key)) changes[key] = fields.value(key).toString().toStdString();
    }
    if (changes.contains("name")) changes["name"] = QString::fromStdString(changes["name"].get<std::string>()).trimmed().toStdString();
    for (const char* key : {"joinApproval", "newMembersPaused", "invitationsSuspended"}) {
        if (fields.contains(key)) changes[key] = fields.value(key).toBool();
    }
    if (fields.contains("slowModeSeconds")) changes["slowModeSeconds"] = fields.value("slowModeSeconds").toInt();
    if (fields.contains("memberLimit")) {
        const QString limit = fields.value("memberLimit").toString().trimmed();
        changes["memberLimit"] = limit.isEmpty() ? Json(nullptr) : Json(limit.toInt());
    }
    const int version = community_.value("staff").toMap().value("version").toInt();
    if (version > 0) changes["expectedVersion"] = version;
    auto api = api_;
    const std::string id = communityId_;
    act([api, id, changes] { return toVariant(community_api::update(*api, id, changes)).toMap(); },
        [this](QVariantMap page) {
            community_ = std::move(page);
            emit communityChanged();
            setMessages({}, QStringLiteral("Saved."));
            if (chats_) chats_->refreshNow();  // the chat is named after the community
        });
}

void CommunitySession::setCommunityImage(const QString& kind, const QUrl& file) {
    if (kind != QStringLiteral("avatar") && kind != QStringLiteral("banner")) return;
    const qint64 limit = kind == QStringLiteral("avatar") ? 300 * 1024 : 800 * 1024;
    const int version = community_.value("staff").toMap().value("version").toInt();
    auto api = api_;
    const std::string id = communityId_;
    const std::string field = kind.toStdString();
    act(
        [api, id, field, file, limit, version] {
            Json changes{{field, imageBase64(file, limit)}};
            if (version > 0) changes["expectedVersion"] = version;
            return toVariant(community_api::update(*api, id, changes)).toMap();
        },
        [this](QVariantMap page) {
            community_ = std::move(page);
            emit communityChanged();
            ++imageRevision_;
            emit imagesChanged();
            setMessages({}, QStringLiteral("Picture updated."));
            if (chats_) chats_->refreshNow();  // the chat shows the community's picture
        });
}

void CommunitySession::setPrice(const QString& dollars) {
    const int cents = parseDollars(dollars);
    if (cents < 0) {
        setMessages(QStringLiteral("Enter a monthly price like 10 or 9.99."), {});
        return;
    }
    auto api = api_;
    const std::string id = communityId_;
    act([api, id, cents] { community_api::setPrice(*api, id, cents); },
        [this] {
            setMessages({}, QStringLiteral("New price saved. It applies to new members; current members keep the price they signed up at."));
            reloadCommunity();
        });
}

void CommunitySession::loadMembers(const QString& status) {
    memberFilter_ = status;
    members_.clear();
    emit manageChanged();
    auto api = api_;
    const std::string id = communityId_;
    const std::string filter = status.toStdString();
    runInBackground(
        pool_, this, [api, id, filter] { return toVariant(community_api::members(*api, id, filter)).toList(); },
        [this, status](QVariantList list) {
            if (status != memberFilter_) return;
            members_ = std::move(list);
            emit manageChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

void CommunitySession::setMemberRole(const QString& userId, const QString& role) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string user = userId.toStdString();
    const std::string r = role.toStdString();
    act([api, id, user, r] { community_api::setMember(*api, id, user, r, std::nullopt); }, [this] { loadMembers(memberFilter_); });
}

void CommunitySession::setMemberCanPost(const QString& userId, bool canPost) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string user = userId.toStdString();
    act([api, id, user, canPost] { community_api::setMember(*api, id, user, std::nullopt, canPost); }, [this] { loadMembers(memberFilter_); });
}

void CommunitySession::removeMember(const QString& userId, bool ban, const QString& reason) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string user = userId.toStdString();
    const auto why = optionalText(reason);
    act([api, id, user, ban, why] { community_api::removeMember(*api, id, user, ban, why); },
        [this, ban] {
            setMessages({}, ban ? QStringLiteral("Banned. The next post switches to a new key they never get.")
                                : QStringLiteral("Removed. The next post switches to a new key they never get."));
            loadMembers(memberFilter_);
            reloadCommunity();
            // Rotate now rather than at the next post: nothing new is readable with a key they kept.
            runUpkeep(false);
        });
}

void CommunitySession::unban(const QString& userId) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string user = userId.toStdString();
    act([api, id, user] { community_api::unban(*api, id, user); }, [this] { loadMembers(memberFilter_); });
}

void CommunitySession::decideRequest(const QString& userId, bool approve) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string user = userId.toStdString();
    act([api, id, user, approve] { community_api::decideJoinRequest(*api, id, user, approve); },
        [this, approve] {
            loadMembers(memberFilter_);
            if (approve) runUpkeep(false);  // they're waiting for the key now: deliver it
        });
}

void CommunitySession::loadInvitations() {
    auto api = api_;
    const std::string id = communityId_;
    runInBackground(
        pool_, this, [api, id] { return toVariant(community_api::invitations(*api, id)).toList(); },
        [this](QVariantList list) {
            invitations_ = std::move(list);
            emit manageChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

void CommunitySession::createInvitation(int maxUses, int expiresInDays) {
    auto api = api_;
    const std::string id = communityId_;
    const std::optional<int> uses = maxUses > 0 ? std::optional<int>(maxUses) : std::nullopt;
    const std::optional<int> days = expiresInDays > 0 ? std::optional<int>(expiresInDays) : std::nullopt;
    act([api, id, uses, days] { return community_api::createInvitation(*api, id, uses, days).at("token").get<std::string>(); },
        [this](std::string token) {
            // The web app's deep link: it opens this community's page with the invitation (here too, clicked in a chat).
            createdInvite_ = qs(community_api::inviteUrl(webAppBase_.toStdString(), communityId_, token));
            emit manageChanged();
            loadInvitations();
        });
}

void CommunitySession::revokeInvitation(const QString& invitationId) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string invitation = invitationId.toStdString();
    act([api, id, invitation] { community_api::revokeInvitation(*api, id, invitation); }, [this] { loadInvitations(); });
}

void CommunitySession::loadReports() {
    auto api = api_;
    const std::string id = communityId_;
    runInBackground(
        pool_, this, [api, id] { return toVariant(community_api::reports(*api, id)).toList(); },
        [this](QVariantList list) {
            reports_ = std::move(list);
            emit manageChanged();
        },
        [this](const Failure& failure) { fail(failure); });
}

void CommunitySession::resolveReport(const QString& reportId, const QString& status) {
    auto api = api_;
    const std::string id = communityId_;
    const std::string report = reportId.toStdString();
    const std::string s = status.toStdString();
    act([api, id, report, s] { community_api::resolveReport(*api, id, report, s, std::nullopt); }, [this] { loadReports(); });
}

// --- Pictures and links ---

QString CommunitySession::imagePath(const QString& kind, const QString& communityId) const {
    const std::string id = communityId.toStdString();
    // Only the community opened through the link has its invitation.
    const std::string invite = id == communityId_ ? invite_ : std::string();
    return qs(community_api::imagePath(id, kind.toStdString(), invite));
}

QString CommunitySession::creatorAvatarPath(const QString& creatorId) const {
    return qs(community_api::creatorAvatarPath(creatorId.toStdString()));
}

bool CommunitySession::isCommunityLink(const QString& text) const {
    return community_api::parseLink(text.toStdString()).has_value();
}

bool CommunitySession::isOwnCommunityLink(const QString& text) const {
    return text.trimmed().startsWith(webAppBase_, Qt::CaseInsensitive) && isCommunityLink(text);
}

void CommunitySession::openLink(const QString& text) {
    const auto link = community_api::parseLink(text.toStdString());
    if (!link) return;
    open(QStringLiteral("community"), qs(link->communityId), qs(link->invite));
    emit showRequested();
}

// --- Helpers ---

QString CommunitySession::formatMoney(double amountMinor, const QString& currency) const {
    const double amount = amountMinor / 100.0;
    const QString code = currency.toUpper();
    const QString symbol = code == QStringLiteral("USD") ? QStringLiteral("$") : code + u' ';
    return QLocale().toCurrencyString(amount, symbol, 2);
}

QString CommunitySession::formatRate(int bps) const {
    return QLocale().toString(bps / 100.0, 'g', 4) + u'%';
}

QString CommunitySession::formatDate(const QString& iso) const {
    const auto ms = util::parseIsoMillis(iso.toStdString());
    if (iso.isEmpty() || !ms) return QStringLiteral("—");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(*ms).toLocalTime().date(), QStringLiteral("d MMM yyyy"));
}

int CommunitySession::parseDollars(const QString& text) const {
    const auto cents = community_api::parseDollars(text.toStdString());
    return cents ? static_cast<int>(*cents) : -1;
}

int CommunitySession::commissionFor(int cents) const {
    const qint64 bps = config_.value("commissionBps").toLongLong();
    return static_cast<int>((static_cast<qint64>(cents) * bps + 5000) / 10000);
}

QString CommunitySession::categoryLabel(const QString& category) const {
    return categoryLabels().value(category, category);
}

void CommunitySession::copyText(const QString& text) {
    QGuiApplication::clipboard()->setText(text);
    setMessages({}, QStringLiteral("Copied."));
}

}  // namespace redvault::app
