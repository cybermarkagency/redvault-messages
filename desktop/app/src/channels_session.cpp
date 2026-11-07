#include "channels_session.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QSettings>

#include <QQmlEngine>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <utility>

#include "async.hpp"
#include "avatar_jpeg.hpp"
#include "file_safety.hpp"
#include "prefs.hpp"
#include "redvault/crypto/channel_crypto.hpp"
#include "redvault/model/channel_posts.hpp"
#include "redvault/model/text_format.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace {

namespace channels = api::channels;
namespace follows = model::channels;
namespace links = crypto::channels;
namespace posts = model::channel_posts;

constexpr const char* kFollowedKey = "followedChannels";
/// Which new posts were already notified (per account; the web app keeps it in localStorage).
constexpr const char* kNotifiedKey = "channelNotified";
/// Posts this computer already counted a view for. For the whole computer, like the web app's localStorage list (lib/channelViews.ts).
constexpr const char* kViewedKey = "channels/viewedPosts";
/// Your votes, for the whole computer (the web's "redvaultsystem:channel-votes").
constexpr const char* kVotesKey = "channels/votes";

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

std::optional<QDateTime> localTime(const std::string& iso) {
    const auto ms = util::parseIsoMillis(iso);
    if (!ms) return std::nullopt;
    return QDateTime::fromMSecsSinceEpoch(*ms).toLocalTime();
}

/// "18 Oct 2026" for a list row, as the web app's toLocaleDateString(day, short month, year).
QString lastPostLabel(const std::optional<std::string>& iso) {
    const auto at = iso ? localTime(*iso) : std::nullopt;
    return at ? QLocale().toString(at->date(), QStringLiteral("d MMM yyyy")) : QStringLiteral("No posts yet");
}

/// What ChannelAvatar hands the "channelavatar" image provider: the photo's URL and the access token, if any.
QString photoKey(const channels::Channel& c, const std::optional<std::string>& access) {
    if (!c.avatarUrl) return {};
    return qs(*c.avatarUrl) + u'|' + qs(access.value_or(""));
}

/// The server's own words for an API error (as the web app shows err.message), or `fallback` for anything else.
QString worded(const Failure& failure, const QString& fallback) {
    return failure.code.isEmpty() ? fallback : failure.message;
}

std::vector<std::string> trimmedList(const QVariant& value) {
    std::vector<std::string> out;
    for (const auto& item : value.toList()) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) out.push_back(text.toStdString());
    }
    return out;
}

/// A publish time from the composer: only one in the future schedules (as the web's `scheduledFor > Date.now()`).
std::optional<std::string> futureTime(const QString& iso) {
    if (iso.isEmpty()) return std::nullopt;
    const auto ms = util::parseIsoMillis(iso.toStdString());
    if (!ms || *ms <= QDateTime::currentMSecsSinceEpoch()) return std::nullopt;
    return iso.toStdString();
}

QVariantMap suggestionOf(const channels::Suggestion& s) {
    QVariantList photos;
    for (const auto& m : s.media) photos.append(ChannelPostModel::mediaOf(m));
    const QString status = s.status == "approved" ? QStringLiteral("Published")
                           : s.status == "rejected" ? QStringLiteral("Not published")
                                                    : QStringLiteral("Waiting for the owner");
    return {{"id", qs(s.id)},
            {"body", qs(s.body)},
            {"bodyRich", qs(model::text_format::toHtml(s.body))},
            {"sent", ChannelPostModel::when(s.createdAt)},
            {"status", status},
            {"photos", photos}};
}

}  // namespace

ChannelsSession::ChannelsSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                                 QString webAppBase, std::function<std::string()> tier, QObject* parent)
    : QObject(parent),
      api_(std::move(api)),
      identity_(std::move(identity)),
      webAppBase_(std::move(webAppBase)),
      tier_(std::move(tier)),
      posts_(std::make_unique<ChannelPostModel>([this](const std::string& id) { return rememberedVote(id); })),
      postUploads_(std::make_unique<ChannelUploads>(api_, channels::kMaxMediaPerPost, false)),
      suggestionUploads_(std::make_unique<ChannelUploads>(api_, channels::kMaxMediaPerSuggestion, true)) {
    pool_.setMaxThreadCount(3);
    // QML reads these through properties; they stay this session's.
    QQmlEngine::setObjectOwnership(posts_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(postUploads_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(suggestionUploads_.get(), QQmlEngine::CppOwnership);
    followed_ = follows::fromJson(prefs::text(identity_->userId, kFollowedKey).toStdString());
    connect(postUploads_.get(), &ChannelUploads::sessionExpired, this, &ChannelsSession::sessionExpired);
    connect(suggestionUploads_.get(), &ChannelUploads::sessionExpired, this, &ChannelsSession::sessionExpired);

    // New posts in followed channels: checked here, every couple of minutes and when the window comes back.
    checkTimer_.setInterval(posts::kCheckEveryMs);
    connect(&checkTimer_, &QTimer::timeout, this, &ChannelsSession::checkFollowed);
    checkTimer_.start();
    soonTimer_.setSingleShot(true);
    soonTimer_.setInterval(800);
    connect(&soonTimer_, &QTimer::timeout, this, &ChannelsSession::checkFollowed);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) scheduleCheck();
    });
    QTimer::singleShot(1500, this, &ChannelsSession::checkFollowed);
}

ChannelsSession::~ChannelsSession() {
    pool_.clear();
    pool_.waitForDone();
}

bool ChannelsSession::canCreate() const {
    return tier_ && tier_() == "premium";
}

bool ChannelsSession::isAdmin() const {
    return identity_->role == "super_admin";
}

bool ChannelsSession::isFollowing() const {
    return !channelId_.empty() && follows::isFollowing(followed_, channelId_);
}

bool ChannelsSession::isMuted() const {
    for (const auto& f : followed_) {
        if (f.id == channelId_) return f.muted;
    }
    return false;
}

void ChannelsSession::setPageShown(bool shown) {
    if (pageShown_ == shown) return;
    pageShown_ = shown;
    emit pageShownChanged();
}

std::optional<std::string> ChannelsSession::accessFor(const std::optional<std::string>& secret) const {
    if (!secret || secret->empty()) return std::nullopt;
    if (const auto it = accessCache_.find(*secret); it != accessCache_.end()) return it->second;
    try {
        return accessCache_[*secret] = links::accessToken(*secret);
    } catch (const crypto::CryptoError&) {
        return std::nullopt;
    }
}

std::optional<std::string> ChannelsSession::link() const {
    if (!channel_) return std::nullopt;
    if (channel_->isPrivate) {
        if (!secret_) return std::nullopt;
        return follows::channelUrl(webAppBase_.toStdString(), channel_->id, secret_);
    }
    return follows::channelUrl(webAppBase_.toStdString(), channel_->id);
}

std::optional<posts::RememberedVote> ChannelsSession::rememberedVote(const std::string& postId) const {
    return posts::rememberedVote(QSettings().value(QLatin1String(kVotesKey)).toString().toStdString(), postId);
}

QVariantMap ChannelsSession::rowOf(const channels::Channel& c, const std::optional<std::string>& access) const {
    return {{"id", qs(c.id)},
            {"name", qs(c.name)},
            {"description", qs(c.description)},
            {"when", lastPostLabel(c.lastPostAt)},
            {"unread", false},
            {"deleted", false},
            {"isPrivate", c.isPrivate},
            {"photo", photoKey(c, access)}};
}

QVariantMap ChannelsSession::channel() const {
    if (!channel_) return {};
    const auto shareable = link();
    return {{"id", qs(channel_->id)},
            {"name", qs(channel_->name)},
            {"description", qs(channel_->description)},
            {"isPrivate", channel_->isPrivate},
            {"acceptSuggestions", channel_->acceptSuggestions},
            {"hasPhoto", channel_->avatarUrl.has_value()},
            {"photo", photoKey(*channel_, accessFor(secret_))},
            {"link", shareable ? qs(*shareable) : QString()}};
}

QVariantList ChannelsSession::scheduled() const {
    QVariantList out;
    for (const auto& post : scheduled_) {
        QVariantMap row = ChannelPostModel::rowOf(post, rememberedVote(post.id));
        row["scheduledFor"] = QStringLiteral("Scheduled for ") + ChannelPostModel::when(post.createdAt);
        out.append(row);
    }
    return out;
}

QVariantList ChannelsSession::suggestions() const {
    QVariantList out;
    for (const auto& s : suggestions_) out.append(suggestionOf(s));
    return out;
}

QVariantList ChannelsSession::mySuggestions() const {
    QVariantList out;
    for (const auto& s : mySuggestions_) out.append(suggestionOf(s));
    return out;
}

QVariantList ChannelsSession::following() const {
    QVariantList out;
    for (const auto& f : followed_) {
        const auto it = fetched_.find(f.id);
        if (it != fetched_.end() && !it->second) {
            out.append(QVariantMap{{"id", qs(f.id)},
                                   {"name", qs(f.name)},
                                   {"description", QStringLiteral("This channel was deleted, or made private and you don't have its new link.")},
                                   {"when", QString()},
                                   {"unread", false},
                                   {"deleted", true},
                                   {"isPrivate", false},
                                   {"photo", QString()}});
        } else if (it != fetched_.end()) {
            QVariantMap row = rowOf(*it->second, accessFor(f.secret));
            row["unread"] = follows::hasUnread(f, *it->second);
            out.append(row);
        } else {
            // Not fetched (yet, or the server couldn't say): what this computer remembers.
            channels::Channel remembered{f.id, f.name, {}, {}, std::nullopt, f.secret.has_value(), std::nullopt, std::nullopt, false};
            out.append(rowOf(remembered, std::nullopt));
        }
    }
    return out;
}

// --- The list ---

void ChannelsSession::refreshFollowing() {
    std::vector<std::pair<std::string, std::optional<std::string>>> wanted;
    for (const auto& f : followed_) wanted.emplace_back(f.id, accessFor(f.secret));
    if (wanted.empty()) return;
    using Results = std::vector<std::pair<std::string, std::optional<channels::Channel>>>;
    auto api = api_;
    runInBackground(
        pool_, this,
        [api, wanted] {
            Results out;
            for (const auto& [id, access] : wanted) {
                try {
                    out.emplace_back(id, channels::get(*api, id, access));
                } catch (const api::ApiError& e) {
                    // Gone for good (or private, without its current link); anything else (offline, say) leaves the row as remembered.
                    if (e.status() == 404) out.emplace_back(id, std::nullopt);
                }
            }
            return out;
        },
        [this](Results results) {
            fetched_.clear();
            for (auto& [id, found] : results) fetched_[id] = std::move(found);
            emit followingChanged();
        },
        [](const Failure&) {});
}

void ChannelsSession::removeFollowed(const QString& id) {
    fetched_.erase(id.toStdString());
    saveFollowed(follows::unfollow(followed_, id.toStdString()));
}

void ChannelsSession::search(const QString& query) {
    discoverQuery_ = query;
    ++discoverGeneration_;
    searchPage(0);
}

void ChannelsSession::loadMoreDiscover() {
    if (!discoverMore_) return;
    searchPage(static_cast<int>(discover_.size()));
}

void ChannelsSession::searchPage(int offset) {
    auto api = api_;
    const std::string query = discoverQuery_.toStdString();
    const int generation = discoverGeneration_;
    runInBackground(
        pool_, this, [api, query, offset] { return channels::search(*api, query, offset); },
        [this, generation, offset](std::vector<channels::Channel> page) {
            if (generation != discoverGeneration_) return;
            if (offset == 0) discover_.clear();
            for (const auto& c : page) discover_.append(rowOf(c, std::nullopt));
            discoverMore_ = static_cast<int>(page.size()) == channels::kPageSize;
            discoverLoaded_ = true;
            discoverError_.clear();
            emit discoverChanged();
        },
        [this, generation, offset](const Failure&) {
            if (generation != discoverGeneration_) return;
            if (offset == 0) {
                discoverError_ = QStringLiteral("Couldn't load channels. Try again.");
            } else {
                discoverMore_ = false;
            }
            emit discoverChanged();
        });
}

void ChannelsSession::refreshMine() {
    auto api = api_;
    auto identity = identity_;
    runInBackground(
        pool_, this,
        [api, identity] {
            std::vector<OwnChannel> out;
            // A private channel's photo needs its link's token: this account's key unseals the secret.
            for (auto& c : channels::mine(*api)) {
                const auto secret = c.isPrivate ? links::openSecret(c.secretSealed, identity->publicKey, identity->privateKey) : std::nullopt;
                out.push_back(OwnChannel{std::move(c), secret ? std::optional<std::string>(links::accessToken(*secret)) : std::nullopt});
            }
            return out;
        },
        [this](std::vector<OwnChannel> list) { applyMine(std::move(list)); },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
            mine_.clear();
            mineIds_.clear();
            mineLoaded_ = true;
            emit mineChanged();
        });
}

void ChannelsSession::applyMine(std::vector<OwnChannel> list) {
    mine_.clear();
    mineIds_.clear();
    for (const auto& own : list) {
        mine_.append(rowOf(own.channel, own.access));
        mineIds_.push_back(own.channel.id);
    }
    mineLoaded_ = true;
    const bool mine = !channelId_.empty() && std::find(mineIds_.begin(), mineIds_.end(), channelId_) != mineIds_.end();
    if (mine != isMine_) {
        isMine_ = mine;
        emit channelChanged();
    }
    emit mineChanged();
}

void ChannelsSession::create(const QString& name, const QString& description) {
    if (creating_) return;
    creating_ = true;
    emit busyChanged();
    auto api = api_;
    const std::string n = name.toStdString();
    const std::string d = description.toStdString();
    runInBackground(
        pool_, this, [api, n, d] { return channels::create(*api, n, d); },
        [this](channels::Channel made) {
            creating_ = false;
            emit busyChanged();
            emit created({});
            refreshMine();
            openChannel(qs(made.id));
        },
        [this](const Failure& failure) {
            creating_ = false;
            emit busyChanged();
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            emit created(worded(failure, QStringLiteral("Couldn't create the channel. Try again.")));
        });
}

void ChannelsSession::scheduleCheck() {
    soonTimer_.start();
}

void ChannelsSession::checkFollowed() {
    if (checking_) {
        checkAgain_ = true;
        return;
    }
    checking_ = true;
    struct Job {
        follows::FollowedChannel followed;
        std::optional<std::string> access;
    };
    std::vector<Job> jobs;
    for (const auto& f : followed_) jobs.push_back(Job{f, accessFor(f.secret)});
    struct Found {
        int unread = 0;
        std::map<std::string, std::string> notified;
        /// channel id, name, preview
        std::vector<std::array<std::string, 3>> notifications;
        std::vector<std::pair<std::string, channels::Channel>> channels;
    };
    auto api = api_;
    const auto notified = posts::notifiedFromJson(prefs::text(identity_->userId, kNotifiedKey).toStdString());
    // The channel open on screen right now isn't notified (you're looking at it).
    const std::string current = pageShown_ ? channelId_ : std::string();
    runInBackground(
        pool_, this,
        [api, jobs, notified, current] {
            Found out;
            out.notified = notified;
            for (const auto& job : jobs) {
                try {
                    const auto channel = channels::get(*api, job.followed.id, job.access);
                    out.channels.emplace_back(job.followed.id, channel);
                    const auto outcome = posts::watch(job.followed, channel, out.notified, current);
                    if (outcome.unread) ++out.unread;
                    if (!outcome.notify) continue;
                    std::optional<channels::Post> latest;
                    try {
                        const auto page = channels::posts(*api, job.followed.id, std::nullopt, job.access, 1);
                        if (!page.empty()) latest = page.front();
                    } catch (const std::exception&) {
                    }
                    out.notifications.push_back({job.followed.id, channel.name, posts::postPreview(latest ? &*latest : nullptr)});
                } catch (const std::exception&) {
                    // Gone, private without the link, or offline: try again next time.
                }
            }
            return out;
        },
        [this](Found found) {
            checking_ = false;
            prefs::setText(identity_->userId, kNotifiedKey, qs(posts::notifiedToJson(found.notified)));
            for (auto& [id, channel] : found.channels) {
                if (follows::isFollowing(followed_, id)) fetched_[id] = std::move(channel);
            }
            if (unreadChannels_ != found.unread) {
                unreadChannels_ = found.unread;
                emit unreadChannelsChanged();
            }
            emit followingChanged();
            for (const auto& [id, name, preview] : found.notifications) emit newPostNotification(qs(id), qs(name), qs(preview));
            if (checkAgain_) {
                checkAgain_ = false;
                scheduleCheck();
            }
        },
        [this](const Failure&) {
            checking_ = false;
            if (checkAgain_) {
                checkAgain_ = false;
                scheduleCheck();
            }
        });
}

// --- One channel ---

void ChannelsSession::openChannel(const QString& id, const QString& secret) {
    const int generation = ++channelGeneration_;
    channelId_ = id.toStdString();
    channel_.reset();
    secret_.reset();
    posts_->reset({});
    scheduled_.clear();
    suggestions_.clear();
    mySuggestions_.clear();
    suggestionSent_ = false;
    suggestionError_.clear();
    composerError_.clear();
    postUploads_->setStarter({});
    suggestionUploads_->setStarter({});
    hasOlder_ = false;
    loadingOlder_ = false;
    isMine_ = std::find(mineIds_.begin(), mineIds_.end(), channelId_) != mineIds_.end();
    setMessages({}, {});
    emit channelChanged();
    emit postsChanged();
    emit scheduledChanged();
    emit suggestionsChanged();
    emit composerChanged();
    emit followingChanged();

    struct Loaded {
        /// Your channels, when they could be read (they say whether this one is yours).
        std::optional<std::vector<OwnChannel>> mine;
        channels::Channel channel;
        std::vector<channels::Post> posts;
        std::vector<channels::Post> scheduled;
        std::optional<std::string> secret;
    };
    auto api = api_;
    auto identity = identity_;
    const std::string channelId = channelId_;
    const auto linkSecret = links::secretFromFragment(secret.toStdString());
    std::optional<std::string> followedSecret;
    for (const auto& f : followed_) {
        if (f.id == channelId) followedSecret = f.secret;
    }
    runInBackground(
        pool_, this,
        [api, identity, channelId, linkSecret, followedSecret, admin = isAdmin()] {
            Loaded out;
            std::optional<std::string> ownSecret;
            bool own = false;
            try {
                std::vector<OwnChannel> mine;
                for (auto& c : channels::mine(*api)) {
                    const auto opened = c.isPrivate ? links::openSecret(c.secretSealed, identity->publicKey, identity->privateKey) : std::nullopt;
                    if (c.id == channelId) {
                        ownSecret = opened;
                        own = true;
                    }
                    mine.push_back(OwnChannel{std::move(c), opened ? std::optional<std::string>(links::accessToken(*opened)) : std::nullopt});
                }
                out.mine = std::move(mine);
            } catch (const api::ApiError&) {
                // Not knowing which channels are yours only hides the owner's controls.
            }
            // The link just opened, else the owner's own copy, else the one kept with the follow.
            std::optional<std::string> known = linkSecret ? linkSecret : ownSecret ? ownSecret : followedSecret;
            if (known && !links::secretFromFragment(*known)) known.reset();
            const auto access = known ? std::optional<std::string>(links::accessToken(*known)) : std::nullopt;
            out.channel = channels::get(*api, channelId, access);
            out.posts = channels::posts(*api, channelId, std::nullopt, access);
            if (own || admin) {
                try {
                    out.scheduled = channels::scheduled(*api, channelId);
                } catch (const api::ApiError&) {
                }
            }
            if (out.channel.isPrivate) out.secret = known;
            return out;
        },
        [this, generation](Loaded loaded) {
            if (generation != channelGeneration_) return;
            if (loaded.mine) applyMine(std::move(*loaded.mine));
            isMine_ = std::find(mineIds_.begin(), mineIds_.end(), channelId_) != mineIds_.end();
            secret_ = std::move(loaded.secret);
            channel_ = std::move(loaded.channel);
            hasOlder_ = static_cast<int>(loaded.posts.size()) == channels::kPageSize;
            QStringList seen;
            for (const auto& p : loaded.posts) seen.append(qs(p.id));
            posts_->reset(std::move(loaded.posts));
            scheduled_ = std::move(loaded.scheduled);
            // Uploads go to this channel: the owner's or an admin's for posts, a follower's photos for suggestions.
            auto api = api_;
            const std::string id = channel_->id;
            const auto access = accessFor(secret_);
            if (canManage()) {
                postUploads_->setStarter([api, id](const channels::UploadMeta& meta) { return channels::startMedia(*api, id, meta); });
            } else if (channel_->acceptSuggestions) {
                suggestionUploads_->setStarter(
                    [api, id, access](const channels::UploadMeta& meta) { return channels::startSuggestionMedia(*api, id, meta, access); });
            }
            countViews(seen);
            markSeen();
            loadSuggestions();
            emit channelChanged();
            emit postsChanged();
            emit scheduledChanged();
        },
        [this, generation](const Failure& failure) {
            if (generation != channelGeneration_) return;
            setMessages(failure.status == 404 ? failure.message + QStringLiteral(" A private channel opens only from its private link.")
                                              : QStringLiteral("Couldn't load this channel. Try again."),
                        {});
        });
}

void ChannelsSession::loadSuggestions() {
    if (!channel_ || !channel_->acceptSuggestions) return;
    auto api = api_;
    const std::string id = channel_->id;
    const bool own = canManage();
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, id, own] { return own ? channels::suggestions(*api, id) : channels::mySuggestions(*api, id); },
        [this, generation, own](std::vector<channels::Suggestion> list) {
            if (generation != channelGeneration_) return;
            (own ? suggestions_ : mySuggestions_) = std::move(list);
            emit suggestionsChanged();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
        });
}

void ChannelsSession::closeChannel() {
    ++channelGeneration_;
    channelId_.clear();
    channel_.reset();
    secret_.reset();
    posts_->reset({});
    scheduled_.clear();
    suggestions_.clear();
    mySuggestions_.clear();
    postUploads_->setStarter({});
    suggestionUploads_->setStarter({});
    hasOlder_ = false;
    isMine_ = false;
    setMessages({}, {});
    emit channelChanged();
    emit postsChanged();
    emit scheduledChanged();
    emit suggestionsChanged();
    emit followingChanged();
}

void ChannelsSession::loadOlder() {
    if (!hasOlder_ || loadingOlder_ || posts_->posts().empty()) return;
    loadingOlder_ = true;
    auto api = api_;
    const std::string channelId = channelId_;
    const std::string before = posts_->posts().back().createdAt;
    const auto access = accessFor(secret_);
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, before, access] { return channels::posts(*api, channelId, before, access); },
        [this, generation](std::vector<channels::Post> page) {
            if (generation != channelGeneration_) return;
            loadingOlder_ = false;
            hasOlder_ = static_cast<int>(page.size()) == channels::kPageSize;
            QStringList seen;
            for (const auto& p : page) seen.append(qs(p.id));
            posts_->append(std::move(page));
            countViews(seen);
            emit postsChanged();
        },
        [this, generation](const Failure&) {
            if (generation != channelGeneration_) return;
            loadingOlder_ = false;
            hasOlder_ = false;
            emit postsChanged();
        });
}

void ChannelsSession::toggleFollow() {
    if (!channel_) return;
    if (follows::isFollowing(followed_, channel_->id)) {
        saveFollowed(follows::unfollow(followed_, channel_->id));
    } else {
        fetched_[channel_->id] = channel_;
        saveFollowed(follows::follow(followed_, *channel_, secret_));
    }
}

void ChannelsSession::toggleMuted() {
    if (!isFollowing()) return;
    saveFollowed(follows::setMuted(followed_, channelId_, !isMuted()));
}

void ChannelsSession::markSeen() {
    if (!channel_ || !follows::isFollowing(followed_, channel_->id)) return;
    fetched_[channel_->id] = channel_;
    const auto it = std::find_if(followed_.begin(), followed_.end(), [&](const auto& f) { return f.id == channel_->id; });
    // The private link it was opened with replaces an older one kept with the follow.
    const auto newSecret = channel_->isPrivate && secret_ && secret_ != it->secret ? secret_ : std::nullopt;
    if (it->seenAt != channel_->lastPostAt || it->name != channel_->name || newSecret) {
        saveFollowed(follows::markSeen(followed_, *channel_, newSecret));
    } else {
        emit followingChanged();
    }
}

void ChannelsSession::post(const QString& body) {
    submitPost(body, {}, {});
}

void ChannelsSession::submitPost(const QString& body, const QVariantMap& extra, const QString& publishAt) {
    if (posting_ || !channel_) return;
    channels::NewPost post;
    post.body = body.toStdString();
    const QString type = extra.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("poll") || type == QStringLiteral("quiz")) {
        channels::NewPost::NewPoll poll;
        poll.question = extra.value(QStringLiteral("question")).toString().toStdString();
        poll.options = trimmedList(extra.value(QStringLiteral("options")));
        poll.multiple = type == QStringLiteral("poll") && extra.value(QStringLiteral("multiple")).toBool();
        if (type == QStringLiteral("quiz")) {
            poll.quiz = channels::NewPost::NewPoll::Quiz{extra.value(QStringLiteral("correct")).toInt(),
                                                         extra.value(QStringLiteral("explanation")).toString().toStdString()};
        }
        post.poll = std::move(poll);
    } else if (type == QStringLiteral("checklist")) {
        post.checklist = channels::NewPost::NewChecklist{extra.value(QStringLiteral("title")).toString().toStdString(),
                                                         trimmedList(extra.value(QStringLiteral("items"))),
                                                         extra.value(QStringLiteral("collaborative")).toBool()};
    } else {
        post.media = postUploads_->ready();
    }
    post.publishAt = futureTime(publishAt);
    const bool isScheduled = post.publishAt.has_value();

    posting_ = true;
    composerError_.clear();
    emit busyChanged();
    emit composerChanged();
    auto api = api_;
    const std::string channelId = channel_->id;
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, post] { return channels::post(*api, channelId, post); },
        [this, generation, isScheduled](channels::Post made) {
            posting_ = false;
            emit busyChanged();
            if (generation != channelGeneration_ || !channel_) return;
            postUploads_->clear();
            added(std::move(made), isScheduled);
            emit posted();
        },
        [this](const Failure& failure) {
            posting_ = false;
            emit busyChanged();
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            composerError_ = worded(failure, QStringLiteral("Couldn't post. Try again."));
            emit composerChanged();
        });
}

void ChannelsSession::added(channels::Post post, bool isScheduled) {
    if (!channel_) return;
    if (isScheduled) {
        const QString when = ChannelPostModel::when(post.createdAt);
        scheduled_.push_back(std::move(post));
        std::sort(scheduled_.begin(), scheduled_.end(), [](const auto& a, const auto& b) { return a.createdAt < b.createdAt; });
        emit scheduledChanged();
        setMessages({}, QStringLiteral("Scheduled for %1.").arg(when));
        return;
    }
    channel_->lastPostAt = post.createdAt;
    const QString id = qs(post.id);
    posts_->prepend(std::move(post));
    countViews({id});
    markSeen();
    emit postsChanged();
}

void ChannelsSession::deletePost(const QString& postId) {
    if (busy_ || !channel_) return;
    setBusy(true);
    setMessages({}, notice_);
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = postId.toStdString();
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, id] { channels::removePost(*api, channelId, id); },
        [this, generation, id] {
            setBusy(false);
            if (generation != channelGeneration_) return;
            posts_->remove(id);
            emit postsChanged();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't delete the post.")); });
}

void ChannelsSession::saveDetails(const QString& name, const QString& description) {
    if (busy_ || !channel_) return;
    setBusy(true);
    setMessages({}, notice_);
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string n = name.toStdString();
    const std::string d = description.toStdString();
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, n, d] { return channels::update(*api, channelId, n, d); },
        [this, generation](channels::Channel saved) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            channel_ = std::move(saved);
            markSeen();
            emit channelChanged();
            emit detailsSaved();
            refreshMine();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't save the changes.")); });
}

void ChannelsSession::setPrivate(bool makePrivate) {
    if (busy_ || !channel_ || !isMine_) return;
    setBusy(true);
    setMessages({}, {});
    struct Changed {
        channels::Channel channel;
        std::optional<std::string> secret;
    };
    auto api = api_;
    auto identity = identity_;
    const std::string channelId = channel_->id;
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this,
        [api, identity, channelId, makePrivate] {
            if (!makePrivate) return Changed{channels::setPublic(*api, channelId), std::nullopt};
            // A fresh link: the server gets its token and the secret sealed to this account, never the secret itself.
            std::string fresh = links::newSecret();
            channels::Channel updated =
                channels::setPrivate(*api, channelId, links::accessToken(fresh), links::sealSecret(fresh, identity->publicKey));
            return Changed{std::move(updated), std::move(fresh)};
        },
        [this, generation](Changed changed) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            channel_ = std::move(changed.channel);
            secret_ = std::move(changed.secret);
            if (secret_ && follows::isFollowing(followed_, channel_->id)) {
                fetched_[channel_->id] = channel_;
                saveFollowed(follows::markSeen(followed_, *channel_, secret_));
            }
            emit channelChanged();
            refreshMine();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't change who can read the channel.")); });
}

void ChannelsSession::setAcceptSuggestions(bool accept) {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, accept] { return channels::setAcceptSuggestions(*api, channelId, accept); },
        [this, generation](channels::Channel updated) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            channel_ = std::move(updated);
            emit channelChanged();
            loadSuggestions();
        },
        [this](const Failure& failure) {
            fail(failure, QStringLiteral("Couldn't change that setting."));
            emit channelChanged();
        });
}

void ChannelsSession::setPhoto(const QUrl& imageFile) {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const QString path = imageFile.isLocalFile() ? imageFile.toLocalFile() : imageFile.toString();
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, path] { return channels::setAvatar(*api, channelId, avatarJpegBase64(path)); },
        [this, generation](channels::Channel updated) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            channel_ = std::move(updated);
            if (follows::isFollowing(followed_, channel_->id)) fetched_[channel_->id] = channel_;
            emit channelChanged();
            emit followingChanged();
            refreshMine();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't use that photo.")); });
}

void ChannelsSession::removePhoto() {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId] { channels::removeAvatar(*api, channelId); },
        [this, generation] {
            setBusy(false);
            if (generation != channelGeneration_ || !channel_) return;
            channel_->avatarUrl.reset();
            if (follows::isFollowing(followed_, channel_->id)) fetched_[channel_->id] = channel_;
            emit channelChanged();
            emit followingChanged();
            refreshMine();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't remove the photo.")); });
}

void ChannelsSession::deleteChannel() {
    if (busy_ || !channel_) return;
    setBusy(true);
    setMessages({}, notice_);
    auto api = api_;
    const std::string channelId = channel_->id;
    runInBackground(
        pool_, this, [api, channelId] { channels::remove(*api, channelId); },
        [this, channelId] {
            setBusy(false);
            fetched_.erase(channelId);
            saveFollowed(follows::unfollow(followed_, channelId));
            if (channelId_ == channelId) closeChannel();
            refreshMine();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't delete the channel.")); });
}

void ChannelsSession::report(const QString& reason) {
    if (busy_ || !channel_ || reason.trimmed().isEmpty()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string text = reason.toStdString();
    runInBackground(
        pool_, this, [api, channelId, text] { channels::report(*api, channelId, text); },
        [this] {
            setBusy(false);
            setMessages({}, QStringLiteral("Thanks. The admins will look at it."));
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't send the report.")); });
}

// --- Polls and checklists ---

void ChannelsSession::vote(const QString& postId, const QVariantList& options) {
    if (!channel_) return;
    const auto* existing = posts_->find(postId.toStdString());
    if (!existing || !existing->poll) return;
    std::vector<int> picked;
    for (const auto& option : options) picked.push_back(option.toInt());
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = postId.toStdString();
    const auto access = accessFor(secret_);
    const int generation = channelGeneration_;
    setMessages({}, {});
    runInBackground(
        pool_, this, [api, channelId, id, picked, access] { return channels::vote(*api, channelId, id, picked, access); },
        [this, generation, id](channels::VoteResult result) {
            if (generation != channelGeneration_) return;
            const auto* post = posts_->find(id);
            if (!post) return;
            // What this computer remembers: your choice, and a quiz's right answer as you were shown it.
            std::optional<posts::RememberedVote> remembered;
            if (!result.mine.empty()) {
                remembered = posts::RememberedVote{result.mine, result.poll.quiz ? result.poll.correct : std::nullopt,
                                                   result.poll.quiz ? result.poll.explanation : std::nullopt};
            }
            QSettings settings;
            settings.setValue(QLatin1String(kVotesKey),
                              qs(posts::rememberVote(settings.value(QLatin1String(kVotesKey)).toString().toStdString(), id, remembered,
                                                     result.poll)));
            channels::Post updated = *post;
            updated.poll = std::move(result.poll);
            posts_->replace(updated);
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setMessages(worded(failure, QStringLiteral("Couldn't send your vote. Try again.")), {});
        });
}

void ChannelsSession::closePoll(const QString& postId) {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = postId.toStdString();
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, id] { return channels::closePoll(*api, channelId, id); },
        [this, generation, id](channels::Post closed) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            if (const auto* post = posts_->find(id); post && closed.poll) {
                channels::Post updated = *post;
                updated.poll = closed.poll;
                posts_->replace(updated);
            }
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't close the poll.")); });
}

void ChannelsSession::tick(const QString& postId, int index) {
    if (!channel_) return;
    const auto* post = posts_->find(postId.toStdString());
    if (!post || !post->checklist || index < 0 || index >= static_cast<int>(post->checklist->items.size())) return;
    if (!canManage() && !post->checklist->collaborative) return;
    // Ticked straight away; put back if the server says no.
    const channels::Post before = *post;
    channels::Post next = before;
    auto& item = next.checklist->items[static_cast<std::size_t>(index)];
    item.done = !item.done;
    const bool done = item.done;
    posts_->replace(next);
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = postId.toStdString();
    const auto access = accessFor(secret_);
    const int generation = channelGeneration_;
    setMessages({}, {});
    runInBackground(
        pool_, this, [api, channelId, id, index, done, access] { return channels::setChecklistItem(*api, channelId, id, index, done, access); },
        [this, generation, id](channels::Checklist checklist) {
            if (generation != channelGeneration_) return;
            if (const auto* current = posts_->find(id)) {
                channels::Post updated = *current;
                updated.checklist = std::move(checklist);
                posts_->replace(updated);
            }
        },
        [this, generation, before](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            if (generation == channelGeneration_) posts_->replace(before);
            setMessages(worded(failure, QStringLiteral("Couldn't update the checklist.")), {});
        });
}

// --- Scheduled and suggested posts ---

void ChannelsSession::publishScheduled(const QString& postId) {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = postId.toStdString();
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, id] { return channels::publishNow(*api, channelId, id); },
        [this, generation, id](channels::Post published) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            std::erase_if(scheduled_, [&](const channels::Post& p) { return p.id == id; });
            emit scheduledChanged();
            added(std::move(published), false);
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't publish it.")); });
}

void ChannelsSession::deleteScheduled(const QString& postId) {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = postId.toStdString();
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, id] { channels::removePost(*api, channelId, id); },
        [this, generation, id] {
            setBusy(false);
            if (generation != channelGeneration_) return;
            std::erase_if(scheduled_, [&](const channels::Post& p) { return p.id == id; });
            emit scheduledChanged();
        },
        [this](const Failure& failure) { fail(failure, QStringLiteral("Couldn't delete it.")); });
}

void ChannelsSession::approveSuggestion(const QString& suggestionId, const QString& body, const QString& publishAt) {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = suggestionId.toStdString();
    const std::string text = body.toStdString();
    const auto at = futureTime(publishAt);
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, id, text, at] { return channels::approveSuggestion(*api, channelId, id, text, at); },
        [this, generation, isScheduled = at.has_value()](channels::Post post) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            emit suggestionDecided();
            added(std::move(post), isScheduled);
            loadSuggestions();
        },
        [this](const Failure& failure) {
            fail(failure, QStringLiteral("Something went wrong. Try again."));
            loadSuggestions();
        });
}

void ChannelsSession::rejectSuggestion(const QString& suggestionId) {
    if (busy_ || !channel_ || !canManage()) return;
    setBusy(true);
    setMessages({}, {});
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string id = suggestionId.toStdString();
    runInBackground(
        pool_, this, [api, channelId, id] { channels::rejectSuggestion(*api, channelId, id); },
        [this] {
            setBusy(false);
            emit suggestionDecided();
            loadSuggestions();
        },
        [this](const Failure& failure) {
            fail(failure, QStringLiteral("Something went wrong. Try again."));
            loadSuggestions();
        });
}

void ChannelsSession::sendSuggestion(const QString& body) {
    if (busy_ || !channel_ || canManage()) return;
    setBusy(true);
    suggestionError_.clear();
    emit suggestionsChanged();
    auto api = api_;
    const std::string channelId = channel_->id;
    const std::string text = body.toStdString();
    const auto media = suggestionUploads_->ready();
    const auto access = accessFor(secret_);
    const int generation = channelGeneration_;
    runInBackground(
        pool_, this, [api, channelId, text, media, access] { return channels::suggest(*api, channelId, text, media, access); },
        [this, generation](channels::Suggestion suggestion) {
            setBusy(false);
            if (generation != channelGeneration_) return;
            mySuggestions_.insert(mySuggestions_.begin(), std::move(suggestion));
            suggestionUploads_->clear();
            suggestionSent_ = true;
            emit suggestionsChanged();
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            suggestionError_ = worded(failure, QStringLiteral("Couldn't send your suggestion. Try again."));
            emit suggestionsChanged();
        });
}

void ChannelsSession::resetSuggestionForm() {
    suggestionError_.clear();
    suggestionUploads_->clear();
    emit suggestionsChanged();
}

// --- Media ---

void ChannelsSession::saveMedia(const QString& url, const QUrl& destination) {
    const QString path = destination.isLocalFile() ? destination.toLocalFile() : destination.toString();
    if (path.isEmpty()) return;
    setMessages({}, QStringLiteral("Saving…"));
    auto api = api_;
    const std::string link = url.toStdString();
    // UTF-16 to a path, so names outside the ANSI code page work on Windows too.
    const std::filesystem::path target(path.toStdU16String());
    runInBackground(
        pool_, this,
        [api, link, target] {
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) throw api::UserFacingError("Couldn't write the file there.");
            try {
                channels::downloadMedia(*api, link, out);
                out.close();
                if (!out) throw api::UserFacingError("Couldn't write the file there.");
                file_safety::markFromInternet(target);
            } catch (...) {
                out.close();
                std::error_code ignored;
                std::filesystem::remove(target, ignored);
                throw;
            }
        },
        [this, name = QFileInfo(path).fileName()] { setMessages({}, QStringLiteral("Saved %1.").arg(name)); },
        [this](const Failure& failure) { setMessages(worded(failure, QStringLiteral("Couldn't save it.")), {}); });
}

QString ChannelsSession::formatDuration(double seconds) const {
    return qs(posts::formatDuration(seconds));
}

void ChannelsSession::copyLink() {
    // A private channel's link carries its secret, so it's only offered when this computer has it.
    const auto shareable = link();
    if (!shareable) return;
    QGuiApplication::clipboard()->setText(qs(*shareable));
    setMessages(error_, QStringLiteral("Link copied."));
}

void ChannelsSession::clearMessages() {
    setMessages({}, {});
}

void ChannelsSession::setNotice(const QString& notice) {
    setMessages({}, notice);
}

void ChannelsSession::countViews(const QStringList& postIds) {
    std::vector<std::string> ids;
    for (const auto& id : postIds) ids.push_back(id.toStdString());
    QSettings settings;
    std::vector<std::string> remembered;
    for (const auto& id : settings.value(QLatin1String(kViewedKey)).toStringList()) remembered.push_back(id.toStdString());
    const auto fresh = follows::uncountedViews(remembered, ids);
    if (fresh.empty()) return;
    QStringList saved;
    for (const auto& id : remembered) saved.append(qs(id));
    settings.setValue(QLatin1String(kViewedKey), saved);
    auto api = api_;
    for (std::size_t i = 0; i < fresh.size(); i += channels::kMaxViewsPerReport) {
        const std::vector<std::string> batch(fresh.begin() + static_cast<std::ptrdiff_t>(i),
                                             fresh.begin() + static_cast<std::ptrdiff_t>(std::min(fresh.size(), i + channels::kMaxViewsPerReport)));
        // Without the report, this computer simply doesn't count them: that's all.
        runInBackground(pool_, this, [api, batch] { channels::recordViews(*api, batch); }, [] {}, [](const Failure&) {});
    }
}

bool ChannelsSession::isOwnChannelLink(const QString& text) const {
    return text.trimmed().startsWith(webAppBase_, Qt::CaseInsensitive) && follows::parseChannelLink(text.toStdString()).has_value();
}

void ChannelsSession::openChannelLink(const QString& text) {
    const auto parsed = follows::parseChannelLink(text.toStdString());
    if (!parsed) return;
    openChannel(qs(parsed->id), parsed->secret ? qs(*parsed->secret) : QString());
    emit showRequested();
}

void ChannelsSession::showChannel(const QString& id) {
    if (id.isEmpty()) return;
    openChannel(id.toLower());
    emit showRequested();
}

void ChannelsSession::saveFollowed(model::channels::Followed list) {
    followed_ = std::move(list);
    prefs::setText(identity_->userId, kFollowedKey, qs(follows::toJson(followed_)));
    emit followingChanged();
    // Following, unfollowing, muting or seeing posts changes the badge.
    scheduleCheck();
}

void ChannelsSession::fail(const Failure& failure, const QString& fallback) {
    setBusy(false);
    if (failure.status == 401) {
        emit sessionExpired();
        return;
    }
    setMessages(worded(failure, fallback), {});
}

void ChannelsSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void ChannelsSession::setMessages(const QString& error, const QString& notice) {
    if (error_ == error && notice_ == notice) return;
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

}  // namespace redvault::app
