#include "desktop_shell.hpp"

#include <QApplication>
#include <QDir>
#include <QJSEngine>
#include <QMenu>
#include <QQmlEngine>
#include <QSettings>
#include <QSystemTrayIcon>

#include "app_controller.hpp"
#include "call_notice.hpp"

#ifdef _WIN32
#include <windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace redvault::app {

namespace {

DesktopShell* gInstance = nullptr;

const QString kCapture = QStringLiteral("desktop/captureProtection");
const QString kCloseToTray = QStringLiteral("desktop/closeToTray");
const QString kNotifications = QStringLiteral("desktop/notifications");
const QString kShowSender = QStringLiteral("desktop/notificationShowsSender");

bool setting(const QString& key, bool fallback) {
    return QSettings().value(key, fallback).toBool();
}

// Every size of the logo, each drawn for that size, so nothing gets scaled into a blur.
QIcon appIcon() {
    QIcon icon;
    const QDir folder(QStringLiteral(":/qt/qml/Redvault/icons/app"));
    for (const QString& file : folder.entryList({QStringLiteral("redvault-*.png")}, QDir::Files)) {
        icon.addFile(folder.filePath(file));
    }
    return icon;
}

}  // namespace

DesktopShell::DesktopShell(AppController* app, QObject* parent)
    : QObject(parent), app_(app), icon_(appIcon()) {
    QGuiApplication::setWindowIcon(icon_);
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

    menu_ = std::make_unique<QMenu>();
    menu_->addAction(QStringLiteral("Open redvaultmessages"), this, &DesktopShell::showWindow);
    lockAction_ = menu_->addAction(QStringLiteral("Lock now"), this, [this] { app_->lock()->lockNow(); });
    menu_->addSeparator();
    menu_->addAction(QStringLiteral("Quit (signs you out)"), app_, &AppController::quit);
    connect(menu_.get(), &QMenu::aboutToShow, this, &DesktopShell::updateMenu);

    tray_ = std::make_unique<QSystemTrayIcon>(icon_);
    tray_->setToolTip(QStringLiteral("redvaultmessages"));
    tray_->setContextMenu(menu_.get());
    connect(tray_.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showWindow();
    });
    connect(tray_.get(), &QSystemTrayIcon::messageClicked, this, [this] {
        showWindow();
        if (!lastNotifiedChannelId_.isEmpty()) {
            emit openChannelRequested(lastNotifiedChannelId_);
        } else if (!lastNotifiedChatId_.isEmpty()) {
            emit openChatRequested(lastNotifiedChatId_);
        }
    });
    tray_->show();
}

DesktopShell::~DesktopShell() {
    if (tray_) tray_->hide();
#ifdef _WIN32
    for (void* icon : taskbarIcons_) {
        if (icon) DestroyIcon(static_cast<HICON>(icon));
    }
#endif
}

DesktopShell* DesktopShell::create(QQmlEngine*, QJSEngine*) {
    QJSEngine::setObjectOwnership(gInstance, QJSEngine::CppOwnership);
    return gInstance;
}

void DesktopShell::setInstance(DesktopShell* instance) {
    gInstance = instance;
}

bool DesktopShell::trayAvailable() const {
    return tray_ != nullptr;
}

// --- Settings (this computer only) ---

bool DesktopShell::captureProtection() const {
    return setting(kCapture, false);
}

void DesktopShell::setCaptureProtection(bool on) {
    QSettings().setValue(kCapture, on);
    applyCaptureProtection();
    emit settingsChanged();
}

bool DesktopShell::closeToTray() const {
    return setting(kCloseToTray, true);
}

void DesktopShell::setCloseToTray(bool on) {
    QSettings().setValue(kCloseToTray, on);
    emit settingsChanged();
}

bool DesktopShell::notifications() const {
    return setting(kNotifications, true);
}

void DesktopShell::setNotifications(bool on) {
    QSettings().setValue(kNotifications, on);
    emit settingsChanged();
}

bool DesktopShell::notificationShowsSender() const {
    return setting(kShowSender, false);
}

void DesktopShell::setNotificationShowsSender(bool on) {
    QSettings().setValue(kShowSender, on);
    emit settingsChanged();
}

// --- Window ---

void DesktopShell::attachWindow(QWindow* window) {
    window_ = window;
    applyCaptureProtection();
    applyTaskbarIcon();
    // The native window can be recreated (e.g. when shown again); keep protection on it.
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) return;
        applyCaptureProtection();
        applyTaskbarIcon();
    });
    connect(window, &QWindow::screenChanged, this, [this] { applyTaskbarIcon(); });
}

// Qt hands Windows a 32 px "big" icon at 100% scaling, but the taskbar draws at 24 px and squeezes
// it to fit, which is what blurs it. So the window gets the size the taskbar really draws.
void DesktopShell::applyTaskbarIcon() {
#ifdef _WIN32
    if (!window_) return;
    const auto hwnd = reinterpret_cast<HWND>(window_->winId());
    const UINT dpi = GetDpiForWindow(hwnd);
    const WPARAM kinds[] = {ICON_BIG, ICON_SMALL};
    const int sizes[] = {MulDiv(24, static_cast<int>(dpi), 96), GetSystemMetricsForDpi(SM_CXSMICON, dpi)};
    for (int i = 0; i < 2; ++i) {
        // IDI_ICON1 in redvault.rc: Windows takes the frame of this exact size from the .ico.
        const HANDLE icon = LoadImageW(GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON, sizes[i], sizes[i], LR_DEFAULTCOLOR);
        if (!icon) continue;
        SendMessageW(hwnd, WM_SETICON, kinds[i], reinterpret_cast<LPARAM>(icon));
        if (taskbarIcons_[i]) DestroyIcon(static_cast<HICON>(taskbarIcons_[i]));
        taskbarIcons_[i] = icon;
    }
#endif
}

void DesktopShell::applyCaptureProtection() {
#ifdef _WIN32
    if (!window_) return;
    const auto hwnd = reinterpret_cast<HWND>(window_->winId());
    if (!captureProtection()) {
        SetWindowDisplayAffinity(hwnd, WDA_NONE);
        return;
    }
    // Windows 10 2004+: the window is simply left out of captures. Older
    // versions only support blacking it out, which still hides the content.
    if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
#endif
}

void DesktopShell::showWindow() {
    if (!window_) return;
    window_->show();
    if (window_->windowStates() & Qt::WindowMinimized) window_->setWindowStates(window_->windowStates() & ~Qt::WindowMinimized);
    window_->raise();
    window_->requestActivate();
}

void DesktopShell::hideToTray() {
    if (!window_) return;
    window_->hide();
    if (tray_ && !toldAboutTray_) {
        toldAboutTray_ = true;
        tray_->showMessage(QStringLiteral("redvaultmessages is still running"),
                           QStringLiteral("You'll get notifications here. Quit from this icon's menu to sign out."),
                           icon_, 6000);
    }
}

void DesktopShell::updateMenu() {
    if (lockAction_) lockAction_->setVisible(app_->state() == AppController::State::SignedIn && app_->lock()->enabled());
}

// --- Notifications ---

void DesktopShell::notify(const QString& title, const QString& text) {
    if (tray_) tray_->showMessage(title, text, icon_, 6000);
}

void DesktopShell::notifyIncomingCall(const QString& handle, const QString& kind) {
    if (!notifications()) return;
    const CallNotice notice = callNotice(handle, kind, notificationShowsSender() && !app_->lock()->locked());
    if (tray_) tray_->showMessage(notice.title, notice.text, icon_, 6000);
}

void DesktopShell::notifyDeviceKeyMessages(int count) {
    if (const auto notice = deviceKeyNotice(count, notifications())) notify(notice->title, notice->text);
}

void DesktopShell::notifyIncoming(const QString& chatId, const QString& title, int count, bool isCurrentChat) {
    if (!notifications() || count <= 0) return;
    const bool windowActive = window_ && window_->isVisible() && window_->isActive();
    // Looking at the app already: the chat list shows it.
    if (windowActive) return;
    (void)isCurrentChat;

    const bool named = notificationShowsSender() && !app_->lock()->locked();
    QString text;
    if (named) {
        text = count == 1 ? QStringLiteral("New message from %1").arg(title)
                          : QStringLiteral("%1 new messages from %2").arg(count).arg(title);
    } else {
        text = count == 1 ? QStringLiteral("New message") : QStringLiteral("%1 new messages").arg(count);
    }
    lastNotifiedChatId_ = chatId;
    lastNotifiedChannelId_.clear();
    if (tray_) tray_->showMessage(QStringLiteral("redvaultmessages"), text, icon_, 5000);
    if (window_) window_->alert(0);  // flash the taskbar button until the window is used
}

void DesktopShell::notifyShareIncoming(const QString& text) {
    if (!notifications()) return;
    const bool named = notificationShowsSender() && !app_->lock()->locked();
    if (tray_) {
        tray_->showMessage(QStringLiteral("New files in Incoming"), named ? text : QStringLiteral("Files were added through one of your sharing links."),
                           icon_, 6000);
    }
}

void DesktopShell::notifyChannelPost(const QString& channelId, const QString& channelName, const QString& preview) {
    if (!notifications()) return;
    const bool named = notificationShowsSender() && !app_->lock()->locked();
    lastNotifiedChannelId_ = channelId;
    if (tray_) {
        tray_->showMessage(named ? channelName : QStringLiteral("redvaultmessages"),
                           named ? preview : QStringLiteral("New post in a channel you follow"), icon_, 6000);
    }
}

}  // namespace redvault::app
