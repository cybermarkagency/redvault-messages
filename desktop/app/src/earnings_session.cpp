#include "earnings_session.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QStandardPaths>

#include <algorithm>
#include <sstream>
#include <utility>

#include "chat_images.hpp"
#include "chat_session.hpp"
#include "redvault/net/http.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/crypto/post_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace eapi = api::earnings;
namespace em = model::earnings;
namespace pc = crypto::posts;

namespace earnings_detail {

struct Opened {
    eapi::Post dto;
    std::optional<em::PostBody> body;
    /// It has a body for this account but it didn't open (not sealed by its author for you, or malformed).
    bool failed = false;
};

}  // namespace earnings_detail

namespace {

using earnings_detail::Opened;
using OpenedPtr = std::shared_ptr<Opened>;

constexpr int kPollMs = 2000;
constexpr std::int64_t kGiveUpMs = 35ll * 60 * 1000;
constexpr int kKeyDeliveryMs = 60'000;
constexpr int kMaxImageSide = 4096;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QString money(std::int64_t minor) {
    return qs(em::formatUsd(minor));
}

/// The server's own words for an API error, or `fallback` for anything else.
QString worded(const Failure& failure, const QString& fallback) {
    return failure.code.isEmpty() ? fallback : failure.message;
}

QString dateOf(const std::optional<std::string>& iso) {
    if (!iso || iso->empty()) return {};
    const QDateTime when = QDateTime::fromString(qs(*iso), Qt::ISODateWithMs);
    return when.isValid() ? when.toLocalTime().toString(QStringLiteral("d MMM yyyy")) : QString();
}

QString countryName(const std::string& code) {
    static const std::map<std::string, QString> names = {
        {"us", "United States"}, {"gb", "United Kingdom"}, {"ca", "Canada"}, {"au", "Australia"},
        {"nz", "New Zealand"},   {"ie", "Ireland"},        {"de", "Germany"}, {"fr", "France"},
    };
    const auto it = names.find(code);
    return it != names.end() ? it->second : qs(code).toUpper();
}

QString mediaDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/post-media");
}

QString imageKey(const std::string& postId) {
    return QStringLiteral("post-") + qs(postId);
}

/// Who your chats say each person is: their key in your 1:1 chat with them.
std::map<std::string, std::string> chatKeysOf(ChatSession* chats) {
    std::map<std::string, std::string> out;
    if (!chats) return out;
    for (const auto& meta : chats->metas()) {
        if (meta && !meta->isGroup && !meta->otherUserId.empty()) out[meta->otherUserId] = meta->otherPublicKeyB64;
    }
    return out;
}

/**
 * Opens one post when this account can read it (its own, or one bought whose
 * key has arrived). The author's key: from your chat with them when you have
 * one (the key you've seen), else the server's.
 */
OpenedPtr openOne(api::ApiClient& api, eapi::Post dto, const data::SelfIdentity& self, const std::map<std::string, std::string>& chatKeys,
                  std::map<std::string, crypto::Bytes>& authorKeys) {
    auto out = std::make_shared<Opened>();
    out->dto = std::move(dto);
    if (!out->dto.body) return out;
    try {
        crypto::Bytes author;
        if (out->dto.mine) {
            author = self.publicKey;
        } else if (const auto chat = chatKeys.find(out->dto.authorId); chat != chatKeys.end() && !chat->second.empty()) {
            author = crypto::base64Decode(chat->second);
        } else {
            auto known = authorKeys.find(out->dto.authorId);
            if (known == authorKeys.end()) {
                known = authorKeys.emplace(out->dto.authorId, crypto::base64Decode(api.getUser(out->dto.authorId).publicKey)).first;
            }
            author = known->second;
        }
        const auto& body = *out->dto.body;
        const crypto::SecretBytes key = pc::unwrapPostKey(out->dto.id, crypto::base64Decode(body.wrappedKey), author, self.privateKey.view());
        out->body = em::parseBody(pc::openBody(out->dto.id, key, crypto::base64Decode(body.ciphertext), crypto::base64Decode(body.nonce)));
    } catch (const api::ApiError& e) {
        if (e.status() == 401) throw;
        out->failed = true;
    } catch (const std::exception&) {
        out->failed = true;
    }
    return out;
}

struct ConfigResult {
    eapi::Config config;
    eapi::Settings settings;
};

struct OfferResult {
    eapi::Offer offer;
    std::vector<OpenedPtr> posts;
};

struct CreatorResult {
    eapi::CreatorSetup setup;
    std::optional<eapi::Creator> creator;
};

QVariantMap purchaseMap(const eapi::Purchase& p) {
    const bool refundable = p.status == "paid" || p.status == "consumed";
    return {{"id", qs(p.id)},
            {"product", qs(p.product)},
            {"productName", qs(em::productName(p.product))},
            {"amount", money(p.amountMinor)},
            {"fee", money(p.platformFeeMinor)},
            {"status", qs(p.status)},
            {"statusLabel", qs(em::statusName(p.status))},
            {"from", p.buyerUsername ? qs(*p.buyerUsername) : (p.product == "tip" ? QStringLiteral("A supporter") : QStringLiteral("-"))},
            {"when", dateOf(p.paidAt ? p.paidAt : std::optional<std::string>(p.createdAt))},
            {"refundable", refundable}};
}

}  // namespace

EarningsSession::EarningsSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                                 QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)), chats_(chats) {
    pool_.setMaxThreadCount(3);
    // Videos left from a session that didn't close cleanly.
    QDir(mediaDir()).removeRecursively();
    pollTimer_.setSingleShot(true);
    connect(&pollTimer_, &QTimer::timeout, this, &EarningsSession::poll);
    keyTimer_.setInterval(kKeyDeliveryMs);
    connect(&keyTimer_, &QTimer::timeout, this, &EarningsSession::deliverKeys);
    keyTimer_.start();
    if (chats_) {
        connect(chats_, &ChatSession::currentChatChanged, this, &EarningsSession::onChatChanged);
        // After a send to someone who charges: the paid message (or safety contact) is used up.
        connect(chats_, &ChatSession::sent, this, [this](bool ok) {
            if (!ok || !quote_) return;
            safety_ = false;
            refreshQuote();
        });
        connect(chats_, &ChatSession::paymentRequired, this, [this](const QString& chatId) {
            if (chatId.toStdString() == chatId_) refreshQuote();
        });
    }
    refresh();
}

EarningsSession::~EarningsSession() {
    pollTimer_.stop();
    keyTimer_.stop();
    pool_.clear();
    pool_.waitForDone();
    dropTempFiles();
}

bool EarningsSession::canBuy() const {
    return config_.enabled &&
           std::find(config_.purchasePlatforms.begin(), config_.purchasePlatforms.end(), "desktop") != config_.purchasePlatforms.end();
}

QVariantList EarningsSession::tipPresets() const {
    QVariantList out;
    const auto& presets = config_.tipPresetsMinor;
    if (presets.empty()) {
        for (const auto minor : em::kTipPresetsMinor) out.append(QVariantMap{{"minor", static_cast<int>(minor)}, {"label", money(minor)}});
    } else {
        for (const auto minor : presets) out.append(QVariantMap{{"minor", static_cast<int>(minor)}, {"label", money(minor)}});
    }
    return out;
}

QVariantMap EarningsSession::rates() const {
    return {{"paidMessage", config_.paidMessage.ratePercent},
            {"tip", config_.tip.ratePercent},
            {"post", config_.exclusivePost.ratePercent},
            {"tipMin", money(config_.tip.minMinor > 0 ? config_.tip.minMinor : 100)},
            {"tipMax", money(config_.tip.maxMinor > 0 ? config_.tip.maxMinor : 50'000)},
            {"messageMin", money(config_.paidMessage.minMinor > 0 ? config_.paidMessage.minMinor : 100)},
            {"messageMax", money(config_.paidMessage.maxMinor > 0 ? config_.paidMessage.maxMinor : 10'000)},
            {"postMin", money(config_.exclusivePost.minMinor > 0 ? config_.exclusivePost.minMinor : 100)},
            {"postMax", money(config_.exclusivePost.maxMinor > 0 ? config_.exclusivePost.maxMinor : 20'000)}};
}

QVariantMap EarningsSession::quote() const {
    if (!quote_) return {{"required", false}};
    const auto& q = *quote_;
    return {{"required", true},
            {"mode", qs(q.mode)},
            {"price", q.priceMinor ? money(*q.priceMinor) : QString()},
            {"purchasable", q.purchasable && canBuy()},
            {"paid", q.availablePurchaseId.has_value()},
            {"safetyAvailable", q.safetyContactAvailable}};
}

void EarningsSession::setSafety(bool on) {
    if (safety_ == on) return;
    safety_ = on && quote_ && quote_->safetyContactAvailable;
    emit quoteChanged();
}

QVariantMap EarningsSession::settings() const {
    const auto& s = settings_;
    return {{"loaded", settingsLoaded_},
            {"available", s.available},
            {"creatorReady", s.creatorReady},
            {"mode", qs(s.paidMessagesMode)},
            {"priceMinor", s.paidMessagePriceMinor ? static_cast<int>(*s.paidMessagePriceMinor) : 0},
            {"priceText", s.paidMessagePriceMinor ? QString::number(static_cast<double>(*s.paidMessagePriceMinor) / 100.0, 'f', 2) : QStringLiteral("3.00")},
            {"tipsEnabled", s.tipsEnabled},
            {"acceptsMessageRequests", s.acceptsMessageRequests},
            {"exemptCount", static_cast<int>(s.exemptions.size())}};
}

QVariantList EarningsSession::myPosts() const {
    QVariantList out;
    for (const auto& p : myPosts_) out.append(postMap(*p));
    return out;
}

QVariantList EarningsSession::library() const {
    QVariantList out;
    for (const auto& p : library_) out.append(postMap(*p));
    return out;
}

QVariantMap EarningsSession::creator() const {
    QVariantList countries;
    for (const auto& c : creatorSetup_.creatorCountries) countries.append(QVariantMap{{"code", qs(c)}, {"name", countryName(c)}});
    QVariantMap out{{"loaded", creatorLoaded_},
                    {"exists", creator_.has_value()},
                    {"paymentsEnabled", creatorSetup_.paymentsEnabled},
                    {"countries", countries},
                    {"termsVersion", qs(creatorSetup_.creatorTermsVersion)}};
    if (creator_) {
        out["handle"] = qs(creator_->handle);
        out["displayName"] = qs(creator_->displayName);
        out["status"] = qs(creator_->paymentsStatus);
        out["requirementsDue"] = creator_->requirementsDue;
        out["ready"] = creator_->paymentsReady;
        out["suspended"] = creator_->status != "active";
    }
    return out;
}

void EarningsSession::refresh() {
    auto api = api_;
    runInBackground(
        pool_, this,
        [api]() {
            ConfigResult r;
            r.config = eapi::config(*api);
            r.settings = eapi::settings(*api);
            return r;
        },
        [this](ConfigResult r) {
            const bool wasEnabled = config_.enabled;
            config_ = std::move(r.config);
            settings_ = std::move(r.settings);
            settingsLoaded_ = true;
            emit configChanged();
            emit settingsChanged();
            emit quoteChanged();
            if (config_.enabled && !wasEnabled) {
                refreshQuote();
                // A creator's first key delivery soon after signing in.
                QTimer::singleShot(5000, this, &EarningsSession::deliverKeys);
            }
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
        });
}

// --- Paid messages in the open chat ---

void EarningsSession::onChatChanged() {
    if (!chats_) return;
    const std::string chat = chats_->currentChatId().toStdString();
    const std::string user = chats_->currentIsGroup() ? std::string() : chats_->currentUserId().toStdString();
    if (chat == chatId_ && user == chatUserId_) return;
    chatId_ = chat;
    chatUserId_ = user;
    quote_.reset();
    safety_ = false;
    ++quoteGeneration_;
    emit quoteChanged();
    refreshQuote();
}

void EarningsSession::refreshQuote() {
    if (!config_.enabled || chatUserId_.empty()) {
        if (quote_) {
            quote_.reset();
            emit quoteChanged();
        }
        return;
    }
    const int generation = ++quoteGeneration_;
    auto api = api_;
    const std::string user = chatUserId_;
    const std::optional<std::string> chat = chatId_.empty() ? std::nullopt : std::optional<std::string>(chatId_);
    runInBackground(
        pool_, this, [api, user, chat]() { return eapi::quote(*api, user, chat); },
        [this, generation](eapi::Quote q) {
            if (generation != quoteGeneration_) return;
            if (q.required) {
                quote_ = std::move(q);
            } else {
                quote_.reset();
            }
            if (!quote_ || !quote_->safetyContactAvailable) safety_ = false;
            emit quoteChanged();
        },
        [this, generation](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            if (generation != quoteGeneration_) return;
            // Unknown: sending is tried as free; the server says if it isn't (payment_required) and the bar appears.
            quote_.reset();
            emit quoteChanged();
        });
}

QVariantMap EarningsSession::sendOptions() const {
    if (!quote_) return {{"ok", true}};
    if (safety_) return {{"ok", true}, {"safetyContact", true}};
    if (quote_->availablePurchaseId) return {{"ok", true}, {"entitlementId", qs(*quote_->availablePurchaseId)}};
    return {{"ok", false}};
}

// --- Paying ---

void EarningsSession::startPayment(eapi::StartPurchase input, const QString& context, const std::string& target) {
    if (paying()) return;
    payTarget_ = qs(input.postId ? *input.postId : input.creatorUserId.value_or(std::string()));
    if (!canBuy()) {
        payError_ = QStringLiteral("Buying isn't available in this app right now.");
        payErrorContext_ = context;
        emit payChanged();
        return;
    }
    const std::string attempt = context.toStdString() + ':' + target;
    input.idempotencyKey = retryTarget_ == attempt && !retryKey_.empty() ? retryKey_ : eapi::newIdempotencyKey();
    retryTarget_ = attempt;
    retryKey_ = input.idempotencyKey;
    payContext_ = context;
    payError_.clear();
    payErrorContext_.clear();
    paidProduct_.clear();
    payStatus_ = QStringLiteral("Opening the payment page...");
    emit payChanged();
    auto api = api_;
    runInBackground(
        pool_, this, [api, input]() { return eapi::startPurchase(*api, input); },
        [this, context](eapi::Started started) {
            // An answer came: the next attempt is a new one.
            retryTarget_.clear();
            retryKey_.clear();
            pendingPurchaseId_ = started.purchaseId;
            pendingStartedMs_ = util::nowMillis();
            if (!started.checkoutUrl) {
                // An unused paid message was already there: nothing charged twice.
                poll();
                return;
            }
            const QUrl url(qs(*started.checkoutUrl));
            if (!net::isHttpsUrl(url.toString(QUrl::FullyEncoded).toStdString()) || !QDesktopServices::openUrl(url)) {
                failPayment(QStringLiteral("Couldn't open the payment page in your browser."));
                return;
            }
            payStatus_ = QStringLiteral("Finish paying on Stripe's page in your browser. Waiting for the payment provider to confirm...");
            emit payChanged();
            pollTimer_.start(kPollMs);
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            if (!failure.code.isEmpty()) {
                retryTarget_.clear();
                retryKey_.clear();
            }
            failPayment(worded(failure, QStringLiteral("Couldn't start the payment. Check your connection and try again.")));
        });
}

void EarningsSession::poll() {
    if (pendingPurchaseId_.empty()) return;
    auto api = api_;
    const std::string id = pendingPurchaseId_;
    runInBackground(
        pool_, this, [api, id]() { return eapi::purchase(*api, id); },
        [this, id](eapi::Purchase p) {
            if (id != pendingPurchaseId_) return;
            // Only the server's word counts, and it only says "paid" after the provider's verified webhook.
            if (p.status == "paid" || p.status == "consumed") {
                finishPayment(qs(p.product));
                return;
            }
            if (p.status != "checkout_pending" && p.status != "consuming") {
                failPayment(p.status == "refunded" || p.status == "refund_pending" ? QStringLiteral("This payment was refunded.")
                                                                                  : QStringLiteral("The payment didn't go through."));
                return;
            }
            if (util::nowMillis() - pendingStartedMs_ > kGiveUpMs) {
                failPayment(QStringLiteral("We haven't heard from the payment provider yet. If you paid, it will show up shortly."));
                return;
            }
            pollTimer_.start(kPollMs);
        },
        [this, id](const Failure& failure) {
            if (id != pendingPurchaseId_) return;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            if (util::nowMillis() - pendingStartedMs_ > kGiveUpMs) {
                failPayment(QStringLiteral("We haven't heard from the payment provider yet. If you paid, it will show up shortly."));
                return;
            }
            // Offline for a moment: keep asking.
            pollTimer_.start(kPollMs * 2);
        });
}

void EarningsSession::finishPayment(const QString& product) {
    pollTimer_.stop();
    pendingPurchaseId_.clear();
    payContext_.clear();
    payStatus_.clear();
    payError_.clear();
    payErrorContext_.clear();
    paidProduct_ = product;
    auto then = std::move(onPaid_);
    onPaid_ = nullptr;
    emit payChanged();
    if (then) then();
}

void EarningsSession::failPayment(const QString& message) {
    pollTimer_.stop();
    pendingPurchaseId_.clear();
    payErrorContext_ = payContext_;
    payContext_.clear();
    payStatus_.clear();
    payError_ = message;
    onPaid_ = nullptr;
    emit payChanged();
}

void EarningsSession::cancelPayment() {
    failPayment({});
}

void EarningsSession::clearPayment() {
    if (paidProduct_.isEmpty() && payError_.isEmpty()) return;
    paidProduct_.clear();
    payError_.clear();
    payErrorContext_.clear();
    emit payChanged();
}

void EarningsSession::payForMessage() {
    if (chatUserId_.empty() || !quote_) return;
    onPaid_ = [this]() { refreshQuote(); };
    eapi::StartPurchase input;
    input.product = "paid_message";
    input.creatorUserId = chatUserId_;
    startPayment(std::move(input), QStringLiteral("paid_message"), chatUserId_);
}

void EarningsSession::tip(const QString& creatorUserId, int amountMinor, bool showName) {
    const std::int64_t min = config_.tip.minMinor > 0 ? config_.tip.minMinor : 100;
    const std::int64_t max = config_.tip.maxMinor > 0 ? config_.tip.maxMinor : 50'000;
    if (amountMinor < min || amountMinor > max) {
        payError_ = QStringLiteral("Tips are between %1 and %2.").arg(money(min), money(max));
        payErrorContext_ = QStringLiteral("tip");
        payTarget_ = creatorUserId;
        emit payChanged();
        return;
    }
    onPaid_ = nullptr;
    const std::string user = creatorUserId.toStdString();
    eapi::StartPurchase input;
    input.product = "tip";
    input.creatorUserId = user;
    input.amountMinor = amountMinor;
    input.showName = showName;
    startPayment(std::move(input), QStringLiteral("tip"), user + ':' + std::to_string(amountMinor) + (showName ? ":named" : ":unnamed"));
}

void EarningsSession::buyPost(const QString& postId) {
    const std::string id = postId.toStdString();
    onPaid_ = [this]() { loadLibrary(); };
    eapi::StartPurchase input;
    input.product = "exclusive_post";
    input.postId = id;
    startPayment(std::move(input), QStringLiteral("exclusive_post"), id);
}

int EarningsSession::parseAmount(const QString& text) const {
    const auto cents = em::parseUsd(text.toStdString());
    return cents && *cents > 0 && *cents <= 100'000'000 ? static_cast<int>(*cents) : -1;
}

QString EarningsSession::formatAmount(int minor) const {
    return money(minor);
}

// --- Someone's profile ---

void EarningsSession::loadOffer(const QString& userIdText) {
    if (!config_.enabled) {
        emit offerLoaded(userIdText, {{"available", false}});
        return;
    }
    auto api = api_;
    auto identity = identity_;
    const auto chatKeys = chatKeysOf(chats_);
    const std::string userId = userIdText.toStdString();
    runInBackground(
        pool_, this,
        [api, identity, chatKeys, userId]() {
            OfferResult r;
            r.offer = eapi::offer(*api, userId);
            if (r.offer.available && r.offer.posts > 0) {
                std::map<std::string, crypto::Bytes> authorKeys;
                for (auto& p : eapi::creatorPosts(*api, userId)) r.posts.push_back(openOne(*api, std::move(p), *identity, chatKeys, authorKeys));
            }
            return r;
        },
        [this, userIdText](OfferResult r) {
            QVariantList posts;
            for (const auto& p : r.posts) {
                known_[p->dto.id] = p;
                posts.append(postMap(*p));
            }
            QString paidMessages;
            if (r.offer.paidMessagesMode == "first_contact") {
                paidMessages = QStringLiteral("Starting a chat costs %1 unless you're friends.").arg(money(r.offer.paidMessagePriceMinor));
            } else if (r.offer.paidMessagesMode == "per_message") {
                paidMessages = QStringLiteral("Each message costs %1 unless you're friends.").arg(money(r.offer.paidMessagePriceMinor));
            }
            emit offerLoaded(userIdText, {{"available", r.offer.available},
                                          {"handle", qs(r.offer.creatorHandle)},
                                          {"displayName", qs(r.offer.creatorDisplayName)},
                                          {"paidMessages", paidMessages},
                                          {"tips", r.offer.tips && canBuy()},
                                          {"posts", posts}});
        },
        [this, userIdText](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            emit offerLoaded(userIdText, {{"available", false}});
        });
}

bool EarningsSession::isExempt(const QString& userId) const {
    const std::string id = userId.toStdString();
    return std::find(settings_.exemptions.begin(), settings_.exemptions.end(), id) != settings_.exemptions.end();
}

void EarningsSession::setExempt(const QString& userIdText, bool exempt) {
    auto api = api_;
    const std::string userId = userIdText.toStdString();
    runInBackground(
        pool_, this, [api, userId, exempt]() { eapi::setExemption(*api, userId, exempt); },
        [this, userId, exempt]() {
            auto& list = settings_.exemptions;
            list.erase(std::remove(list.begin(), list.end(), userId), list.end());
            if (exempt) list.push_back(userId);
            emit settingsChanged();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't change that. Try again.")); });
}

void EarningsSession::loadPostMedia(const QString& postIdText) {
    const std::string postId = postIdText.toStdString();
    const auto it = known_.find(postId);
    if (it == known_.end() || !it->second->body || !it->second->body->media) {
        emit postMediaFailed(postIdText);
        return;
    }
    const auto media = *it->second->body->media;
    const bool video = QString::fromStdString(media.mime).startsWith(QStringLiteral("video/"));
    if (!video && ChatImageCache::instance().has(imageKey(postId))) {
        emit postMediaReady(postIdText, QStringLiteral("image://chatimage/full/") + imageKey(postId), false);
        return;
    }
    auto api = api_;
    runInBackground(
        pool_, this,
        [api, postId, media, video]() -> QString {
            const auto key = crypto::base64Decode(media.key);
            const auto header = crypto::base64Decode(media.header);
            std::istringstream in(eapi::downloadPostMedia(*api, postId));
            std::ostringstream out;
            crypto::files::decrypt(in, out, key, header, media.encryptedSize, media.sha256);
            const std::string plain = out.str();
            if (!video) {
                const QByteArray bytes(plain.data(), static_cast<qsizetype>(plain.size()));
                const auto image = ChatImageCache::decode(bytes);
                if (!image) throw std::runtime_error("Not an image");
                ChatImageCache::instance().put(imageKey(postId), ChatImageCache::thumbnail(*image), bytes);
                return QStringLiteral("image://chatimage/full/") + imageKey(postId);
            }
            // The player needs a file: a private one, deleted when you sign out or quit.
            const QString dir = mediaDir();
            QDir().mkpath(dir);
            const QString ext = media.mime == "video/webm" ? "webm" : media.mime == "video/quicktime" ? "mov" : "mp4";
            const QString path = dir + u'/' + QString::fromStdString(util::randomUuid()) + u'.' + ext;
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(plain.data(), static_cast<qint64>(plain.size())) != static_cast<qint64>(plain.size())) {
                QFile::remove(path);
                throw std::runtime_error("Couldn't write the video");
            }
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            return path;
        },
        [this, postIdText, video](QString location) {
            if (!video) {
                emit postMediaReady(postIdText, location, false);
                return;
            }
            tempFiles_.append(location);
            emit postMediaReady(postIdText, QUrl::fromLocalFile(location).toString(), true);
        },
        [this, postIdText](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            emit postMediaFailed(postIdText);
        });
}

// --- Your dashboard ---

void EarningsSession::loadDashboard() {
    auto api = api_;
    runInBackground(
        pool_, this, [api]() { return eapi::dashboard(*api); },
        [this](eapi::Dashboard d) {
            QVariantList products;
            for (const auto& p : d.products) {
                products.append(QVariantMap{{"product", qs(p.product)},
                                            {"name", qs(em::productName(p.product))},
                                            {"percent", p.commissionPercent},
                                            {"count", p.count},
                                            {"gross", money(p.grossMinor)},
                                            {"fees", money(p.platformFeeMinor)},
                                            {"net", money(p.creatorNetMinor)},
                                            {"refunded", money(p.refundedMinor)},
                                            {"refunds", p.refunds},
                                            {"disputes", p.disputes}});
            }
            QVariantList transactions;
            for (const auto& t : d.transactions) transactions.append(purchaseMap(t));
            const auto sum = [](const std::vector<eapi::Money>& list) {
                QStringList parts;
                for (const auto& m : list) parts << money(m.amountMinor) + (m.currency == "usd" ? QString() : u' ' + qs(m.currency).toUpper());
                return parts.isEmpty() ? money(0) : parts.join(QStringLiteral(", "));
            };
            QVariantList payouts;
            for (const auto& p : d.payouts) {
                payouts.append(QVariantMap{{"amount", money(p.amountMinor)}, {"status", qs(p.status)}, {"arrival", dateOf(p.arrivalDate)}});
            }
            dashboard_ = {{"loaded", true},
                          {"available", d.available},
                          {"creatorReady", d.creatorReady},
                          {"products", products},
                          {"gross", money(d.grossMinor)},
                          {"fees", money(d.platformFeeMinor)},
                          {"net", money(d.creatorNetMinor)},
                          {"transactions", transactions},
                          {"awaitingKeys", d.awaitingKeys},
                          {"hasBalance", d.hasBalance},
                          {"balanceAvailable", sum(d.balanceAvailable)},
                          {"balancePending", sum(d.balancePending)},
                          {"payouts", payouts}};
            settings_ = std::move(d.settings);
            settingsLoaded_ = true;
            emit dashboardChanged();
            emit settingsChanged();
        },
        [this](const Failure& failure) {
            // No creator profile yet (a buyer looking at their Library): nothing to show, nothing wrong.
            if (failure.status == 404) {
                dashboard_ = {{"loaded", true}, {"noCreator", true}};
                emit dashboardChanged();
                return;
            }
            fail(failure, QStringLiteral("Couldn't load your earnings."));
        });
}

void EarningsSession::loadTips() {
    auto api = api_;
    runInBackground(
        pool_, this, [api]() { return eapi::tipsReceived(*api); },
        [this](std::vector<eapi::Purchase> tips) {
            tips_.clear();
            for (const auto& t : tips) tips_.append(purchaseMap(t));
            emit dashboardChanged();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't load your tips.")); });
}

void EarningsSession::loadPosts(std::function<std::vector<eapi::Post>(api::ApiClient&)> fetch, std::function<void(std::vector<OpenedPtr>)> done) {
    auto api = api_;
    auto identity = identity_;
    const auto chatKeys = chatKeysOf(chats_);
    runInBackground(
        pool_, this,
        [api, identity, chatKeys, fetch]() {
            std::map<std::string, crypto::Bytes> authorKeys;
            std::vector<OpenedPtr> out;
            for (auto& p : fetch(*api)) out.push_back(openOne(*api, std::move(p), *identity, chatKeys, authorKeys));
            return out;
        },
        [this, done](std::vector<OpenedPtr> posts) {
            for (const auto& p : posts) known_[p->dto.id] = p;
            done(std::move(posts));
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't load the posts.")); });
}

void EarningsSession::loadMyPosts() {
    const std::string self = identity_->userId;
    loadPosts([self](api::ApiClient& api) { return eapi::creatorPosts(api, self); },
              [this](std::vector<OpenedPtr> posts) {
                  myPosts_ = std::move(posts);
                  emit postsChanged();
              });
}

void EarningsSession::loadLibrary() {
    loadPosts([](api::ApiClient& api) { return eapi::library(api); },
              [this](std::vector<OpenedPtr> posts) {
                  library_ = std::move(posts);
                  libraryLoaded_ = true;
                  emit postsChanged();
              });
}

QVariantMap EarningsSession::postMap(const Opened& post) const {
    const auto& p = post.dto;
    const bool hasMedia = post.body && post.body->media;
    const QString status = p.status == "published" ? QStringLiteral("On sale") : p.status == "unpublished" ? QStringLiteral("Off sale") : qs(p.status);
    return {{"id", qs(p.id)},
            {"authorId", qs(p.authorId)},
            {"price", money(p.priceMinor)},
            {"teaser", qs(p.teaser)},
            {"status", qs(p.status)},
            {"statusLabel", status},
            {"mine", p.mine},
            {"owned", p.owned},
            {"awaitingKey", p.awaitingKey},
            {"locked", p.locked},
            {"opened", post.body.has_value()},
            {"failed", post.failed},
            {"text", post.body ? qs(post.body->text) : QString()},
            {"hasMedia", hasMedia},
            {"video", hasMedia && post.body->media->mime.rfind("video/", 0) == 0},
            {"creatorHandle", qs(p.creatorHandle)},
            {"creatorName", qs(p.creatorDisplayName)},
            {"when", dateOf(p.purchasedAt ? p.purchasedAt : std::optional<std::string>(p.createdAt))}};
}

void EarningsSession::saveMessageSettings(const QString& mode, const QString& priceText) {
    eapi::SettingsPatch patch;
    patch.paidMessagesMode = mode.toStdString();
    if (mode == "off") {
        patch.clearPrice = true;
    } else {
        const auto cents = em::parseUsd(priceText.toStdString());
        if (!cents || *cents <= 0) {
            setMessages(QStringLiteral("Enter a price, like 3.00."), {});
            return;
        }
        patch.paidMessagePriceMinor = *cents;
    }
    auto api = api_;
    setBusy(true);
    runInBackground(
        pool_, this, [api, patch]() { return eapi::updateSettings(*api, patch); },
        [this](eapi::Settings s) {
            setBusy(false);
            settings_ = std::move(s);
            emit settingsChanged();
            setMessages({}, QStringLiteral("Saved."));
            loadDashboard();
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure, QStringLiteral("Couldn't save that. Try again."));
        });
}

void EarningsSession::setTipsEnabled(bool on) {
    auto api = api_;
    setBusy(true);
    runInBackground(
        pool_, this,
        [api, on]() {
            eapi::SettingsPatch patch;
            patch.tipsEnabled = on;
            return eapi::updateSettings(*api, patch);
        },
        [this](eapi::Settings s) {
            setBusy(false);
            settings_ = std::move(s);
            emit settingsChanged();
            loadDashboard();
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure, QStringLiteral("Couldn't save that. Try again."));
        });
}

QVariantMap EarningsSession::checkPostFile(const QUrl& file) const {
    const QFileInfo info(file.toLocalFile());
    const QString ext = info.suffix().toLower();
    QString kind;
    if (QStringList{"jpg", "jpeg", "png", "webp", "gif", "bmp"}.contains(ext)) kind = QStringLiteral("image");
    if (QStringList{"mp4", "webm", "mov"}.contains(ext)) kind = QStringLiteral("video");
    QString error;
    if (kind.isEmpty()) {
        error = QStringLiteral("Use a photo (JPEG, PNG, WebP) or a video (MP4, WebM, MOV).");
    } else if (!config_.postMedia) {
        error = QStringLiteral("Photos and videos in posts aren't available on this server yet.");
    } else if (static_cast<std::uint64_t>(info.size()) + 1024 * 1024 > eapi::kMaxPostMediaBytes) {
        error = QStringLiteral("That file is too large for a post (at most 199 MB).");
    }
    return {{"kind", kind}, {"name", info.fileName()}, {"error", error}};
}

void EarningsSession::publishPost(const QString& text, const QString& teaser, const QString& priceText, const QUrl& file) {
    if (!publishStage_.isEmpty()) return;
    if (!settings_.creatorReady) {
        setMessages(QStringLiteral("Finish payout setup first (Payouts tab)."), {});
        return;
    }
    const auto cents = em::parseUsd(priceText.toStdString());
    if (!cents || *cents <= 0) {
        setMessages(QStringLiteral("Enter a price, like 5.00."), {});
        return;
    }
    const QString body = text.trimmed();
    const QString cleanTeaser = teaser.trimmed();
    const bool withFile = !file.isEmpty();
    if (body.isEmpty() && !withFile) {
        setMessages(QStringLiteral("Write something or add a photo or video."), {});
        return;
    }
    if (body.size() > static_cast<qsizetype>(em::kMaxPostText) || cleanTeaser.size() > static_cast<qsizetype>(em::kMaxTeaser)) {
        setMessages(QStringLiteral("That's too long (posts up to 10,000 characters, teasers up to 280)."), {});
        return;
    }
    QString path;
    bool video = false;
    if (withFile) {
        const QVariantMap check = checkPostFile(file);
        if (!check.value("error").toString().isEmpty()) {
            setMessages(check.value("error").toString(), {});
            return;
        }
        path = file.toLocalFile();
        video = check.value("kind").toString() == "video";
    }
    clearMessages();
    publishStage_ = QStringLiteral("publishing");
    emit postsChanged();
    auto api = api_;
    auto identity = identity_;
    const std::int64_t price = *cents;
    const std::string bodyText = body.toStdString();
    const std::string teaserText = cleanTeaser.toStdString();
    runInBackground(
        pool_, this,
        [api, identity, price, bodyText, teaserText, path, video]() {
            const std::string id = util::randomUuid();
            const crypto::SecretBytes key = pc::newPostKey();
            em::PostBody postBody{bodyText, std::nullopt};
            std::string ciphertext;
            if (!path.isEmpty()) {
                std::string plain;
                std::string mime;
                if (video) {
                    QFile in(path);
                    if (!in.open(QIODevice::ReadOnly)) throw api::UserFacingError("That video couldn't be opened.");
                    const QByteArray all = in.readAll();
                    plain.assign(all.constData(), static_cast<std::size_t>(all.size()));
                    const QString ext = QFileInfo(path).suffix().toLower();
                    mime = ext == "webm" ? "video/webm" : ext == "mov" ? "video/quicktime" : "video/mp4";
                } else {
                    // Redrawn and saved fresh: location, camera and time stay behind.
                    QImageReader reader(path);
                    reader.setAutoTransform(true);
                    QImage image = reader.read();
                    if (image.isNull()) throw api::UserFacingError("That photo couldn't be opened.");
                    if (std::max(image.width(), image.height()) > kMaxImageSide) {
                        image = image.scaled(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    QByteArray encoded;
                    QBuffer buffer(&encoded);
                    buffer.open(QIODevice::WriteOnly);
                    const bool png = image.hasAlphaChannel();
                    if (!image.save(&buffer, png ? "PNG" : "JPEG", png ? -1 : 90)) throw api::UserFacingError("That photo couldn't be prepared.");
                    plain.assign(encoded.constData(), static_cast<std::size_t>(encoded.size()));
                    mime = png ? "image/png" : "image/jpeg";
                }
                std::istringstream in(plain);
                std::ostringstream out;
                const auto encrypted = crypto::files::encrypt(in, out);
                ciphertext = out.str();
                if (ciphertext.size() > eapi::kMaxPostMediaBytes) throw api::UserFacingError("That file is too large for a post.");
                postBody.media = em::PostMedia{crypto::base64Encode(encrypted.key.view()), crypto::base64Encode(encrypted.header),
                                               encrypted.ciphertextSha256, encrypted.encryptedSize, crypto::files::kChunkBytes, mime};
            }
            const std::string json = em::toJson(postBody);
            em::parseBody(json);  // the same checks every reader makes
            const auto sealed = pc::sealBody(id, key, json);
            const auto wrapped = pc::wrapPostKey(id, key, identity->publicKey, identity->privateKey.view());
            eapi::createPost(*api, {id, price, teaserText, crypto::base64Encode(sealed.ciphertext), crypto::base64Encode(sealed.nonce),
                                    crypto::base64Encode(wrapped),
                                    ciphertext.empty() ? std::nullopt : std::optional<std::uint64_t>(ciphertext.size())});
            if (!ciphertext.empty()) eapi::uploadPostMedia(*api, id, std::move(ciphertext));
        },
        [this]() {
            publishStage_.clear();
            emit postsChanged();
            emit published();
            setMessages({}, QStringLiteral("Published."));
            loadMyPosts();
            loadDashboard();
        },
        [this](const Failure& failure) {
            publishStage_.clear();
            emit postsChanged();
            if (failure.status == 401) emit sessionExpired();
            setMessages(failure.message, {});
        });
}

void EarningsSession::unpublishPost(const QString& postIdText) {
    auto api = api_;
    const std::string postId = postIdText.toStdString();
    runInBackground(
        pool_, this, [api, postId]() { eapi::unpublishPost(*api, postId); },
        [this]() {
            setMessages({}, QStringLiteral("Off sale. People who bought it keep it."));
            loadMyPosts();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't change that. Try again.")); });
}

void EarningsSession::refund(const QString& purchaseIdText) {
    auto api = api_;
    const std::string purchaseId = purchaseIdText.toStdString();
    setBusy(true);
    runInBackground(
        pool_, this, [api, purchaseId]() { eapi::refund(*api, purchaseId); },
        [this]() {
            setBusy(false);
            setMessages({}, QStringLiteral("Refund started. The buyer gets it all back."));
            loadDashboard();
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure, QStringLiteral("Couldn't refund that. Try again."));
        });
}

// --- Creator profile and payouts ---

void EarningsSession::loadCreator() {
    auto api = api_;
    runInBackground(
        pool_, this,
        [api]() {
            CreatorResult r;
            r.setup = eapi::creatorSetup(*api);
            r.creator = eapi::myCreator(*api);
            // Back from Stripe's onboarding (or any visit while it's unfinished): ask Stripe how it went.
            if (r.creator && r.creator->paymentsConfigured && r.creator->paymentsStatus != "not_started" && !r.creator->paymentsReady) {
                try {
                    r.creator = eapi::refreshPayoutSetup(*api);
                } catch (const api::ApiError& e) {
                    if (e.status() == 401) throw;
                }
            }
            return r;
        },
        [this](CreatorResult r) {
            const bool wasReady = creator_ && creator_->paymentsReady;
            creatorSetup_ = std::move(r.setup);
            creator_ = std::move(r.creator);
            creatorLoaded_ = true;
            emit creatorChanged();
            if (creator_ && creator_->paymentsReady && !wasReady) refresh();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't load your creator profile.")); });
}

void EarningsSession::becomeCreator(const QString& handle, const QString& displayName) {
    const QString h = handle.trimmed().toLower();
    const QString name = displayName.trimmed();
    if (h.size() < 3 || name.isEmpty()) {
        setMessages(QStringLiteral("Pick a handle (at least 3 characters) and a display name."), {});
        return;
    }
    auto api = api_;
    setBusy(true);
    runInBackground(
        pool_, this, [api, h = h.toStdString(), name = name.toStdString()]() { return eapi::becomeCreator(*api, h, name); },
        [this](eapi::Creator c) {
            setBusy(false);
            creator_ = std::move(c);
            emit creatorChanged();
            setMessages({}, QStringLiteral("Creator profile created. Next: payouts."));
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure, QStringLiteral("Couldn't create your creator profile."));
        });
}

void EarningsSession::startPayoutSetup(const QString& country) {
    if (!creator_) return;
    auto api = api_;
    const bool enable = !creator_->paidMembershipsEnabled;
    setBusy(true);
    runInBackground(
        pool_, this,
        [api, enable, country = country.toStdString()]() {
            // Payouts are shared with paid communities, which the server switches on with them.
            if (enable) eapi::enablePayouts(*api);
            return eapi::startPayoutSetup(*api, country);
        },
        [this](std::string link) {
            setBusy(false);
            const QUrl url(qs(link));
            if (!net::isHttpsUrl(url.toString(QUrl::FullyEncoded).toStdString()) || !QDesktopServices::openUrl(url)) {
                setMessages(QStringLiteral("Couldn't open Stripe's page in your browser."), {});
                return;
            }
            setMessages({}, QStringLiteral("Finish payout setup on Stripe's page in your browser, then press Check status."));
            loadCreator();
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure, QStringLiteral("Couldn't start payout setup."));
        });
}

void EarningsSession::refreshPayoutSetup() {
    auto api = api_;
    setBusy(true);
    runInBackground(
        pool_, this, [api]() { return eapi::refreshPayoutSetup(*api); },
        [this](eapi::Creator c) {
            setBusy(false);
            const bool ready = c.paymentsReady;
            creator_ = std::move(c);
            emit creatorChanged();
            setMessages({}, ready ? QStringLiteral("Payouts are set up.") : QStringLiteral("Status updated."));
            if (ready) refresh();
        },
        [this](const Failure& failure) {
            setBusy(false);
            fail(failure, QStringLiteral("Couldn't check with Stripe."));
        });
}

void EarningsSession::openStripeDashboard() {
    auto api = api_;
    runInBackground(
        pool_, this, [api]() { return eapi::stripeDashboardLink(*api); },
        [this](std::string link) {
            const QUrl url(qs(link));
            if (!net::isHttpsUrl(url.toString(QUrl::FullyEncoded).toStdString()) || !QDesktopServices::openUrl(url)) {
                setMessages(QStringLiteral("Couldn't open Stripe in your browser."), {});
            }
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't open your Stripe dashboard.")); });
}

// --- The creator's key delivery ---

void EarningsSession::deliverKeys() {
    if (delivering_ || !config_.enabled || !settings_.creatorReady) return;
    delivering_ = true;
    auto api = api_;
    auto identity = identity_;
    const auto chatKeys = chatKeysOf(chats_);
    runInBackground(
        pool_, this,
        [api, identity, chatKeys]() {
            // Buyers whose payment the server confirmed get the post's key sealed to them. Only this
            // app can (the server never holds it); one not delivered within 72 hours is refunded.
            int delivered = 0;
            std::map<std::string, std::shared_ptr<const crypto::SecretBytes>> keys;
            for (const auto& r : eapi::keyRequests(*api)) {
                try {
                    // Someone you chat with: only to the key your chat shows, so a server can't slip in its own.
                    const auto chat = chatKeys.find(r.buyerId);
                    if (chat != chatKeys.end() && !chat->second.empty() && chat->second != r.buyerPublicKey) continue;
                    auto it = keys.find(r.postId);
                    if (it == keys.end()) {
                        const auto post = eapi::post(*api, r.postId);
                        if (!post.mine || !post.body) continue;
                        auto key = std::make_shared<const crypto::SecretBytes>(
                            pc::unwrapPostKey(post.id, crypto::base64Decode(post.body->wrappedKey), identity->publicKey, identity->privateKey.view()));
                        it = keys.emplace(r.postId, std::move(key)).first;
                    }
                    const auto sealed = pc::wrapPostKey(r.postId, it->second->view(), crypto::base64Decode(r.buyerPublicKey), identity->privateKey.view());
                    eapi::deliverPostKey(*api, r.postId, r.purchaseId, crypto::base64Encode(sealed));
                    ++delivered;
                } catch (const api::ApiError& e) {
                    if (e.status() == 401) throw;
                } catch (const std::exception&) {
                    // One that fails (refunded meanwhile, say) doesn't stop the rest.
                }
            }
            return delivered;
        },
        [this](int delivered) {
            delivering_ = false;
            if (delivered > 0 && dashboard_.value("loaded").toBool()) loadDashboard();
        },
        [this](const Failure& failure) {
            delivering_ = false;
            if (failure.status == 401) emit sessionExpired();
        });
}

// --- Messages ---

void EarningsSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit messagesChanged();
}

void EarningsSession::setMessages(const QString& error, const QString& notice) {
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

void EarningsSession::clearMessages() {
    if (error_.isEmpty() && notice_.isEmpty()) return;
    setMessages({}, {});
}

void EarningsSession::fail(const Failure& failure, const QString& fallback) {
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    setMessages(worded(failure, fallback), {});
}

void EarningsSession::dropTempFiles() {
    for (const auto& path : tempFiles_) QFile::remove(path);
    tempFiles_.clear();
    QDir(mediaDir()).removeRecursively();
}

}  // namespace redvault::app
