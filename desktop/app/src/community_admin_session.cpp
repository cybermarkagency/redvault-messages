// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "community_admin_session.hpp"

#include <QJsonDocument>

#include <type_traits>
#include <utility>

#include "redvault/api/community_api.hpp"

namespace redvault::app {

namespace {

namespace community_api = api::community;
using Json = api::Json;

/// The server's JSON as QML sees it: objects become maps, arrays lists.
QVariant toVariant(const Json& j) {
    if (j.is_object() || j.is_array()) return QJsonDocument::fromJson(QByteArray::fromStdString(j.dump())).toVariant();
    return {};
}

std::optional<std::string> optionalText(const QString& text) {
    const QString trimmed = text.trimmed();
    return trimmed.isEmpty() ? std::nullopt : std::optional<std::string>(trimmed.toStdString());
}

const QStringList kSections{QStringLiteral("overview"), QStringLiteral("transactions"), QStringLiteral("creators"),
                            QStringLiteral("communities"), QStringLiteral("reports")};

}  // namespace

CommunityAdminSession::CommunityAdminSession(std::shared_ptr<api::ApiClient> api, QObject* parent)
    : QObject(parent), api_(std::move(api)) {
    pool_.setMaxThreadCount(2);
}

CommunityAdminSession::~CommunityAdminSession() {
    pool_.clear();
    pool_.waitForDone();
}

// --- Plumbing ---

template <typename Work, typename Done>
void CommunityAdminSession::act(Work work, Done done) {
    setBusy(true);
    setMessages({}, {});
    using Result = std::invoke_result_t<Work&>;
    auto onFail = [this](const Failure& failure) {
        setBusy(false);
        fail(failure);
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

void CommunityAdminSession::setLoaded(bool loaded) {
    if (loaded_ == loaded) return;
    loaded_ = loaded;
    emit loadedChanged();
}

void CommunityAdminSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void CommunityAdminSession::setMessages(const QString& error, const QString& notice) {
    if (error_ == error && notice_ == notice) return;
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

void CommunityAdminSession::clearMessages() {
    setMessages({}, {});
}

void CommunityAdminSession::fail(const Failure& failure) {
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    // The server's own words, as the web app shows err.message (403 for anyone who isn't a super admin).
    setMessages(failure.message, {});
}

// --- Sections ---

void CommunityAdminSession::open(const QString& section) {
    if (!kSections.contains(section)) return;
    if (section_ != section) {
        section_ = section;
        setMessages({}, {});
        emit sectionChanged();
    }
    setLoaded(false);
    load(section_);
}

void CommunityAdminSession::reload() {
    load(section_);
}

void CommunityAdminSession::load(const QString& section) {
    const int generation = ++generation_;
    auto api = api_;
    const std::string filter = transactionFilter_.toStdString();
    runInBackground(
        pool_, this,
        [api, section, filter] {
            if (section == QStringLiteral("overview")) return toVariant(community_api::admin::overview(*api));
            if (section == QStringLiteral("transactions")) return toVariant(community_api::admin::transactions(*api, filter));
            if (section == QStringLiteral("creators")) return toVariant(community_api::admin::creators(*api));
            if (section == QStringLiteral("communities")) return toVariant(community_api::admin::communities(*api));
            return toVariant(community_api::admin::reports(*api, "open"));
        },
        [this, generation, section](QVariant result) {
            if (generation != generation_) return;  // a newer load is out
            if (section == QStringLiteral("overview")) {
                overview_ = result.toMap();
                emit overviewChanged();
            } else if (section == QStringLiteral("transactions")) {
                transactions_ = result.toList();
                emit transactionsChanged();
            } else if (section == QStringLiteral("creators")) {
                creators_ = result.toList();
                emit creatorsChanged();
            } else if (section == QStringLiteral("communities")) {
                communityList_ = result.toList();
                emit communityListChanged();
            } else {
                reports_ = result.toList();
                emit reportsChanged();
            }
            setLoaded(true);
        },
        [this, generation](const Failure& failure) {
            if (generation != generation_) return;
            setLoaded(true);
            fail(failure);
        });
}

// --- Overview ---

void CommunityAdminSession::addCommissionRule(const QString& ratePercent, const QString& minDollars, const QString& maxDollars,
                                              const QString& note) {
    const auto rateBps = community_api::parseRateBps(ratePercent.toStdString());
    const auto minMinor = community_api::parseDollars(minDollars.toStdString());
    const auto maxMinor = community_api::parseDollars(maxDollars.toStdString());
    if (!rateBps || !minMinor || !maxMinor) {
        setMessages(QStringLiteral("Check the numbers."), {});
        return;
    }
    auto api = api_;
    const std::string why = note.trimmed().toStdString();
    act([api, rateBps, minMinor, maxMinor, why] { community_api::admin::addCommissionRule(*api, *rateBps, *minMinor, *maxMinor, why); },
        [this] {
            setMessages({}, QStringLiteral("Saved as a new version."));
            if (section_ == QStringLiteral("overview")) load(section_);
        });
}

// --- Transactions ---

void CommunityAdminSession::setTransactionFilter(const QString& filter) {
    if (filter != QString() && filter != QStringLiteral("pending") && filter != QStringLiteral("matched") && filter != QStringLiteral("mismatch")) return;
    if (transactionFilter_ != filter) {
        transactionFilter_ = filter;
        emit transactionsChanged();
    }
    if (section_ == QStringLiteral("transactions")) load(section_);
}

void CommunityAdminSession::refund(const QString& transactionId, const QString& amountDollars, const QString& reason) {
    std::optional<std::int64_t> amount;
    if (!amountDollars.trimmed().isEmpty()) {
        amount = community_api::parseDollars(amountDollars.toStdString());
        if (!amount || *amount <= 0) {
            setMessages(QStringLiteral("Enter an amount like 5 or 4.99."), {});
            return;
        }
    }
    const auto why = optionalText(reason);
    if (!why) {
        setMessages(QStringLiteral("A reason is required."), {});
        return;
    }
    auto api = api_;
    const std::string id = transactionId.toStdString();
    act([api, id, amount, why] { community_api::admin::refund(*api, id, amount, *why); },
        [this] {
            setMessages({}, QStringLiteral("Refund sent to Stripe."));
            if (section_ == QStringLiteral("transactions")) load(section_);
        });
}

void CommunityAdminSession::reconcile() {
    auto api = api_;
    act([api] { return community_api::admin::reconcile(*api); },
        [this](Json result) {
            const auto count = [&](const char* key) { return QString::number(result.value(key, 0LL)); };
            setMessages({}, QStringLiteral("Checked %1; %2 mismatch(es); %3 unbalanced journal(s).")
                                .arg(count("checked"), count("mismatches"), count("unbalancedJournals")));
            if (section_ == QStringLiteral("transactions") || section_ == QStringLiteral("overview")) load(section_);
        });
}

// --- Creators and communities ---

void CommunityAdminSession::setCreatorVerified(const QString& creatorId, bool verified) {
    auto api = api_;
    const std::string id = creatorId.toStdString();
    act([api, id, verified] { community_api::admin::setCreator(*api, id, std::nullopt, verified); },
        [this] { if (section_ == QStringLiteral("creators")) load(section_); });
}

void CommunityAdminSession::setCreatorStatus(const QString& creatorId, const QString& status) {
    if (status != QStringLiteral("active") && status != QStringLiteral("suspended")) return;
    auto api = api_;
    const std::string id = creatorId.toStdString();
    const std::string value = status.toStdString();
    act([api, id, value] { community_api::admin::setCreator(*api, id, value, std::nullopt); },
        [this] { if (section_ == QStringLiteral("creators")) load(section_); });
}

void CommunityAdminSession::communityAction(const QString& communityId, const QString& action) {
    auto api = api_;
    const std::string id = communityId.toStdString();
    const std::string what = action.toStdString();
    act([api, id, what] { community_api::admin::communityAction(*api, id, what); },
        [this] { if (section_ == QStringLiteral("communities")) load(section_); });
}

// --- Reports ---

void CommunityAdminSession::resolveReport(const QString& reportId, const QString& status) {
    if (status != QStringLiteral("actioned") && status != QStringLiteral("dismissed")) return;
    auto api = api_;
    const std::string id = reportId.toStdString();
    const std::string value = status.toStdString();
    act([api, id, value] { community_api::admin::resolveReport(*api, id, value, std::nullopt); },
        [this] { if (section_ == QStringLiteral("reports")) load(section_); });
}

}  // namespace redvault::app
