#include "group_call_controller.hpp"

#include <QAudioDevice>
#include <QCameraDevice>
#include <QCoreApplication>
#include <QDir>
#include <QMediaDevices>
#include <QMetaObject>

#include <atomic>

#include "call_controller.hpp"
#include "chat_session.hpp"
#include "live_session.hpp"
#include "media_io.hpp"
#include "prefs.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/groupcall/ffi.hpp"
#include "redvault/groupcall/session.hpp"
#include "redvault/model/group_call_link.hpp"
#include "video_convert.hpp"

namespace redvault::app {

namespace {

constexpr int kEndedBannerMs = 4000;

/// Where a group call's microphone, speaker and pictures connect to the call.
class GroupMediaPort final : public MediaPort {
public:
    explicit GroupMediaPort(lk::GroupCallSession* session) : session_(session) {}
    void sendAudio(const std::int16_t* pcm) override { session_->sendAudio(pcm); }
    void readAudio(std::int16_t* out, std::size_t samples) override { session_->readAudio(out, samples); }
    void sendVideo(const rtc::VideoFrame& frame, bool screen) override {
        lk::VideoFrame copy;
        copy.width = frame.width;
        copy.height = frame.height;
        copy.data = frame.data;
        session_->sendVideo(screen ? lk::VideoSource::Screen : lk::VideoSource::Camera, copy);
    }

private:
    lk::GroupCallSession* session_;
};

QVariantMap tileMap(const lk::GroupCallSession::Tile& t) {
    return QVariantMap{{"key", QString::fromStdString(t.key)},
                       {"userId", QString::fromStdString(t.userId)},
                       {"handle", QString::fromStdString(t.handle)},
                       {"name", QString::fromStdString(t.name)},
                       {"avatarUrl", QString::fromStdString(t.avatarUrl)},
                       {"isSelf", t.isSelf},
                       {"screen", t.screen},
                       {"videoOn", t.videoOn},
                       {"micMuted", t.micMuted},
                       {"speaking", t.speaking}};
}

QString webBase(const api::ApiClient& api) {
    // The API lives at <web app>/api/.
    QString base = QString::fromStdString(api.baseUrl());
    if (base.endsWith(u'/')) base.chop(1);
    if (base.endsWith(QStringLiteral("/api"))) base.chop(4);
    return base + u'/';
}

}  // namespace

GroupCallController::GroupCallController(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                                         LiveSession* live, ChatSession* chats, CallController* calls, TierProvider tier, QObject* parent)
    : QObject(parent),
      api_(std::move(api)),
      identity_(std::move(identity)),
      live_(live),
      chats_(chats),
      calls_(calls),
      tier_(std::move(tier)) {
    // LiveKit's media library sits next to the program (the installer and the AppImage both put it there or in ../lib).
    const QString dir = QCoreApplication::applicationDirPath();
    lk::Ffi::setSearchDirectories({std::filesystem::path(dir.toStdU16String()), std::filesystem::path(QDir(dir).filePath(QStringLiteral("../lib")).toStdU16String())});
    libraryOk_ = lk::Ffi::libraryInstalled();

    audioThread_.setObjectName(QStringLiteral("group-call-audio"));
    audioThread_.start(QThread::TimeCriticalPriority);
    endedTimer_.setSingleShot(true);
    endedTimer_.setInterval(kEndedBannerMs);
    connect(&endedTimer_, &QTimer::timeout, this, [this] { session_->dismiss(); });

    auto repository = chats_ ? chats_->repository() : nullptr;
    ChatSession* chatSession = chats_.data();

    lk::GroupCallSession::Environment env;
    env.api = api_;
    env.self = identity_;
    env.siteBase = webBase(*api_).toStdString();
    env.group = [this](const std::string& id) -> std::optional<lk::GroupCallSession::GroupInfo> {
        const auto meta = chats_ ? chats_->metaFor(id) : nullptr;
        if (!meta || !meta->isGroup) return std::nullopt;
        lk::GroupCallSession::GroupInfo group;
        group.label = meta->handle;
        for (const auto& m : meta->members) {
            lk::GroupCallSession::Member member;
            member.userId = m.userId;
            member.handle = m.handle;
            member.avatarUrl = m.avatarUrl.value_or("");
            try {
                member.publicKey = crypto::base64Decode(m.publicKeyB64);
            } catch (const std::exception&) {
            }
            group.members.push_back(std::move(member));
        }
        return group;
    };
    env.tier = tier_;
    env.otherCallActive = [this] { return calls_ && calls_->onCall(); };
    env.post = [this](std::function<void()> fn) { QMetaObject::invokeMethod(this, [fn = std::move(fn)] { fn(); }, Qt::QueuedConnection); };
    // The starter's link goes into the group chat as an ordinary end-to-end encrypted message (on the session's worker thread).
    env.announce = [this, repository, chatSession](const std::string& conversationId, const std::string& text) {
        std::shared_ptr<const data::ConversationMeta> meta;
        {
            QMutexLocker lock(&announceMutex_);
            meta = announceMeta_;
        }
        if (!repository || !meta || meta->id != conversationId) return false;
        repository->send(*meta, text);
        if (chatSession) QMetaObject::invokeMethod(chatSession, "refreshNow", Qt::QueuedConnection);
        return true;
    };

    lk::GroupCallSession::Hooks hooks;
    hooks.changed = [this] { onSessionChanged(); };
    hooks.activeChanged = [this] {
        ++activeRevision_;
        emit activeChanged();
    };
    hooks.video = [this, alive = alive_](const std::string& key, const lk::VideoFrame& frame) {
        if (!alive->load()) return;
        const QVideoFrame qf = video::toVideoFrame(frame.width, frame.height, frame.y(), frame.u(), frame.v());
        const QString k = QString::fromStdString(key);
        {
            QMutexLocker lock(&frameMutex_);
            const bool pending = latestFrames_.contains(k);
            latestFrames_.insert(k, qf);
            if (pending) return;  // a newer picture replaces the one still waiting
        }
        QMetaObject::invokeMethod(
            this,
            [this, k] {
                QVideoFrame latest;
                {
                    QMutexLocker lock(&frameMutex_);
                    latest = latestFrames_.take(k);
                }
                showTile(k, latest);
            },
            Qt::QueuedConnection);
    };
    session_ = std::make_unique<lk::GroupCallSession>(std::move(env), std::move(hooks));

    if (live_) {
        connect(live_, &LiveSession::liveEvent, this, [this](const api::live::LiveEvent& event) { session_->onLiveEvent(event); });
        connect(live_, &LiveSession::groupCallsEnabledChanged, this, [this] {
            emit availableChanged();
            if (live_ && live_->groupCallsEnabled()) session_->refreshActive();
        });
        if (live_->groupCallsEnabled()) session_->refreshActive();
    }
    auto* devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioInputsChanged, this, &GroupCallController::devicesChanged);
    connect(devices, &QMediaDevices::audioOutputsChanged, this, &GroupCallController::devicesChanged);
    connect(devices, &QMediaDevices::videoInputsChanged, this, &GroupCallController::devicesChanged);
}

GroupCallController::~GroupCallController() {
    alive_->store(false);
    endedTimer_.stop();
    stopMedia();
    session_.reset();  // leaves the call, so the others get a fresh key straight away
    audioThread_.quit();
    audioThread_.wait(3000);
}

// --- Properties ---

bool GroupCallController::available() const {
    return live_ && live_->groupCallsEnabled() && libraryOk_;
}

bool GroupCallController::present() const {
    return session_->view().present;
}

bool GroupCallController::inCall() const {
    return session_->inCall();
}

QString GroupCallController::phase() const {
    return QString::fromStdString(session_->view().phase);
}

QString GroupCallController::kind() const {
    return QString::fromStdString(session_->view().kind);
}

QString GroupCallController::conversationId() const {
    return QString::fromStdString(session_->view().conversationId);
}

QString GroupCallController::groupLabel() const {
    return QString::fromStdString(session_->view().groupLabel);
}

QString GroupCallController::hostHandle() const {
    return QString::fromStdString(session_->view().hostHandle);
}

bool GroupCallController::muted() const {
    return session_->view().muted;
}

bool GroupCallController::cameraOff() const {
    return session_->view().cameraOff;
}

bool GroupCallController::sharingScreen() const {
    return session_->view().sharingScreen;
}

bool GroupCallController::minimized() const {
    return session_->view().minimized;
}

qint64 GroupCallController::connectedAt() const {
    return session_->view().connectedAt;
}

QStringList GroupCallController::sas() const {
    QStringList out;
    for (const auto& w : session_->view().sas) out.append(QString::fromStdString(w));
    return out;
}

bool GroupCallController::keyReady() const {
    return session_->view().keyReady;
}

QString GroupCallController::notice() const {
    return QString::fromStdString(session_->view().notice);
}

QString GroupCallController::endedMessage() const {
    return QString::fromStdString(session_->view().endedMessage);
}

QVariantList GroupCallController::microphones() const {
    QVariantList out;
    for (const auto& d : QMediaDevices::audioInputs()) out.append(QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}});
    return out;
}

QVariantList GroupCallController::speakers() const {
    QVariantList out;
    for (const auto& d : QMediaDevices::audioOutputs()) out.append(QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}});
    return out;
}

QVariantList GroupCallController::cameras() const {
    QVariantList out;
    for (const auto& d : QMediaDevices::videoInputs()) out.append(QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}});
    return out;
}

QString GroupCallController::microphoneId() const {
    return prefs::text(identity_->userId, "microphoneId");
}

QString GroupCallController::speakerId() const {
    return prefs::text(identity_->userId, "speakerId");
}

QString GroupCallController::cameraId() const {
    return prefs::text(identity_->userId, "cameraId");
}

// --- Starting, joining, answering ---

QString GroupCallController::start(const QString& chatId, const QString& kindIn) {
    if (!live_ || !live_->groupCallsEnabled()) return QStringLiteral("Group calls aren't available on this server.");
    if (!libraryOk_) return QStringLiteral("Group calls need LiveKit's media library, which isn't installed with this copy of the app.");
    const auto meta = chats_ ? chats_->metaFor(chatId.toStdString()) : nullptr;
    if (!meta || !meta->isGroup) return QStringLiteral("Group calls are for group chats.");
    // Joining a call that's already going on needs no plan; starting one needs Premium (the server checks it too).
    if (callIn(chatId).isEmpty() && tier_() != "premium") return QStringLiteral("Only Premium members can start group calls. Anyone in the group can join one.");
    if (calls_ && calls_->onCall()) return QStringLiteral("You're already on a call.");
    endedTimer_.stop();
    {
        QMutexLocker lock(&announceMutex_);
        // Nothing goes into a chat that can't take it: the link is simply not posted, and the call screen says so.
        announceMeta_ = chats_->sendRefusal(meta->id, device_key_refusal::kMessage).isEmpty() ? meta : nullptr;
    }
    const std::string kind = kindIn == QStringLiteral("video") ? "video" : "voice";
    return QString::fromStdString(session_->start(meta->id, kind));
}

QString GroupCallController::joinLink(const QString& text) {
    const auto id = model::group_calls::parseCallLink(text.toStdString());
    if (!id) return QStringLiteral("That isn't a group call link.");
    if (!live_ || !live_->groupCallsEnabled()) return QStringLiteral("Group calls aren't available on this server.");
    if (!libraryOk_) return QStringLiteral("Group calls need LiveKit's media library, which isn't installed with this copy of the app.");
    if (calls_ && calls_->onCall()) return QStringLiteral("You're already on a call.");
    endedTimer_.stop();
    return QString::fromStdString(session_->joinByLink(*id));
}

bool GroupCallController::isOwnCallLink(const QString& text) const {
    return text.startsWith(webBase(*api_), Qt::CaseInsensitive) && model::group_calls::parseCallLink(text.toStdString()).has_value();
}

QVariantMap GroupCallController::callIn(const QString& chatId) const {
    const auto& active = session_->active();
    const auto it = active.find(chatId.toStdString());
    if (it == active.end()) return {};
    return QVariantMap{{"callId", QString::fromStdString(it->second.callId)},
                       {"kind", QString::fromStdString(it->second.kind)},
                       {"participants", it->second.participants}};
}

void GroupCallController::refreshActive() {
    if (live_ && live_->groupCallsEnabled()) session_->refreshActive();
}

void GroupCallController::accept() {
    if (calls_) calls_->stopTone();
    session_->accept();
}

void GroupCallController::decline() {
    if (calls_) calls_->stopTone();
    session_->decline();
}

void GroupCallController::leave() {
    session_->leave();
}

void GroupCallController::toggleMute() {
    session_->toggleMute();
}

void GroupCallController::toggleCamera() {
    session_->toggleCamera();
}

QString GroupCallController::toggleScreenShare() {
    return QString::fromStdString(session_->toggleScreenShare());
}

void GroupCallController::setMinimized(bool minimized) {
    session_->setMinimized(minimized);
}

void GroupCallController::dismiss() {
    endedTimer_.stop();
    session_->dismiss();
}

void GroupCallController::setMicrophone(const QString& id) {
    prefs::setText(identity_->userId, "microphoneId", id);
    emit devicesChanged();
    if (audio_) {
        const QByteArray device = id.toUtf8();
        QMetaObject::invokeMethod(audio_, [a = audio_, device] { a->setMicrophone(device); }, Qt::QueuedConnection);
    }
}

void GroupCallController::setSpeaker(const QString& id) {
    prefs::setText(identity_->userId, "speakerId", id);
    emit devicesChanged();
    if (audio_) {
        const QByteArray device = id.toUtf8();
        QMetaObject::invokeMethod(audio_, [a = audio_, device] { a->setSpeaker(device); }, Qt::QueuedConnection);
    }
}

void GroupCallController::setCamera(const QString& id) {
    prefs::setText(identity_->userId, "cameraId", id);
    emit devicesChanged();
    if (video_ && cameraRunning_) video_->startCamera(id.toUtf8());
}

// --- Pictures ---

void GroupCallController::setTileSink(const QString& key, QObject* sink) {
    sinks_.insert(key, qobject_cast<QVideoSink*>(sink));
    if (key == QStringLiteral("self") || key == QStringLiteral("self:screen")) applySinks();
}

void GroupCallController::clearTileSink(const QString& key, QObject* sink) {
    if (sinks_.value(key).data() != qobject_cast<QVideoSink*>(sink)) return;
    sinks_.remove(key);
    if (key == QStringLiteral("self") || key == QStringLiteral("self:screen")) applySinks();
}

void GroupCallController::applySinks() {
    if (!video_) return;
    video_->setLocalSink(sinks_.value(QStringLiteral("self")).data());
    video_->setLocalScreenSink(sinks_.value(QStringLiteral("self:screen")).data());
}

void GroupCallController::showTile(const QString& key, const QVideoFrame& frame) {
    if (const auto sink = sinks_.value(key)) sink->setVideoFrame(frame);
}

// --- Keeping the Qt side in step with the session ---

void GroupCallController::onSessionChanged() {
    const auto& v = session_->view();
    tiles_.clear();
    for (const auto& t : v.tiles) tiles_.append(tileMap(t));

    const QString phase = QString::fromStdString(v.phase);
    if (phase != lastPhase_) {
        if (phase == QStringLiteral("incoming")) {
            if (calls_ && calls_->shouldRing(QString::fromStdString(v.conversationId))) calls_->playTone(true);
            emit ringing(QString::fromStdString(v.hostHandle) + QStringLiteral(" (") + QString::fromStdString(v.groupLabel) + u')', QString::fromStdString(v.kind));
        } else if (lastPhase_ == QStringLiteral("incoming") && calls_) {
            calls_->stopTone();
        }
        if (phase == QStringLiteral("ended")) endedTimer_.start();
        else endedTimer_.stop();
        lastPhase_ = phase;
    }
    reconcileMedia();
    emit viewChanged();
}

void GroupCallController::reconcileMedia() {
    const auto& v = session_->view();
    const bool up = v.present && (v.phase == "active" || v.phase == "reconnecting");
    if (up && !audio_) startMedia();
    if (!up && audio_) stopMedia();
    if (!audio_) return;

    audio_->setMuted(v.muted);

    const bool wantCamera = v.kind == "video" && v.published && !v.cameraOff;
    if (wantCamera && !cameraRunning_) {
        if (QMediaDevices::videoInputs().isEmpty()) {
            session_->cameraFailed();
        } else {
            video_->startCamera(cameraId().toUtf8());
            video_->setCameraEnabled(true);
            cameraRunning_ = true;
        }
    } else if (!wantCamera && cameraRunning_) {
        video_->setCameraEnabled(false);
        video_->stopCamera();
        cameraRunning_ = false;
    }

    if (v.sharingScreen && !screenRunning_) {
        if (video_->startScreen()) {
            screenRunning_ = true;
        } else {
            session_->screenStopped();
        }
    } else if (!v.sharingScreen && screenRunning_) {
        video_->stopScreen();
        screenRunning_ = false;
    }
}

void GroupCallController::startMedia() {
    auto port = std::make_shared<GroupMediaPort>(session_.get());
    audio_ = new AudioIo(port);
    audio_->moveToThread(&audioThread_);
    audio_->setMuted(session_->view().muted);
    const QByteArray mic = microphoneId().toUtf8();
    const QByteArray speaker = speakerId().toUtf8();
    QMetaObject::invokeMethod(audio_, [a = audio_, mic, speaker] { a->start(mic, speaker); }, Qt::QueuedConnection);

    video_ = new VideoIo(port, this, true);
    connect(video_, &VideoIo::screenStopped, this, [this] { session_->screenStopped(); });
    applySinks();
}

void GroupCallController::stopMedia() {
    if (audio_) {
        QMetaObject::invokeMethod(audio_, [a = audio_] { delete a; }, Qt::BlockingQueuedConnection);
        audio_ = nullptr;
    }
    if (video_) {
        delete video_;
        video_ = nullptr;
    }
    cameraRunning_ = false;
    screenRunning_ = false;
    QMutexLocker lock(&frameMutex_);
    latestFrames_.clear();
}

}  // namespace redvault::app
