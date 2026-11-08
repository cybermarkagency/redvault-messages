// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QSoundEffect>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/chat_models.hpp"

namespace redvault::rtc {
class CallMedia;
}

namespace redvault::app {

class AudioIo;
class ChatSession;
class GroupCallController;
class LiveSession;
class VideoIo;

/**
 * One-to-one voice and video calls with screen sharing, between contacts or
 * through a call link: the desktop counterpart of the web app's
 * state/CallContext.tsx, and interoperable with it.
 *
 *   - Media: the rtc engine (libdatachannel, Opus, VP8), relayed through
 *     TURN only so nobody learns anyone's IP address, encrypted end to end
 *     by DTLS-SRTP.
 *   - Setup: offer and answer are sealed end to end (identity keys for a
 *     contact, the link's secret for a call link), so the server can't
 *     slip into the call. Once connected, both screens show the same four
 *     words from both fingerprints.
 *   - Plans: one-to-one voice and video calls with a contact are free.
 *     Call links need Pro (voice) or Premium (video), and a link's owner
 *     hosts; screen sharing needs Premium. The server enforces each.
 *   - History: kept only on this computer, encrypted with a key derived
 *     from the account's private key.
 */
class CallController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(QString phase READ phase NOTIFY callChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY callChanged)
    Q_PROPERTY(QString direction READ direction NOTIFY callChanged)
    Q_PROPERTY(QString peerUserId READ peerUserId NOTIFY callChanged)
    Q_PROPERTY(QString peerHandle READ peerHandle NOTIFY callChanged)
    Q_PROPERTY(QString peerAvatarUrl READ peerAvatarUrl NOTIFY callChanged)
    Q_PROPERTY(QString conversationId READ conversationId NOTIFY callChanged)
    Q_PROPERTY(QString linkName READ linkName NOTIFY callChanged)
    Q_PROPERTY(bool isLink READ isLink NOTIFY callChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY callChanged)
    Q_PROPERTY(bool cameraOff READ cameraOff NOTIFY callChanged)
    Q_PROPERTY(bool sharingScreen READ sharingScreen NOTIFY callChanged)
    Q_PROPERTY(bool remoteSharing READ remoteSharing NOTIFY callChanged)
    Q_PROPERTY(qint64 connectedAt READ connectedAt NOTIFY callChanged)
    Q_PROPERTY(QStringList sas READ sas NOTIFY callChanged)
    Q_PROPERTY(bool minimized READ minimized NOTIFY callChanged)
    Q_PROPERTY(bool chatOpen READ chatOpen NOTIFY callChanged)
    Q_PROPERTY(QString endedMessage READ endedMessage NOTIFY callChanged)
    Q_PROPERTY(bool callsAvailable READ callsAvailable NOTIFY callsAvailableChanged)
    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList speakers READ speakers NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY devicesChanged)
    Q_PROPERTY(QString microphoneId READ microphoneId NOTIFY devicesChanged)
    Q_PROPERTY(QString speakerId READ speakerId NOTIFY devicesChanged)
    Q_PROPERTY(QString cameraId READ cameraId NOTIFY devicesChanged)
    Q_PROPERTY(bool echoCancellation READ echoCancellation CONSTANT)
    Q_PROPERTY(bool ringtone READ ringtone WRITE setRingtone NOTIFY ringtoneChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantList links READ links NOTIFY linksChanged)
    Q_PROPERTY(QString linkError READ linkError NOTIFY linksChanged)
    Q_PROPERTY(QString webAppBase READ webAppBase CONSTANT)

public:
    using TierProvider = std::function<std::string()>;

    CallController(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, LiveSession* live,
                   ChatSession* chats, TierProvider tier, QObject* parent = nullptr);
    ~CallController() override;

    /// Group calls are separate, but nobody is in two calls at once: each side asks the other.
    void setGroupCalls(GroupCallController* groupCalls) { groupCalls_ = groupCalls; }
    /// A one-to-one call is going on (or ringing).
    bool onCall() const { return session_ != nullptr; }
    /// The ringing / dialing sound, shared with group calls.
    void playTone(bool incoming);
    void stopTone();
    /// Whether an incoming call from `chatId` should ring out loud (the ringtone setting and the chat's mute).
    bool shouldRing(const QString& chatId) const;

    QString phase() const { return phase_; }
    QString kind() const { return kind_; }
    QString direction() const { return direction_; }
    QString peerUserId() const { return peerUserId_; }
    QString peerHandle() const { return peerHandle_; }
    QString peerAvatarUrl() const { return peerAvatarUrl_; }
    QString conversationId() const { return conversationId_; }
    QString linkName() const { return linkName_; }
    bool isLink() const { return !linkId_.isEmpty(); }
    bool muted() const { return muted_; }
    bool cameraOff() const { return cameraOff_; }
    bool sharingScreen() const { return sharingScreen_; }
    bool remoteSharing() const { return remoteSharing_; }
    qint64 connectedAt() const { return connectedAt_; }
    QStringList sas() const { return sas_; }
    bool minimized() const { return minimized_; }
    bool chatOpen() const { return chatOpen_; }
    QString endedMessage() const { return endedMessage_; }
    bool callsAvailable() const;
    QVariantList microphones() const;
    QVariantList speakers() const;
    QVariantList cameras() const;
    QString microphoneId() const;
    QString speakerId() const;
    QString cameraId() const;
    bool echoCancellation() const;
    bool ringtone() const;
    void setRingtone(bool on);
    QVariantList history() const { return history_; }
    QVariantList links() const { return links_; }
    QString linkError() const { return linkError_; }
    QString webAppBase() const;

    /// Starts a call to a 1:1 chat. Returns a message to show instead when the plan doesn't include it.
    Q_INVOKABLE QString startCall(const QString& chatId, const QString& kind);
    /// Asks to join someone's call link (the whole URL, secret included). A group call link (or the chat message
    /// carrying one) joins that call in the app, through the group call controller. Returns a message to show.
    Q_INVOKABLE QString joinLink(const QString& url);
    Q_INVOKABLE void accept(bool audioOnly = false);
    Q_INVOKABLE void decline();
    Q_INVOKABLE void hangUp();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleCamera();
    Q_INVOKABLE QString toggleScreenShare();
    Q_INVOKABLE void setMinimized(bool minimized);
    Q_INVOKABLE void toggleChat();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void setMicrophone(const QString& id);
    Q_INVOKABLE void setSpeaker(const QString& id);
    Q_INVOKABLE void setCamera(const QString& id);
    /// The call window's VideoOutput.videoSink objects.
    Q_INVOKABLE void setVideoSinks(QObject* localSink, QObject* remoteSink);
    Q_INVOKABLE void refreshLinks();
    Q_INVOKABLE void createLink(const QString& name, const QString& kind);
    Q_INVOKABLE void deleteLink(const QString& id);
    Q_INVOKABLE void clearHistory();
    Q_INVOKABLE static void copyText(const QString& text);

signals:
    void callChanged();
    void callsAvailableChanged();
    void devicesChanged();
    void ringtoneChanged();
    void historyChanged();
    void linksChanged();
    void linkCreated(const QString& url);
    /// A call just rang in (for the tray notification).
    void ringing(const QString& handle, const QString& kind);

private:
    struct Session;
    void onLiveEvent(const api::live::LiveEvent& event);
    void ring(std::shared_ptr<Session> session);
    void startMedia(bool withVideo);
    void runOutgoing(std::shared_ptr<Session> session, std::function<api::live::StartedCall()> reserve);
    void onEngineState(int state);
    void finish(const QString& message, const QString& outcome = {});
    void fail(const QString& message);
    void resetView();
    void remember(const Session& session, const QString& outcome);
    void loadHistory();
    void saveHistory();
    void refreshDevices();

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    QPointer<LiveSession> live_;
    QPointer<ChatSession> chats_;
    QPointer<GroupCallController> groupCalls_;
    TierProvider tier_;
    QThreadPool pool_;
    QThread audioThread_;
    QSoundEffect tone_;
    QTimer disconnectTimer_;
    QTimer endedTimer_;

    std::shared_ptr<Session> session_;
    std::shared_ptr<rtc::CallMedia> media_;
    AudioIo* audio_ = nullptr;  // lives on audioThread_
    VideoIo* video_ = nullptr;
    QPointer<QObject> localSinkObject_;
    QPointer<QObject> remoteSinkObject_;

    QString phase_;
    QString kind_;
    QString direction_;
    QString peerUserId_;
    QString peerHandle_;
    QString peerAvatarUrl_;
    QString conversationId_;
    QString linkId_;
    QString linkName_;
    bool muted_ = false;
    bool cameraOff_ = true;
    bool sharingScreen_ = false;
    bool remoteSharing_ = false;
    qint64 connectedAt_ = 0;
    QStringList sas_;
    bool minimized_ = false;
    bool chatOpen_ = false;
    QString endedMessage_;

    QVariantList history_;
    QVariantList links_;
    QString linkError_;
};

}  // namespace redvault::app
