#include "group_invite_session.hpp"

#include <optional>
#include <vector>

#include "account_session.hpp"
#include "async.hpp"
#include "chat_session.hpp"
#include "redvault/api/group_invite_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/group_invite_crypto.hpp"

namespace redvault::app {

namespace {

namespace invites = crypto::invites;
using MaybeLink = std::optional<api::invites::Link>;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

/// The server's own words for an API error (as the web app shows err.message), or `fallback` for anything else.
QString worded(const Failure& failure, const QString& fallback) {
    return failure.code.isEmpty() ? fallback : failure.message;
}

struct Requester {
    std::string userId;
    std::string handle;
    std::string avatarUrl;
};

}  // namespace

GroupInviteSession::GroupInviteSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                                       ChatSession* chats, QString webAppBase, QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)), chats_(chats), webAppBase_(std::move(webAppBase)) {
    pool_.setMaxThreadCount(2);
}

GroupInviteSession::~GroupInviteSession() {
    pool_.clear();
    pool_.waitForDone();
}

bool GroupInviteSession::canManage() const {
    const auto meta = chats_ && !chatId_.empty() ? chats_->metaFor(chatId_) : nullptr;
    return meta && meta->isGroup && (meta->myRole == "owner" || meta->myRole == "admin");
}

// --- Group info > Invite link ---

void GroupInviteSession::load(const QString& chatId) {
    ++generation_;
    chatId_ = chatId.toStdString();
    linkState_ = QStringLiteral("loading");
    linkUrl_.clear();
    approval_ = false;
    linkSecret_.reset();
    requests_.clear();
    setError({});
    emit linkChanged();
    emit requestsChanged();
    const auto meta = chats_ ? chats_->metaFor(chatId_) : nullptr;
    if (!meta || !meta->isGroup) {
        linkState_ = QStringLiteral("none");
        emit linkChanged();
        return;
    }
    auto api = api_;
    const std::string id = chatId_;
    const int generation = generation_;
    runInBackground(
        pool_, this, [api, id] { return api::invites::getLink(*api, id); },
        [this, id, generation](MaybeLink link) {
            if (generation != generation_) return;
            showLink(id, link);
            loadRequests();
        },
        [this, id, generation](const Failure& failure) {
            if (generation != generation_) return;
            if (failure.status == 401) emit sessionExpired();
            // Like the web app: a link that can't be read shows as none.
            showLink(id, std::nullopt);
        });
}

void GroupInviteSession::showLink(const std::string& chatId, const MaybeLink& link) {
    linkSecret_.reset();
    linkUrl_.clear();
    approval_ = false;
    linkState_ = link ? QStringLiteral("on") : QStringLiteral("none");
    if (link) {
        approval_ = link->approval;
        const auto meta = chats_ ? chats_->metaFor(chatId) : nullptr;
        if (meta) {
            try {
                const auto key = chats_->groupRepository()->currentKey(*meta);
                if (auto opened = invites::openInviteLink(link->id, link->secretCiphertext, link->secretNonce, key, webAppBase_.toStdString())) {
                    linkSecret_ = std::make_shared<crypto::SecretBytes>(std::move(opened->secret));
                    linkUrl_ = qs(opened->url);
                }
            } catch (const api::UserFacingError&) {
                // A refused key: the link can't be shown, and the chat says why.
            }
        }
    }
    emit linkChanged();
}

void GroupInviteSession::loadRequests() {
    if (!canManage()) return;
    auto api = api_;
    const std::string id = chatId_;
    const int generation = generation_;
    runInBackground(
        pool_, this,
        [api, id] {
            std::vector<Requester> out;
            for (const auto& r : api::invites::listRequests(*api, id)) {
                Requester named{r.userId, "Unknown account", {}};
                try {
                    const auto user = api->getUser(r.userId);
                    named.handle = user.username;
                    named.avatarUrl = user.avatarUrl.value_or("");
                } catch (const std::exception&) {
                    // Shown as an unknown account; approving it fails cleanly if it's gone.
                }
                out.push_back(std::move(named));
            }
            return out;
        },
        [this, generation](std::vector<Requester> list) {
            if (generation != generation_) return;
            requests_.clear();
            for (const auto& r : list) {
                requests_.append(QVariantMap{{"userId", qs(r.userId)}, {"handle", qs(r.handle)}, {"avatarUrl", qs(r.avatarUrl)}});
            }
            emit requestsChanged();
        },
        [this](const Failure& failure) { failed(failure.status); });
}

template <typename Work>
void GroupInviteSession::linkAction(Work work, const QString& failureText) {
    if (busy_) return;
    const auto meta = chats_ ? chats_->metaFor(chatId_) : nullptr;
    if (!meta) return;
    setBusy(true);
    setError({});
    const std::string id = chatId_;
    const int generation = generation_;
    runInBackground(
        pool_, this, [work = std::move(work), meta] { return work(*meta); },
        [this, id, generation](MaybeLink link) {
            setBusy(false);
            if (generation != generation_) return;
            showLink(id, link);
            if (!link) {
                requests_.clear();
                emit requestsChanged();
            }
        },
        [this, failureText](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(worded(failure, failureText));
        });
}

void GroupInviteSession::createLink() {
    auto api = api_;
    auto groups = chats_ ? chats_->groupRepository() : nullptr;
    linkAction(
        [api, groups](const data::ConversationMeta& meta) -> MaybeLink {
            // A link can't hand out a key nobody verified: this throws the reason when it was refused.
            const auto link = invites::newInviteLink(groups->currentKey(meta), meta.handle, true);
            return api::invites::setLink(*api, meta.id, link.input);
        },
        QStringLiteral("Couldn't create the link."));
}

void GroupInviteSession::resetLink() {
    auto api = api_;
    auto groups = chats_ ? chats_->groupRepository() : nullptr;
    const bool approval = approval_;
    linkAction(
        [api, groups, approval](const data::ConversationMeta& meta) -> MaybeLink {
            const auto link = invites::newInviteLink(groups->currentKey(meta), meta.handle, approval);
            return api::invites::setLink(*api, meta.id, link.input);
        },
        QStringLiteral("Couldn't reset the link."));
}

void GroupInviteSession::setApproval(bool on) {
    // Turning approval off needs the link's secret, to wrap the group key under it.
    if (!linkSecret_ || on == approval_) return;
    auto api = api_;
    auto groups = chats_ ? chats_->groupRepository() : nullptr;
    auto secret = linkSecret_;
    linkAction(
        [api, groups, secret, on](const data::ConversationMeta& meta) -> MaybeLink {
            std::optional<invites::WrappedKey> wrapped;
            if (!on) wrapped = invites::wrapGroupKeyForLink(groups->currentKey(meta), *secret);
            return api::invites::setApproval(*api, meta.id, on, wrapped);
        },
        QStringLiteral("Couldn't change that setting."));
    // The switch shows what the server has until it answers.
    emit linkChanged();
}

void GroupInviteSession::turnOff() {
    auto api = api_;
    linkAction(
        [api](const data::ConversationMeta& meta) -> MaybeLink {
            api::invites::deleteLink(*api, meta.id);
            return std::nullopt;
        },
        QStringLiteral("Couldn't turn off the link."));
}

void GroupInviteSession::approve(const QString& userId) {
    const auto meta = chats_ ? chats_->metaFor(chatId_) : nullptr;
    if (busy_ || !meta) return;
    setBusy(true);
    setError({});
    auto groups = chats_->groupRepository();
    const std::string requester = userId.toStdString();
    runInBackground(
        pool_, this, [groups, meta, requester] { groups->approveJoinRequest(*meta, requester); },
        [this, chatId = meta->id] {
            setBusy(false);
            if (chats_) chats_->groupChanged(chatId);
            loadRequests();
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(worded(failure, QStringLiteral("Couldn't let them in.")));
            loadRequests();
        });
}

void GroupInviteSession::decline(const QString& userId) {
    if (busy_ || chatId_.empty()) return;
    setBusy(true);
    setError({});
    auto api = api_;
    const std::string id = chatId_;
    const std::string requester = userId.toStdString();
    runInBackground(
        pool_, this, [api, id, requester] { api::invites::deny(*api, id, requester); },
        [this] {
            setBusy(false);
            loadRequests();
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setError(worded(failure, QStringLiteral("Couldn't decline the request.")));
        });
}

void GroupInviteSession::copyLink() {
    if (linkUrl_.isEmpty()) return;
    // Anyone holding the link can join: keep it out of clipboard history and cloud clipboard sync.
    AccountSession::copySecret(linkUrl_);
    emit copied();
}

void GroupInviteSession::clearError() {
    setError({});
}

// --- Joining ---

bool GroupInviteSession::isInviteLink(const QString& text) const {
    return invites::parseInvite(text.toStdString()).has_value();
}

bool GroupInviteSession::isOwnInviteLink(const QString& text) const {
    return text.trimmed().startsWith(webAppBase_, Qt::CaseInsensitive) && isInviteLink(text);
}

void GroupInviteSession::startPaste() {
    ++joinGeneration_;
    Join fresh;
    fresh.state = QStringLiteral("paste");
    setJoin(std::move(fresh));
    emit showJoin();
}

QString GroupInviteSession::openInvite(const QString& text) {
    const auto parsed = invites::parseInvite(text.toStdString());
    if (!parsed) return QStringLiteral("That doesn't look like a group invite link. Paste the whole link, including the part after #.");
    const int generation = ++joinGeneration_;
    Join opening;
    opening.linkId = parsed->linkId;
    auto secret = invites::secretFromFragment(parsed->secret);
    if (!secret) {
        opening.state = QStringLiteral("error");
        opening.error = QStringLiteral("This link is incomplete. Ask for it again, and open the whole link.");
        setJoin(std::move(opening));
        emit showJoin();
        return {};
    }
    opening.state = QStringLiteral("opening");
    opening.secret = std::make_shared<crypto::SecretBytes>(std::move(*secret));
    auto shared = opening.secret;
    setJoin(std::move(opening));
    emit showJoin();

    struct Opened {
        api::invites::Preview preview;
        std::string name;
        std::shared_ptr<crypto::SecretBytes> groupKey;
    };
    auto api = api_;
    const std::string linkId = parsed->linkId;
    runInBackground(
        pool_, this,
        [api, linkId, shared] {
            Opened out;
            out.preview = api::invites::open(*api, linkId, invites::inviteKeys(*shared).auth);
            try {
                auto opened = invites::openInvitePreview(out.preview.previewCiphertext, out.preview.previewNonce, out.preview.wrappedKey,
                                                         out.preview.wrappedKeyNonce, *shared);
                out.name = opened.name;
                if (opened.groupKey) out.groupKey = std::make_shared<crypto::SecretBytes>(std::move(*opened.groupKey));
            } catch (const std::exception&) {
                throw api::UserFacingError("This link's secret doesn't match. It may have been reset: ask the group for a new one.");
            }
            return out;
        },
        [this, generation](Opened opened) {
            if (generation != joinGeneration_) return;
            Join ready = join_;
            ready.state = QStringLiteral("ready");
            ready.conversationId = opened.preview.conversationId;
            ready.name = qs(opened.name);
            ready.memberCount = opened.preview.memberCount;
            ready.approval = opened.preview.approval;
            ready.member = opened.preview.member;
            ready.requested = opened.preview.requested;
            ready.groupKey = opened.groupKey;
            setJoin(std::move(ready));
        },
        [this, generation](const Failure& failure) {
            if (generation != joinGeneration_) return;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            Join broken = join_;
            broken.state = QStringLiteral("error");
            // An unknown link and a wrong secret get the same 404 and the server's own words; anything else is worth a retry.
            broken.error = failure.status == 404 || failure.code.isEmpty() ? failure.message : QStringLiteral("Couldn't open this link. Try again.");
            setJoin(std::move(broken));
        });
    return {};
}

void GroupInviteSession::join() {
    if (join_.state != QStringLiteral("ready") || join_.busy || join_.member || join_.requested || !join_.secret) return;
    const int generation = joinGeneration_;
    Join busy = join_;
    busy.busy = true;
    busy.error.clear();
    setJoin(std::move(busy));
    auto api = api_;
    auto identity = identity_;
    auto groups = chats_ ? chats_->groupRepository() : nullptr;
    const std::string linkId = join_.linkId;
    auto secret = join_.secret;
    auto groupKey = join_.groupKey;
    runInBackground(
        pool_, this,
        [api, identity, groups, linkId, secret, groupKey] {
            // When the link carries the group key, we seal ourselves our own copy, as an admin adding us would.
            std::optional<std::string> wrapped;
            if (groupKey) wrapped = crypto::base64Encode(crypto::sealConversationKey(*groupKey, identity->publicKey));
            auto result = api::invites::join(*api, linkId, invites::inviteKeys(*secret).auth, wrapped);
            // Joining through a link adds this person with no other member present, so they confirm themselves.
            if (groups && groupKey && result.status == "joined") groups->confirmSelfAfterJoin(result.conversationId, *groupKey);
            return result;
        },
        [this, generation](api::invites::JoinResult result) {
            if (generation != joinGeneration_) return;
            Join done = join_;
            done.busy = false;
            if (result.status == "requested") {
                done.requested = true;
                setJoin(std::move(done));
                return;
            }
            done.member = true;
            setJoin(std::move(done));
            if (chats_) chats_->openWhenListed(result.conversationId);
            emit joined();
        },
        [this, generation](const Failure& failure) {
            if (generation != joinGeneration_) return;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            Join broken = join_;
            broken.busy = false;
            broken.error = worded(failure, QStringLiteral("Couldn't join the group. Try again."));
            setJoin(std::move(broken));
        });
}

void GroupInviteSession::cancelRequest() {
    if (!join_.requested || join_.busy || join_.conversationId.empty()) return;
    const int generation = joinGeneration_;
    Join busy = join_;
    busy.busy = true;
    busy.error.clear();
    setJoin(std::move(busy));
    auto api = api_;
    const std::string conversationId = join_.conversationId;
    const std::string self = identity_->userId;
    runInBackground(
        pool_, this, [api, conversationId, self] { api::invites::deny(*api, conversationId, self); },
        [this, generation] {
            if (generation != joinGeneration_) return;
            Join done = join_;
            done.busy = false;
            done.requested = false;
            setJoin(std::move(done));
        },
        [this, generation](const Failure& failure) {
            if (generation != joinGeneration_) return;
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            Join broken = join_;
            broken.busy = false;
            broken.error = QStringLiteral("Couldn't cancel your request. Try again.");
            setJoin(std::move(broken));
        });
}

void GroupInviteSession::openGroup() {
    if (join_.conversationId.empty() || !chats_) return;
    chats_->openWhenListed(join_.conversationId);
    emit joined();
}

void GroupInviteSession::closeJoin() {
    ++joinGeneration_;
    setJoin(Join{});
}

void GroupInviteSession::setJoin(Join join) {
    join_ = std::move(join);
    emit joinChanged();
}

void GroupInviteSession::failed(int status) {
    if (status == 401) emit sessionExpired();
}

void GroupInviteSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void GroupInviteSession::setError(const QString& error) {
    if (error_ == error) return;
    error_ = error;
    emit errorChanged();
}

}  // namespace redvault::app
