#pragma once

#include <QIcon>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QMenu;
class QSystemTrayIcon;
class QAction;
class QQmlEngine;
class QJSEngine;

namespace redvault::app {

class AppController;

/**
 * The parts of the app that belong to Windows rather than to Redvault: the
 * tray icon and its menu, new-message notifications, keeping running in the
 * tray when the window is closed, and hiding the window from screenshots,
 * recordings and screen sharing. Exposed to QML as the `Shell` singleton.
 *
 * Notifications never contain message text. By default they don't say who
 * it's from either, because Windows keeps notifications in its history;
 * showing the sender is opt-in, and never while the app is locked.
 */
class DesktopShell : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Shell)
    QML_SINGLETON
    Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)
    Q_PROPERTY(bool captureProtection READ captureProtection WRITE setCaptureProtection NOTIFY settingsChanged)
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY settingsChanged)
    Q_PROPERTY(bool notifications READ notifications WRITE setNotifications NOTIFY settingsChanged)
    Q_PROPERTY(bool notificationShowsSender READ notificationShowsSender WRITE setNotificationShowsSender NOTIFY settingsChanged)

public:
    explicit DesktopShell(AppController* app, QObject* parent = nullptr);
    ~DesktopShell() override;

    /// QML_SINGLETON factory: hands QML the instance main() created.
    static DesktopShell* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    static void setInstance(DesktopShell* instance);

    bool trayAvailable() const;
    bool captureProtection() const;
    void setCaptureProtection(bool on);
    bool closeToTray() const;
    void setCloseToTray(bool on);
    bool notifications() const;
    void setNotifications(bool on);
    bool notificationShowsSender() const;
    void setNotificationShowsSender(bool on);

    /// The main window: capture protection is applied to it, and the tray brings it back.
    Q_INVOKABLE void attachWindow(QWindow* window);
    Q_INVOKABLE void showWindow();
    /// Hides the window to the tray (the app keeps running). Says so the first time.
    Q_INVOKABLE void hideToTray();

    /// A plain notice from the tray (e.g. "an update is available").
    void notify(const QString& title, const QString& text);

    /// Someone is calling. Same rules as a message: off when notifications are, and a name only if that's turned on and the app isn't locked.
    void notifyIncomingCall(const QString& handle, const QString& kind);

    /// Device-key messages in chats that aren't open: how many, and nothing else. Off when notifications are.
    void notifyDeviceKeyMessages(int count);

    /// New incoming messages in a chat (never their content).
    void notifyIncoming(const QString& chatId, const QString& title, int count, bool isCurrentChat);

    /**
     * A new post in a followed channel: its name and text (channels are
     * public, plain text by design), unless notifications are off; while
     * the app is locked or names are hidden, only that there's a new post.
     */
    void notifyChannelPost(const QString& channelId, const QString& channelName, const QString& preview);
    /// Files arrived through one of your public share links (the link's name only while the app isn't locked).
    void notifyShareIncoming(const QString& text);

signals:
    void settingsChanged();
    /// The user clicked a notification for this chat.
    void openChatRequested(const QString& chatId);
    /// The user clicked a notification for a channel's new post.
    void openChannelRequested(const QString& channelId);

private:
    void applyCaptureProtection();
    void applyTaskbarIcon();
    void updateMenu();

    AppController* app_;
    QPointer<QWindow> window_;
    QIcon icon_;
    void* taskbarIcons_[2] = {nullptr, nullptr};  // Windows HICONs for the current scaling
    std::unique_ptr<QMenu> menu_;
    std::unique_ptr<QSystemTrayIcon> tray_;
    QAction* lockAction_ = nullptr;
    QString lastNotifiedChatId_;
    /// Set when the last notification was a channel's post (clicking it opens the channel).
    QString lastNotifiedChannelId_;
    bool toldAboutTray_ = false;
};

}  // namespace redvault::app
