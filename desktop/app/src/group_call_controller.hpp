// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/chat_models.hpp"

namespace redvault::lk {
class GroupCallSession;
}

namespace redvault::app {

class AudioIo;
class CallController;
class ChatSession;
class LiveSession;
class VideoIo;

/**
 * Group voice and video calls, in the app: the desktop counterpart of the web
 * app's state/GroupCallContext.tsx, and interoperable with it. The call logic
 * is redvault_groupcall's GroupCallSession (Qt-free, tested on its own); this
 * is its Qt face: properties and tiles for QML, the microphone, speaker, camera
 * and screen, the ring tone, and posting the call's link in the group chat.
 *
 *   - Media: LiveKit's native client library, through a self-hosted SFU,
 *     with every frame also end-to-end encrypted under a room key the server
 *     never holds (docs/group-calls.md).
 *   - Plans: only a Premium member starts a call; anyone in the group joins;
 *     screen sharing needs your own Premium. The server enforces each.
 *   - Links: the starter's app posts the call's link in the chat once it is
 *     connected; members paste it into Calls -> Join a call link, or click it
 *     in the chat, and join here.
 */
class GroupCallController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    /// The server has group calls (LiveKit) and the media library is installed.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool present READ present NOTIFY viewChanged)
    Q_PROPERTY(bool inCall READ inCall NOTIFY viewChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY viewChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY viewChanged)
    Q_PROPERTY(QString conversationId READ conversationId NOTIFY viewChanged)
    Q_PROPERTY(QString groupLabel READ groupLabel NOTIFY viewChanged)
    Q_PROPERTY(QString hostHandle READ hostHandle NOTIFY viewChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY viewChanged)
    Q_PROPERTY(bool cameraOff READ cameraOff NOTIFY viewChanged)
    Q_PROPERTY(bool sharingScreen READ sharingScreen NOTIFY viewChanged)
    Q_PROPERTY(bool minimized READ minimized NOTIFY viewChanged)
    Q_PROPERTY(qint64 connectedAt READ connectedAt NOTIFY viewChanged)
    Q_PROPERTY(QStringList sas READ sas NOTIFY viewChanged)
    Q_PROPERTY(bool keyReady READ keyReady NOTIFY viewChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY viewChanged)
    Q_PROPERTY(QString endedMessage READ endedMessage NOTIFY viewChanged)
    /// One entry per picture: key, userId, handle, avatarUrl, isSelf, screen, videoOn, micMuted, speaking.
    Q_PROPERTY(QVariantList tiles READ tiles NOTIFY viewChanged)
    /// Goes up whenever the set of calls going on in your groups changes (re-read callIn()).
    Q_PROPERTY(int activeRevision READ activeRevision NOTIFY activeChanged)
    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList speakers READ speakers NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY devicesChanged)
    Q_PROPERTY(QString microphoneId READ microphoneId NOTIFY devicesChanged)
    Q_PROPERTY(QString speakerId READ speakerId NOTIFY devicesChanged)
    Q_PROPERTY(QString cameraId READ cameraId NOTIFY devicesChanged)

public:
    using TierProvider = std::function<std::string()>;

    GroupCallController(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, LiveSession* live,
                        ChatSession* chats, CallController* calls, TierProvider tier, QObject* parent = nullptr);
    ~GroupCallController() override;

    bool available() const;
    bool present() const;
    bool inCall() const;
    QString phase() const;
    QString kind() const;
    QString conversationId() const;
    QString groupLabel() const;
    QString hostHandle() const;
    bool muted() const;
    bool cameraOff() const;
    bool sharingScreen() const;
    bool minimized() const;
    qint64 connectedAt() const;
    QStringList sas() const;
    bool keyReady() const;
    QString notice() const;
    QString endedMessage() const;
    QVariantList tiles() const { return tiles_; }
    int activeRevision() const { return activeRevision_; }
    QVariantList microphones() const;
    QVariantList speakers() const;
    QVariantList cameras() const;
    QString microphoneId() const;
    QString speakerId() const;
    QString cameraId() const;

    /// Starts a call in a group chat (or joins the one going on there). Returns a message to show instead when it can't.
    Q_INVOKABLE QString start(const QString& chatId, const QString& kind);
    /// Joins the call a pasted group call link, or the whole chat message carrying one, points to. Returns a message to show.
    Q_INVOKABLE QString joinLink(const QString& text);
    /// Whether `text` is a group call link that points at this app's own server (so clicking it joins here).
    Q_INVOKABLE bool isOwnCallLink(const QString& text) const;
    /// The call going on in a group chat, for the Join button: callId, kind, participants (empty: none).
    Q_INVOKABLE QVariantMap callIn(const QString& chatId) const;
    /// Asks the server which of your groups have a call going on (when a group chat opens).
    Q_INVOKABLE void refreshActive();
    Q_INVOKABLE void accept();
    Q_INVOKABLE void decline();
    Q_INVOKABLE void leave();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleCamera();
    /// Returns a message to show when it isn't allowed (it needs Premium).
    Q_INVOKABLE QString toggleScreenShare();
    Q_INVOKABLE void setMinimized(bool minimized);
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void setMicrophone(const QString& id);
    Q_INVOKABLE void setSpeaker(const QString& id);
    Q_INVOKABLE void setCamera(const QString& id);
    /// A call screen tile's VideoOutput.videoSink: its picture is shown there while the tile exists.
    Q_INVOKABLE void setTileSink(const QString& key, QObject* sink);
    /// Forgets the tile's sink, but only if it is still `sink` (another tile may have taken the key over).
    Q_INVOKABLE void clearTileSink(const QString& key, QObject* sink);

signals:
    void availableChanged();
    void viewChanged();
    void activeChanged();
    void devicesChanged();
    /// A call just rang in (for the tray notification).
    void ringing(const QString& handle, const QString& kind);

private:
    void onSessionChanged();
    void reconcileMedia();
    void startMedia();
    void stopMedia();
    void showTile(const QString& key, const QVideoFrame& frame);
    void applySinks();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    QPointer<LiveSession> live_;
    QPointer<ChatSession> chats_;
    QPointer<CallController> calls_;
    TierProvider tier_;
    std::unique_ptr<lk::GroupCallSession> session_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    bool libraryOk_ = false;
    QString libraryProblem_;

    QThread audioThread_;
    QTimer endedTimer_;
    AudioIo* audio_ = nullptr;  // lives on audioThread_
    VideoIo* video_ = nullptr;
    bool cameraRunning_ = false;
    bool screenRunning_ = false;
    QString lastPhase_;

    QVariantList tiles_;
    int activeRevision_ = 0;
    QHash<QString, QPointer<QVideoSink>> sinks_;
    // Remote pictures, newest per tile: a slow UI drops frames rather than queue them.
    QMutex frameMutex_;
    QHash<QString, QVideoFrame> latestFrames_;

    // The group the call was started in, for posting its link (read by the session's worker thread).
    QMutex announceMutex_;
    std::shared_ptr<const data::ConversationMeta> announceMeta_;
};

}  // namespace redvault::app
