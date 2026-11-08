// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "call_controller.hpp"

#include <QAudioDevice>
#include <QCameraDevice>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QVideoSink>
#include <QtEndian>

#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

#include "async.hpp"
#include "chat_session.hpp"
#include "group_call_controller.hpp"
#include "live_session.hpp"
#include "media_io.hpp"
#include "prefs.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/model/group_call_link.hpp"
#include "redvault/rtc/call_media.hpp"
#include "video_convert.hpp"

namespace redvault::app {

namespace calls = crypto::calls;
namespace live = api::live;

namespace {

/// A dropped connection gets this long to come back before the call ends.
constexpr int kReconnectGraceMs = 15000;
constexpr int kEndedBannerMs = 4000;
constexpr int kMaxHistory = 500;

int tierRank(const std::string& tier) {
    if (tier == "premium") return 2;
    if (tier == "pro") return 1;
    return 0;
}

QString endedText(const std::string& reason, bool outgoing) {
    if (reason == "declined") return QStringLiteral("Call declined");
    if (reason == "missed") return outgoing ? QStringLiteral("No answer") : QStringLiteral("Missed call");
    if (reason == "busy") return outgoing ? QStringLiteral("They're on another call") : QStringLiteral("Call ended");
    if (reason == "failed") return QStringLiteral("Call dropped");
    return QStringLiteral("Call ended");
}

std::vector<rtc::IceServer> toRtc(const std::vector<live::IceServer>& servers) {
    std::vector<rtc::IceServer> out;
    for (const auto& s : servers) {
        for (const auto& url : s.urls) out.push_back(rtc::IceServer{url, s.username, s.credential});
    }
    return out;
}

/// A mono 16-bit WAV of beeps (like the web app's synthesized ring and ringback tones).
QByteArray toneWav(double hz, double periodSec, std::initializer_list<double> beeps) {
    constexpr int rate = 24000;
    const int samples = static_cast<int>(periodSec * rate);
    QByteArray pcm(samples * 2, '\0');
    auto* out = reinterpret_cast<std::int16_t*>(pcm.data());
    for (double start : beeps) {
        const int from = static_cast<int>(start * rate);
        for (int i = 0; i < static_cast<int>(0.3 * rate) && from + i < samples; ++i) {
            const double t = static_cast<double>(i) / rate;
            const double envelope = t < 0.02 ? t / 0.02 : std::max(0.0, (0.3 - t) / 0.28);
            out[from + i] = static_cast<std::int16_t>(0.12 * envelope * 32767 * std::sin(2 * std::numbers::pi * hz * t));
        }
    }
    QByteArray wav;
    auto u32 = [&wav](std::uint32_t v) {
        char b[4];
        qToLittleEndian(v, b);
        wav.append(b, 4);
    };
    auto u16 = [&wav](std::uint16_t v) {
        char b[2];
        qToLittleEndian(v, b);
        wav.append(b, 2);
    };
    wav.append("RIFF");
    u32(static_cast<std::uint32_t>(36 + pcm.size()));
    wav.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    wav.append("data");
    u32(static_cast<std::uint32_t>(pcm.size()));
    wav.append(pcm);
    return wav;
}

QUrl toneFile(bool incoming) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = dir + (incoming ? QStringLiteral("/ring-in.wav") : QStringLiteral("/ring-out.wav"));
    if (!QFile::exists(path)) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(incoming ? toneWav(880, 2.0, {0, 0.35}) : toneWav(440, 4.0, {0, 0.5}));
            file.commit();
        }
    }
    return QUrl::fromLocalFile(path);
}

QString historyPath(const std::string& userId) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/call-history-") + QString::fromStdString(userId) + QStringLiteral(".json");
}

/// Gathering found no relay (or the engine failed): worded for the call screen.
template <typename Work>
std::string withRelay(Work work) {
    try {
        return work();
    } catch (const rtc::RtcError& e) {
        if (std::string_view(e.what()) == "relay_unreachable") throw api::UserFacingError("Couldn't reach the call relay. Try again in a minute.");
        throw api::UserFacingError("Couldn't set up the call. Try again.");
    }
}

QString describeError(const Failure& failure, const QString& fallback) {
    return failure.status > 0 ? failure.message : fallback;
}

}  // namespace

struct CallController::Session {
    std::string callId;
    std::string conversationId;
    std::string linkId;
    std::string linkName;
    std::shared_ptr<crypto::SecretBytes> linkSecret;  // call links: seals the offer and answer
    std::string peerUserId;
    std::string peerHandle;
    std::string peerAvatarUrl;
    crypto::Bytes peerPublicKey;  // contacts: seals the offer and answer
    std::string kind;
    std::string direction;
    std::vector<live::IceServer> iceServers;
    std::string offerSdp;
    std::shared_ptr<rtc::CallMedia> media;
    bool closed = false;
    qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
    qint64 connectedAt = 0;

    std::string sealedConversationId() const { return linkId.empty() ? conversationId : "link:" + linkId; }
};

CallController::CallController(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity,
                               LiveSession* live, ChatSession* chats, TierProvider tier, QObject* parent)
    : QObject(parent), api_(std::move(api)), identity_(std::move(identity)), live_(live), chats_(chats), tier_(std::move(tier)) {
    pool_.setMaxThreadCount(4);
    audioThread_.setObjectName(QStringLiteral("call-audio"));
    audioThread_.start(QThread::TimeCriticalPriority);
    tone_.setLoopCount(QSoundEffect::Infinite);
    tone_.setVolume(0.8f);
    disconnectTimer_.setSingleShot(true);
    disconnectTimer_.setInterval(kReconnectGraceMs);
    connect(&disconnectTimer_, &QTimer::timeout, this, [this] { fail(QStringLiteral("Call dropped")); });
    endedTimer_.setSingleShot(true);
    connect(&endedTimer_, &QTimer::timeout, this, [this] {
        if (!session_) resetView();
    });
    if (live_) {
        connect(live_, &LiveSession::liveEvent, this, &CallController::onLiveEvent);
        connect(live_, &LiveSession::callsEnabledChanged, this, &CallController::callsAvailableChanged);
    }
    auto* devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioInputsChanged, this, &CallController::devicesChanged);
    connect(devices, &QMediaDevices::audioOutputsChanged, this, &CallController::devicesChanged);
    connect(devices, &QMediaDevices::videoInputsChanged, this, &CallController::devicesChanged);
    loadHistory();
    refreshLinks();
}

CallController::~CallController() {
    // Signing out or quitting ends any call, so the other side isn't left talking to no one.
    if (session_ && !session_->callId.empty()) {
        std::thread([api = api_, id = session_->callId] {
            try {
                live::hangUp(*api, id);
            } catch (...) {
            }
        }).detach();
    }
    if (session_) {
        session_->closed = true;
        stopTone();
        if (audio_) {
            QMetaObject::invokeMethod(audio_, [a = audio_] { delete a; }, Qt::BlockingQueuedConnection);
            audio_ = nullptr;
        }
        delete video_;
        video_ = nullptr;
        if (media_) media_->close();
    }
    audioThread_.quit();
    audioThread_.wait(3000);
    pool_.waitForDone(10000);
}

// --- Properties ---

bool CallController::callsAvailable() const {
    return live_ && live_->callsEnabled();
}

QVariantList CallController::microphones() const {
    QVariantList out;
    for (const auto& d : QMediaDevices::audioInputs()) out.append(QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}});
    return out;
}

QVariantList CallController::speakers() const {
    QVariantList out;
    for (const auto& d : QMediaDevices::audioOutputs()) out.append(QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}});
    return out;
}

QVariantList CallController::cameras() const {
    QVariantList out;
    for (const auto& d : QMediaDevices::videoInputs()) out.append(QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}});
    return out;
}

QString CallController::microphoneId() const {
    return prefs::text(identity_->userId, "microphoneId");
}

QString CallController::speakerId() const {
    return prefs::text(identity_->userId, "speakerId");
}

QString CallController::cameraId() const {
    return prefs::text(identity_->userId, "cameraId");
}

bool CallController::echoCancellation() const {
    return AudioIo::echoCancellationAvailable();
}

bool CallController::ringtone() const {
    return prefs::ringtone(identity_->userId);
}

void CallController::setRingtone(bool on) {
    if (on == ringtone()) return;
    prefs::setFlag(identity_->userId, "ringtone", on);
    emit ringtoneChanged();
}

QString CallController::webAppBase() const {
    // The API lives at <web app>/api/.
    QString base = QString::fromStdString(api_->baseUrl());
    if (base.endsWith(u'/')) base.chop(1);
    if (base.endsWith(QStringLiteral("/api"))) base.chop(4);
    return base + u'/';
}

void CallController::copyText(const QString& text) {
    QGuiApplication::clipboard()->setText(text);
}

// --- Starting calls ---

QString CallController::startCall(const QString& chatId, const QString& kindIn) {
    const std::string kind = kindIn == QStringLiteral("video") ? "video" : "voice";
    if (session_ || (groupCalls_ && groupCalls_->inCall())) return QStringLiteral("You're already on a call.");
    if (!callsAvailable()) return QStringLiteral("Calls aren't available on this server.");
    // No plan check: one-to-one calls with a contact are free on every plan.
    const auto meta = chats_ ? chats_->metaFor(chatId.toStdString()) : nullptr;
    if (!meta || meta->isGroup || meta->otherUserId.empty() || meta->otherPublicKey.empty()) {
        return QStringLiteral("Calls are one-to-one, with a contact.");
    }
    endedTimer_.stop();
    auto session = std::make_shared<Session>();
    session->conversationId = meta->id;
    session->peerUserId = meta->otherUserId;
    session->peerHandle = meta->handle;
    session->peerAvatarUrl = meta->avatarUrl.value_or("");
    session->peerPublicKey = meta->otherPublicKey;
    session->kind = kind;
    session->direction = "outgoing";
    ring(session);  // shows the call screen (outgoing: no ringtone until the offer is out)
    phase_ = QStringLiteral("outgoing");
    emit callChanged();
    auto api = api_;
    const std::string conversationId = meta->id;
    runOutgoing(session, [api, conversationId, kind] { return live::startCall(*api, conversationId, kind); });
    return {};
}

QString CallController::joinLink(const QString& text) {
    // A group call link is the group call controller's to judge: pasting the link of a call that is ringing answers it.
    if (model::group_calls::parseCallLink(text.toStdString())) {
        return groupCalls_ ? groupCalls_->joinLink(text) : QStringLiteral("Group calls aren't available.");
    }
    if (session_ || (groupCalls_ && groupCalls_->inCall())) return QStringLiteral("You're already on a call.");
    const QUrl url(text.trimmed());
    const QString id = QUrlQuery(url).queryItemValue(QStringLiteral("join"));
    auto secret = calls::linkSecretFromFragment(url.fragment().toStdString());
    if (id.isEmpty() || !secret) return QStringLiteral("That isn't a call link. Paste the whole link, including the part after #.");
    auto secretShared = std::make_shared<crypto::SecretBytes>(std::move(*secret));

    struct Info {
        live::CallLinkInfo link;
        std::string name;
        api::UserResult owner;
    };
    auto api = api_;
    const std::string linkId = id.toStdString();
    runInBackground(
        pool_, this,
        [api, linkId, secretShared] {
            Info info;
            info.link = live::callLinkInfo(*api, linkId);
            try {
                info.name = calls::openLinkName({info.link.nameCiphertext, info.link.nameNonce}, secretShared->view());
            } catch (const std::exception&) {
                throw api::UserFacingError("That link's secret doesn't match. Ask for the link again.");
            }
            info.owner = api->getUser(info.link.ownerId);
            return info;
        },
        [this, secretShared](Info info) {
            if (session_) return;
            if (info.link.ownerId == identity_->userId) {
                linkError_ = QStringLiteral("That's your own link. Share it; you'll ring when someone opens it.");
                emit linksChanged();
                return;
            }
            endedTimer_.stop();
            auto session = std::make_shared<Session>();
            session->linkId = info.link.id;
            session->linkName = info.name;
            session->linkSecret = secretShared;
            session->peerUserId = info.link.ownerId;
            session->peerHandle = info.owner.username;
            session->peerAvatarUrl = info.owner.avatarUrl.value_or("");
            session->kind = info.link.kind == "video" ? "video" : "voice";
            session->direction = "outgoing";
            ring(session);
            phase_ = QStringLiteral("outgoing");
            emit callChanged();
            auto api = api_;
            const std::string id = info.link.id;
            runOutgoing(session, [api, id] { return live::joinCallLink(*api, id); });
        },
        [this](const Failure& failure) {
            linkError_ = failure.status == 404 ? QStringLiteral("That call link was deleted or never existed.")
                                               : describeError(failure, QStringLiteral("Couldn't open that call link."));
            emit linksChanged();
        });
    linkError_.clear();
    emit linksChanged();
    return {};
}

void CallController::runOutgoing(std::shared_ptr<Session> session, std::function<live::StartedCall()> reserve) {
    struct Ready {
        live::StartedCall started;
        std::shared_ptr<rtc::CallMedia> media;
        std::string sdp;
    };
    QPointer<CallController> self(this);
    std::weak_ptr<Session> weak = session;
    auto api = api_;
    auto started = std::make_shared<std::string>();  // the reserved call, to hang up if the setup fails after it
    runInBackground(
        pool_, this,
        [reserve, self, weak, api, started] {
            Ready ready;
            ready.started = reserve();
            *started = ready.started.callId;
            rtc::Config config;
            config.servers = toRtc(ready.started.iceServers);
            rtc::Callbacks callbacks;
            callbacks.onState = [self, weak](rtc::ConnectionState state) {
                QMetaObject::invokeMethod(
                    qApp,
                    [self, weak, state] {
                        auto s = weak.lock();
                        if (self && s && self->session_ == s) self->onEngineState(static_cast<int>(state));
                    },
                    Qt::QueuedConnection);
            };
            callbacks.onVideoFrame = [self, weak](const rtc::VideoFrame& frame) {
                QVideoFrame picture = video::toVideoFrame(frame);
                QMetaObject::invokeMethod(
                    qApp,
                    [self, weak, picture] {
                        auto s = weak.lock();
                        if (self && s && self->session_ == s && self->video_) self->video_->showRemoteFrame(picture);
                    },
                    Qt::QueuedConnection);
            };
            ready.media = std::make_shared<rtc::CallMedia>(config, callbacks);
            ready.sdp = withRelay([&] { return ready.media->createOffer(); });
            return ready;
        },
        [this, session, api](Ready ready) {
            if (session->closed || session_ != session) {
                ready.media->close();
                std::thread([api, id = ready.started.callId] {
                    try {
                        live::hangUp(*api, id);
                    } catch (...) {
                    }
                }).detach();
                return;
            }
            session->callId = ready.started.callId;
            session->iceServers = ready.started.iceServers;
            session->media = ready.media;
            if (ready.started.kind) session->kind = *ready.started.kind == "video" ? "video" : "voice";
            kind_ = QString::fromStdString(session->kind);
            startMedia(session->kind == "video");

            calls::CallSignal offer{"offer",        session->callId, session->sealedConversationId(), identity_->userId,
                                    session->peerUserId, session->kind, ready.sdp, calls::nowMs()};
            const calls::Sealed sealed = session->linkSecret ? calls::sealWithLink(offer, session->linkSecret->view())
                                                             : calls::seal(offer, session->peerPublicKey, identity_->privateKey.view());
            const std::string callId = session->callId;
            runInBackground(
                pool_, this, [api, callId, sealed] { live::sendOffer(*api, callId, {sealed.ciphertext, sealed.nonce}); },
                [this, session] {
                    if (session_ == session && phase_ == QStringLiteral("outgoing")) playTone(false);
                },
                [this, session](const Failure& failure) {
                    if (session_ == session) fail(describeError(failure, QStringLiteral("Couldn't start the call. Try again.")));
                });
        },
        [this, session, started](const Failure& failure) {
            if (session_ != session) return;
            session->callId = *started;  // so fail() hangs up a call that was reserved
            fail(failure.message);
        });
}

// --- Media ---

void CallController::startMedia(bool withVideo) {
    media_ = session_->media;
    audio_ = new AudioIo(makeCallMediaPort(media_));
    audio_->moveToThread(&audioThread_);
    audio_->setMuted(muted_);
    const QByteArray mic = microphoneId().toUtf8();
    const QByteArray speaker = speakerId().toUtf8();
    QMetaObject::invokeMethod(audio_, [a = audio_, mic, speaker] { a->start(mic, speaker); }, Qt::QueuedConnection);

    video_ = new VideoIo(makeCallMediaPort(media_), this);
    video_->setLocalSink(qobject_cast<QVideoSink*>(localSinkObject_.data()));
    video_->setRemoteSink(qobject_cast<QVideoSink*>(remoteSinkObject_.data()));
    connect(video_, &VideoIo::screenStopped, this, [this] {
        if (sharingScreen_) toggleScreenShare();
    });
    cameraOff_ = true;
    if (withVideo && !QMediaDevices::videoInputs().isEmpty()) {
        video_->startCamera(cameraId().toUtf8());
        cameraOff_ = false;
    }
    emit callChanged();
}

void CallController::setVideoSinks(QObject* localSink, QObject* remoteSink) {
    localSinkObject_ = localSink;
    remoteSinkObject_ = remoteSink;
    if (video_) {
        video_->setLocalSink(qobject_cast<QVideoSink*>(localSink));
        video_->setRemoteSink(qobject_cast<QVideoSink*>(remoteSink));
    }
}

void CallController::onEngineState(int value) {
    const auto state = static_cast<rtc::ConnectionState>(value);
    if (!session_ || !media_) return;
    if (state == rtc::ConnectionState::Connected) {
        stopTone();
        disconnectTimer_.stop();
        if (!session_->connectedAt) session_->connectedAt = QDateTime::currentMSecsSinceEpoch();
        connectedAt_ = session_->connectedAt;
        phase_ = QStringLiteral("active");
        const std::string mine = calls::sdpFingerprint(media_->localSdp());
        const std::string theirs = calls::sdpFingerprint(media_->remoteSdp());
        if (!mine.empty() && !theirs.empty()) {
            sas_.clear();
            for (const auto& w : calls::sasWords(mine, theirs)) sas_.append(QString::fromStdString(w));
        }
        emit callChanged();
    } else if (state == rtc::ConnectionState::Disconnected) {
        phase_ = QStringLiteral("reconnecting");
        emit callChanged();
        if (!disconnectTimer_.isActive()) disconnectTimer_.start();
    } else if (state == rtc::ConnectionState::Failed) {
        fail(QStringLiteral("Call dropped"));
    }
}

// --- Answering and ending ---

void CallController::accept(bool audioOnly) {
    auto session = session_;
    if (!session || session->direction != "incoming" || session->callId.empty() || session->offerSdp.empty() || session->media) return;
    if (phase_ != QStringLiteral("incoming")) return;
    stopTone();
    phase_ = QStringLiteral("connecting");
    minimized_ = false;
    emit callChanged();

    QPointer<CallController> self(this);
    std::weak_ptr<Session> weak = session;
    const auto servers = toRtc(session->iceServers);
    const std::string offerSdp = session->offerSdp;
    struct Ready {
        std::shared_ptr<rtc::CallMedia> media;
        std::string sdp;
    };
    runInBackground(
        pool_, this,
        [self, weak, servers, offerSdp] {
            rtc::Config config;
            config.servers = servers;
            rtc::Callbacks callbacks;
            callbacks.onState = [self, weak](rtc::ConnectionState state) {
                QMetaObject::invokeMethod(
                    qApp,
                    [self, weak, state] {
                        auto s = weak.lock();
                        if (self && s && self->session_ == s) self->onEngineState(static_cast<int>(state));
                    },
                    Qt::QueuedConnection);
            };
            callbacks.onVideoFrame = [self, weak](const rtc::VideoFrame& frame) {
                QVideoFrame picture = video::toVideoFrame(frame);
                QMetaObject::invokeMethod(
                    qApp,
                    [self, weak, picture] {
                        auto s = weak.lock();
                        if (self && s && self->session_ == s && self->video_) self->video_->showRemoteFrame(picture);
                    },
                    Qt::QueuedConnection);
            };
            Ready ready;
            ready.media = std::make_shared<rtc::CallMedia>(config, callbacks);
            ready.sdp = withRelay([&] { return ready.media->acceptOffer(offerSdp); });
            return ready;
        },
        [this, session, audioOnly](Ready ready) {
            if (session->closed || session_ != session) {
                ready.media->close();
                return;
            }
            session->media = ready.media;
            startMedia(session->kind == "video" && !audioOnly);
            calls::CallSignal answer{"answer",           session->callId, session->sealedConversationId(), identity_->userId,
                                     session->peerUserId, session->kind,   ready.sdp,                        calls::nowMs()};
            const calls::Sealed sealed = session->linkSecret ? calls::sealWithLink(answer, session->linkSecret->view())
                                                             : calls::seal(answer, session->peerPublicKey, identity_->privateKey.view());
            auto api = api_;
            const std::string callId = session->callId;
            runInBackground(
                pool_, this, [api, callId, sealed] { live::sendAnswer(*api, callId, {sealed.ciphertext, sealed.nonce}); }, [] {},
                [this, session](const Failure& failure) {
                    if (session_ == session) fail(describeError(failure, QStringLiteral("Couldn't answer the call.")));
                });
        },
        [this, session](const Failure& failure) {
            if (session_ != session) return;
            fail(failure.message);
        });
}

void CallController::decline() {
    if (!session_ || session_->direction != "incoming") return;
    if (!session_->callId.empty()) {
        auto api = api_;
        const std::string id = session_->callId;
        runInBackground(pool_, this, [api, id] { live::declineCall(*api, id); }, [] {}, [](const Failure&) {});
    }
    finish({}, QStringLiteral("declined"));
}

void CallController::hangUp() {
    if (!session_) return;
    if (!session_->callId.empty()) {
        auto api = api_;
        const std::string id = session_->callId;
        runInBackground(pool_, this, [api, id] { live::hangUp(*api, id); }, [] {}, [](const Failure&) {});
    }
    finish(QStringLiteral("Call ended"));
}

void CallController::fail(const QString& message) {
    if (!session_) return;
    if (!session_->callId.empty()) {
        auto api = api_;
        const std::string id = session_->callId;
        runInBackground(pool_, this, [api, id] { live::hangUp(*api, id); }, [] {}, [](const Failure&) {});
    }
    finish(message, session_->connectedAt ? QStringLiteral("answered") : QStringLiteral("failed"));
}

void CallController::finish(const QString& message, const QString& outcomeIn) {
    auto session = session_;
    session_.reset();
    stopTone();
    disconnectTimer_.stop();
    if (session) {
        session->closed = true;
        const QString outcome = !outcomeIn.isEmpty()           ? outcomeIn
                                : session->connectedAt          ? QStringLiteral("answered")
                                : session->direction == "incoming" ? QStringLiteral("missed")
                                                                   : QStringLiteral("unanswered");
        remember(*session, outcome);
    }
    if (audio_) {
        QMetaObject::invokeMethod(audio_, [a = audio_] { delete a; }, Qt::BlockingQueuedConnection);
        audio_ = nullptr;
    }
    delete video_;
    video_ = nullptr;
    if (auto media = std::move(media_)) {
        runInBackground(pool_, this, [media] { media->close(); }, [] {}, [](const Failure&) {});
    }
    if (auto* local = qobject_cast<QVideoSink*>(localSinkObject_.data())) local->setVideoFrame(QVideoFrame());
    if (auto* remote = qobject_cast<QVideoSink*>(remoteSinkObject_.data())) remote->setVideoFrame(QVideoFrame());

    endedTimer_.stop();
    if (!message.isEmpty() && !phase_.isEmpty()) {
        phase_ = QStringLiteral("ended");
        endedMessage_ = message;
        sharingScreen_ = false;
        remoteSharing_ = false;
        minimized_ = false;
        emit callChanged();
        endedTimer_.start(kEndedBannerMs);
    } else {
        resetView();
    }
}

void CallController::resetView() {
    phase_.clear();
    kind_.clear();
    direction_.clear();
    peerUserId_.clear();
    peerHandle_.clear();
    peerAvatarUrl_.clear();
    conversationId_.clear();
    linkId_.clear();
    linkName_.clear();
    muted_ = false;
    cameraOff_ = true;
    sharingScreen_ = false;
    remoteSharing_ = false;
    connectedAt_ = 0;
    sas_.clear();
    minimized_ = false;
    chatOpen_ = false;
    endedMessage_.clear();
    emit callChanged();
}

void CallController::dismiss() {
    endedTimer_.stop();
    if (!session_) resetView();
}

// --- In-call controls ---

void CallController::toggleMute() {
    if (!session_) return;
    muted_ = !muted_;
    if (audio_) audio_->setMuted(muted_);
    emit callChanged();
}

void CallController::toggleCamera() {
    if (!video_ || kind_ != QStringLiteral("video")) return;
    if (cameraOff_) {
        if (QMediaDevices::videoInputs().isEmpty()) return;
        if (!video_->cameraActive()) video_->startCamera(cameraId().toUtf8());
        video_->setCameraEnabled(true);
        cameraOff_ = false;
    } else {
        video_->setCameraEnabled(false);
        cameraOff_ = true;
    }
    emit callChanged();
}

QString CallController::toggleScreenShare() {
    if (!session_ || !video_ || session_->callId.empty()) return {};
    auto api = api_;
    const std::string callId = session_->callId;
    if (sharingScreen_) {
        video_->stopScreen();
        sharingScreen_ = false;
        emit callChanged();
        runInBackground(pool_, this, [api, callId] { live::setScreenShare(*api, callId, false); }, [] {}, [](const Failure&) {});
        return {};
    }
    if (tierRank(tier_()) < 2) return QStringLiteral("Screen sharing is part of Premium.");
    auto session = session_;
    // The server checks the plan and tells the other side to show it.
    runInBackground(
        pool_, this, [api, callId] { live::setScreenShare(*api, callId, true); },
        [this, session, api, callId] {
            if (session_ != session || !video_) return;
            if (!video_->startScreen()) {
                endedMessage_ = QStringLiteral("Couldn't capture your screen.");
                emit callChanged();
                runInBackground(pool_, this, [api, callId] { live::setScreenShare(*api, callId, false); }, [] {}, [](const Failure&) {});
                return;
            }
            sharingScreen_ = true;
            endedMessage_.clear();
            emit callChanged();
        },
        [this, session](const Failure& failure) {
            if (session_ != session) return;
            endedMessage_ = describeError(failure, QStringLiteral("Couldn't share your screen."));
            emit callChanged();
        });
    return {};
}

void CallController::setMinimized(bool minimized) {
    if (minimized_ == minimized) return;
    minimized_ = minimized;
    emit callChanged();
}

void CallController::toggleChat() {
    chatOpen_ = !chatOpen_;
    emit callChanged();
}

void CallController::setMicrophone(const QString& id) {
    prefs::setText(identity_->userId, "microphoneId", id);
    emit devicesChanged();
    if (audio_) {
        const QByteArray device = id.toUtf8();
        QMetaObject::invokeMethod(audio_, [a = audio_, device] { a->setMicrophone(device); }, Qt::QueuedConnection);
    }
}

void CallController::setSpeaker(const QString& id) {
    prefs::setText(identity_->userId, "speakerId", id);
    emit devicesChanged();
    if (audio_) {
        const QByteArray device = id.toUtf8();
        QMetaObject::invokeMethod(audio_, [a = audio_, device] { a->setSpeaker(device); }, Qt::QueuedConnection);
    }
}

void CallController::setCamera(const QString& id) {
    prefs::setText(identity_->userId, "cameraId", id);
    emit devicesChanged();
    if (video_ && !cameraOff_) video_->startCamera(id.toUtf8());
}

void CallController::refreshDevices() {
    emit devicesChanged();
}

// --- Ringing ---

void CallController::ring(std::shared_ptr<Session> session) {
    session_ = std::move(session);
    const Session& s = *session_;
    phase_ = s.direction == "incoming" ? QStringLiteral("incoming") : QStringLiteral("outgoing");
    kind_ = QString::fromStdString(s.kind);
    direction_ = QString::fromStdString(s.direction);
    peerUserId_ = QString::fromStdString(s.peerUserId);
    peerHandle_ = QString::fromStdString(s.peerHandle);
    peerAvatarUrl_ = QString::fromStdString(s.peerAvatarUrl);
    conversationId_ = QString::fromStdString(s.conversationId);
    linkId_ = QString::fromStdString(s.linkId);
    linkName_ = QString::fromStdString(s.linkName);
    muted_ = false;
    cameraOff_ = s.kind != "video";
    sharingScreen_ = false;
    remoteSharing_ = false;
    connectedAt_ = 0;
    sas_.clear();
    minimized_ = false;
    chatOpen_ = false;
    endedMessage_.clear();
    emit callChanged();
    if (s.direction == "incoming") {
        const bool silent = !ringtone() || prefs::muted(identity_->userId).contains(peerUserId_);
        if (!silent) playTone(true);
        emit ringing(peerHandle_, kind_);
    }
}

void CallController::playTone(bool incoming) {
    tone_.stop();
    tone_.setSource(toneFile(incoming));
    tone_.play();
}

void CallController::stopTone() {
    tone_.stop();
}

bool CallController::shouldRing(const QString& chatId) const {
    return ringtone() && !prefs::muted(identity_->userId).contains(chatId);
}

void CallController::onLiveEvent(const live::LiveEvent& event) {
    if (event.type == "typing") return;

    if (event.type == "incoming") {
        if (session_ || !event.sealed || !event.callerId || !event.kind) return;  // the server doesn't ring someone on a call
        const std::string kind = *event.kind == "video" ? "video" : "voice";
        const std::string callerId = *event.callerId;
        const live::SealedSignal offer = *event.sealed;

        auto showBlocked = [this](const std::string& handle, const QString& message) {
            if (session_) return;
            peerHandle_ = QString::fromStdString(handle);
            direction_ = QStringLiteral("incoming");
            phase_ = QStringLiteral("ended");
            endedMessage_ = message;
            emit callChanged();
            endedTimer_.start(kEndedBannerMs * 2);
        };
        auto declineQuietly = [this](const std::string& callId) {
            auto api = api_;
            runInBackground(pool_, this, [api, callId] { live::declineCall(*api, callId); }, [] {}, [](const Failure&) {});
        };

        if (event.linkId) {
            // Someone opened one of your call links.
            struct Guest {
                std::optional<live::CallLinkDto> link;
                std::optional<api::UserResult> user;
            };
            auto api = api_;
            const std::string linkId = *event.linkId;
            runInBackground(
                pool_, this,
                [api, linkId, callerId] {
                    Guest guest;
                    for (auto& l : live::listCallLinks(*api)) {
                        if (l.id == linkId) guest.link = l;
                    }
                    try {
                        guest.user = api->getUser(callerId);
                    } catch (...) {
                    }
                    return guest;
                },
                [this, event, kind, callerId, offer, showBlocked, declineQuietly](Guest guest) {
                    if (session_ || !guest.link) return;
                    const std::string handle = guest.user ? guest.user->username : std::string("Someone");
                    std::shared_ptr<crypto::SecretBytes> secret;
                    std::string name;
                    try {
                        secret = std::make_shared<crypto::SecretBytes>(
                            calls::openOwnerSecret(guest.link->secretSealed, identity_->publicKey, identity_->privateKey.view()));
                        name = calls::openLinkName({guest.link->nameCiphertext, guest.link->nameNonce}, secret->view());
                    } catch (const std::exception&) {
                        return;
                    }
                    auto session = std::make_shared<Session>();
                    try {
                        session->offerSdp = calls::openWithLink({offer.ciphertext, offer.nonce}, secret->view(),
                                                                calls::Expected{"offer", event.callId, "link:" + guest.link->id, callerId,
                                                                                identity_->userId, std::nullopt})
                                                .sdp;
                    } catch (const std::exception&) {
                        declineQuietly(event.callId);
                        showBlocked(handle, QStringLiteral("Blocked a request to join \"%1\" that wasn't made with the link.")
                                                .arg(QString::fromStdString(name)));
                        return;
                    }
                    session->callId = event.callId;
                    session->linkId = guest.link->id;
                    session->linkName = name;
                    session->linkSecret = secret;
                    session->peerUserId = callerId;
                    session->peerHandle = handle;
                    session->peerAvatarUrl = guest.user && guest.user->avatarUrl ? *guest.user->avatarUrl : "";
                    session->kind = kind;
                    session->direction = "incoming";
                    session->iceServers = event.iceServers;
                    ring(session);
                },
                [](const Failure&) {});
            return;
        }

        if (!event.conversationId) return;
        // Right after sign-in the chat list may still be loading: look for the chat a few times.
        auto attempt = std::make_shared<std::function<void(int)>>();
        QPointer<CallController> self(this);
        std::weak_ptr<std::function<void(int)>> weakAttempt = attempt;
        *attempt = [self, weakAttempt, event, kind, callerId, offer, showBlocked, declineQuietly](int tries) {
            if (!self || self->session_) return;
            const auto meta = self->chats_ ? self->chats_->metaFor(*event.conversationId) : nullptr;
            if (!meta || meta->otherUserId != callerId || meta->otherPublicKey.empty()) {
                auto next = weakAttempt.lock();
                if (next && tries < 20) QTimer::singleShot(300, self, [next, tries] { (*next)(tries + 1); });
                return;
            }
            auto session = std::make_shared<Session>();
            try {
                session->offerSdp = calls::open({offer.ciphertext, offer.nonce}, meta->otherPublicKey, self->identity_->privateKey.view(),
                                                calls::Expected{"offer", event.callId, meta->id, callerId, self->identity_->userId, kind})
                                        .sdp;
            } catch (const std::exception&) {
                // Not sealed by this contact's key: never ring for it.
                declineQuietly(event.callId);
                showBlocked(meta->handle, QStringLiteral("Blocked a call that couldn't be verified as coming from %1's key.")
                                              .arg(QString::fromStdString(meta->handle)));
                return;
            }
            session->callId = event.callId;
            session->conversationId = meta->id;
            session->peerUserId = callerId;
            session->peerHandle = meta->handle;
            session->peerAvatarUrl = meta->avatarUrl.value_or("");
            session->peerPublicKey = meta->otherPublicKey;
            session->kind = kind;
            session->direction = "incoming";
            session->iceServers = event.iceServers;
            self->ring(session);
        };
        // Each pending retry holds the chain; it goes once found or given up.
        (*attempt)(0);
        return;
    }

    auto session = session_;
    if (!session || session->callId != event.callId) return;

    if (event.type == "answered") {
        if (session->direction != "outgoing" || !session->media || !event.sealed) return;
        stopTone();
        phase_ = QStringLiteral("connecting");
        emit callChanged();
        std::string sdp;
        try {
            const calls::Sealed sealed{event.sealed->ciphertext, event.sealed->nonce};
            calls::Expected expected{"answer", session->callId, session->sealedConversationId(), session->peerUserId, identity_->userId,
                                     std::nullopt};
            if (session->linkSecret) {
                sdp = calls::openWithLink(sealed, session->linkSecret->view(), expected).sdp;
            } else {
                expected.kind = session->kind;
                sdp = calls::open(sealed, session->peerPublicKey, identity_->privateKey.view(), expected).sdp;
            }
        } catch (const calls::CallSignalError& e) {
            fail(QStringLiteral("Hung up: ") + QString::fromUtf8(e.what()));
            return;
        } catch (const std::exception&) {
            fail(QStringLiteral("Couldn't connect the call."));
            return;
        }
        auto media = session->media;
        runInBackground(pool_, this, [media, sdp] { media->setAnswer(sdp); }, [] {},
                        [this, session](const Failure&) {
                            if (session_ == session) fail(QStringLiteral("Couldn't connect the call."));
                        });
    } else if (event.type == "screen") {
        remoteSharing_ = event.on;
        emit callChanged();
    } else if (event.type == "ended") {
        const std::string reason = event.reason.value_or("hangup");
        QString outcome;
        if (reason == "declined") outcome = QStringLiteral("declined");
        if (reason == "missed") outcome = session->direction == "incoming" ? QStringLiteral("missed") : QStringLiteral("unanswered");
        finish(endedText(reason, session->direction == "outgoing"), outcome);
    }
}

// --- History (this computer only, encrypted) ---

void CallController::remember(const Session& session, const QString& outcome) {
    if (session.callId.empty()) return;
    QVariantMap record{
        {"id", QString::fromStdString(session.callId)},
        {"peerUserId", QString::fromStdString(session.peerUserId)},
        {"peerHandle", QString::fromStdString(session.peerHandle)},
        {"conversationId", session.conversationId.empty() ? QVariant() : QVariant(QString::fromStdString(session.conversationId))},
        {"linkName", session.linkName.empty() ? QVariant() : QVariant(QString::fromStdString(session.linkName))},
        {"kind", QString::fromStdString(session.kind)},
        {"direction", QString::fromStdString(session.direction)},
        {"outcome", outcome},
        {"startedAt", session.startedAt},
        {"durationSec", session.connectedAt ? (QDateTime::currentMSecsSinceEpoch() - session.connectedAt + 500) / 1000 : 0},
    };
    for (qsizetype i = history_.size() - 1; i >= 0; --i) {
        if (history_[i].toMap().value("id") == record.value("id")) history_.removeAt(i);
    }
    history_.prepend(record);
    std::stable_sort(history_.begin(), history_.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("startedAt").toLongLong() > b.toMap().value("startedAt").toLongLong();
    });
    while (history_.size() > kMaxHistory) history_.removeLast();
    saveHistory();
    emit historyChanged();
}

void CallController::clearHistory() {
    history_.clear();
    saveHistory();
    emit historyChanged();
}

void CallController::loadHistory() {
    history_.clear();
    QFile file(historyPath(identity_->userId));
    if (!file.open(QIODevice::ReadOnly)) return;
    try {
        const QJsonObject box = QJsonDocument::fromJson(file.readAll()).object();
        const auto key = calls::deriveLocalKey(identity_->privateKey.view(), "redvault-call-history-v1");
        const std::string plain = crypto::decryptWithConversationKey(crypto::base64Decode(box.value("c").toString().toStdString()),
                                                                     crypto::base64Decode(box.value("n").toString().toStdString()), key.view());
        for (const auto& value : QJsonDocument::fromJson(QByteArray::fromStdString(plain)).array()) history_.append(value.toObject().toVariantMap());
    } catch (const std::exception&) {
        history_.clear();  // unreadable (another account's key): start over
    }
}

void CallController::saveHistory() {
    try {
        const auto key = calls::deriveLocalKey(identity_->privateKey.view(), "redvault-call-history-v1");
        const QByteArray json = QJsonDocument(QJsonArray::fromVariantList(history_)).toJson(QJsonDocument::Compact);
        const auto sealed = crypto::encryptWithConversationKey(json.toStdString(), key.view());
        QJsonObject box{{"c", QString::fromStdString(crypto::base64Encode(sealed.ciphertext))},
                        {"n", QString::fromStdString(crypto::base64Encode(sealed.nonce))}};
        QSaveFile file(historyPath(identity_->userId));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(box).toJson(QJsonDocument::Compact));
            file.commit();
        }
    } catch (const std::exception&) {
        // The call just isn't remembered.
    }
}

// --- Call links ---

void CallController::refreshLinks() {
    auto api = api_;
    auto identity = identity_;
    const std::string base = webAppBase().toStdString();
    runInBackground(
        pool_, this,
        [api, identity, base] {
            QVariantList out;
            for (const auto& link : live::listCallLinks(*api)) {
                try {
                    const auto secret = calls::openOwnerSecret(link.secretSealed, identity->publicKey, identity->privateKey.view());
                    out.append(QVariantMap{
                        {"id", QString::fromStdString(link.id)},
                        {"kind", QString::fromStdString(link.kind)},
                        {"name", QString::fromStdString(calls::openLinkName({link.nameCiphertext, link.nameNonce}, secret.view()))},
                        {"url", QString::fromStdString(calls::linkUrl(base, link.id, secret.view()))},
                        {"createdOn", QString::fromStdString(link.createdOn)},
                    });
                } catch (const std::exception&) {
                    // Not openable with this key: skip it.
                }
            }
            return out;
        },
        [this](QVariantList links) {
            links_ = std::move(links);
            emit linksChanged();
        },
        [](const Failure&) {});
}

void CallController::createLink(const QString& nameIn, const QString& kindIn) {
    const QString name = nameIn.trimmed().isEmpty() ? QStringLiteral("Call link") : nameIn.trimmed().left(60);
    const std::string kind = kindIn == QStringLiteral("video") ? "video" : "voice";
    if (tierRank(tier_()) < (kind == "video" ? 2 : 1)) {
        linkError_ = kind == "video" ? QStringLiteral("Video call links are part of Premium.")
                                     : QStringLiteral("Call links are part of Pro and Premium.");
        emit linksChanged();
        return;
    }
    auto api = api_;
    auto identity = identity_;
    const std::string base = webAppBase().toStdString();
    runInBackground(
        pool_, this,
        [api, identity, base, name, kind] {
            const auto secret = calls::newLinkSecret();
            const auto sealedName = calls::sealLinkName(name.toStdString(), secret.view());
            const auto link = live::createCallLink(*api, kind, calls::sealSecretForOwner(secret.view(), identity->publicKey), sealedName.ciphertext,
                                                   sealedName.nonce);
            return QString::fromStdString(calls::linkUrl(base, link.id, secret.view()));
        },
        [this](const QString& url) {
            linkError_.clear();
            emit linkCreated(url);
            refreshLinks();
        },
        [this](const Failure& failure) {
            linkError_ = describeError(failure, QStringLiteral("Couldn't create the link."));
            emit linksChanged();
        });
}

void CallController::deleteLink(const QString& id) {
    auto api = api_;
    const std::string linkId = id.toStdString();
    runInBackground(
        pool_, this, [api, linkId] { live::deleteCallLink(*api, linkId); }, [this] { refreshLinks(); },
        [this](const Failure& failure) {
            linkError_ = describeError(failure, QStringLiteral("Couldn't delete the link."));
            emit linksChanged();
        });
}

}  // namespace redvault::app
