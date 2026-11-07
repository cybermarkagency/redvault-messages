#include <QApplication>
#include <QSettings>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>

#include <cstdlib>
#include <string>

#include "app_controller.hpp"
#include "redvault/data/sign_in.hpp"
#include "desktop_shell.hpp"
#include "tor_controller.hpp"
#include "image_providers.hpp"
#include "redvault/groupcall/ffi.hpp"
#include "redvault/util/private_files.hpp"
#include "updater.hpp"
#include "channel_media.hpp"
#include "chat_images.hpp"

namespace {

#ifdef REDVAULT_TOR_ONLY
// A Tor-only build (docs/tor-routing-plan.md): the onion service is the only server.
constexpr const char* kDefaultApiBase = REDVAULT_ONION_API_BASE;
#else
// Same default as android/gradle.properties.
constexpr const char* kDefaultApiBase = "https://www.redvaultsystems.com/redvaultmessages/api/";
#endif

}  // namespace

int main(int argc, char* argv[]) {
    // Before anything writes a file: decrypted files, settings and keys are this user's alone.
    redvault::util::restrictNewFiles();
    // QApplication (not QGuiApplication) for the tray icon and its menu.
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Redvault Systems"));
    // The internal name stays "Redvault": it locates this computer's settings
    // (last handle, app-lock PIN) and cache, which a rename must not lose.
    QApplication::setApplicationName(QStringLiteral("Redvault"));
    QApplication::setApplicationDisplayName(QStringLiteral("redvaultmessages"));
    QApplication::setApplicationVersion(QStringLiteral(REDVAULT_VERSION));
    // Lets Linux desktops match the window to redvaultmessages.desktop (its name and icon).
    QGuiApplication::setDesktopFileName(QStringLiteral("redvaultmessages"));
    // Closing the window can leave the app running in the tray; quitting is explicit.
    QApplication::setQuitOnLastWindowClosed(false);
    // Basic is fully restylable, so the app can use the same design tokens as web and Android.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("redvaultmessages: end-to-end encrypted messenger"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption apiOption(QStringLiteral("api"), QStringLiteral("API base URL (default: production)."),
                                 QStringLiteral("url"));
    parser.addOption(apiOption);
    parser.process(app);

    std::string apiBase = kDefaultApiBase;
    bool overridden = false;
    if (const char* fromEnv = std::getenv("REDVAULT_API_BASE"); fromEnv != nullptr && *fromEnv != '\0') {
        apiBase = fromEnv;
        overridden = true;
    }
    if (parser.isSet(apiOption)) {
        apiBase = parser.value(apiOption).toStdString();
        overridden = true;
    }
    // An address given at run time gets no debug-build leniency: passwords and session cookies would follow it.
    if (!redvault::net::isAllowedServerUrl(apiBase, !overridden && redvault::net::kDebugBuild)) {
        QMessageBox::critical(nullptr, QStringLiteral("redvaultmessages"),
                              QStringLiteral("That server address isn't allowed. It must use https, or be this computer or an onion service."));
        return 1;
    }

    // A session that ended in a crash never got to delete its decrypted files.
    const QDir cacheRoot(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    for (const QString& leftover : cacheRoot.entryList({QStringLiteral("session-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir(cacheRoot.filePath(leftover)).removeRecursively();
    }
    // What's left from a crash or an older version may still be readable by other accounts on this computer.
    for (const QString& folder : {cacheRoot.path(), QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}) {
        // <base>/<organization>/<application>: the organization's folder is this app's too.
        const QFileInfo info(folder);
        const bool ours = info.dir().dirName() == QCoreApplication::organizationName();
        redvault::util::tightenTree((ours ? info.absolutePath() : folder).toStdWString());
    }
    redvault::util::tightenTree(QFileInfo(QSettings().fileName()).absolutePath().toStdWString());

    // Winds LiveKit's media library down once everything that used it is gone (a no-op if no group call ever ran).
    struct FfiShutdown {
        ~FfiShutdown() { redvault::lk::Ffi::shutdown(); }
    } ffiShutdown;

    // Tor routing: whenever the server is a .onion, everything goes through the
    // embedded Tor gateway and nothing else (no fallback to a direct
    // connection). A Tor-only build refuses any other server.
    const auto server = redvault::net::parseUrl(apiBase);
    const bool onion = server && redvault::net::isOnionHost(server->host);
#ifdef REDVAULT_TOR_ONLY
    if (!onion) {
        QMessageBox::critical(nullptr, QStringLiteral("redvaultmessages"),
                              QStringLiteral("This version only connects to Redvault through Tor, and the server address it was "
                                             "given isn't Redvault's onion service."));
        return 1;
    }
#endif
    redvault::app::TorController tor(onion ? QString::fromStdString(server->host) : QString(),
                                     QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tor"));

    // Declared before the engine, so QML is torn down first.
    // Remembers which accounts this computer saw on sign-in keys (docs/auth-key-split.md).
    redvault::data::setSignInPins({
        [](const std::string& handle) { return QSettings().value(QStringLiteral("signInKeys/%1").arg(QString::fromStdString(handle))).toBool(); },
        [](const std::string& handle) { QSettings().setValue(QStringLiteral("signInKeys/%1").arg(QString::fromStdString(handle)), true); },
    });
    redvault::app::AppController controller(apiBase, &tor);
    redvault::app::AppController::setInstance(&controller);
    redvault::app::DesktopShell shell(&controller);
    redvault::app::DesktopShell::setInstance(&shell);
    QObject::connect(&controller, &redvault::app::AppController::incomingMessages, &shell,
                     &redvault::app::DesktopShell::notifyIncoming);
    // Says only that something arrived: no sender, chat or text reaches the operating system's notification centre.
    QObject::connect(&controller, &redvault::app::AppController::deviceKeyMessages, &shell, &redvault::app::DesktopShell::notifyDeviceKeyMessages);
    QObject::connect(&controller, &redvault::app::AppController::incomingCall, &shell,
                     &redvault::app::DesktopShell::notifyIncomingCall);
    QObject::connect(&controller, &redvault::app::AppController::channelPostNotification, &shell,
                     &redvault::app::DesktopShell::notifyChannelPost);
    QObject::connect(&controller, &redvault::app::AppController::storageFull, &shell,
                     [&shell](const QString& message) { shell.notify(QStringLiteral("Storage full"), message); });
    QObject::connect(&controller, &redvault::app::AppController::shareIncoming, &shell, &redvault::app::DesktopShell::notifyShareIncoming);
    redvault::app::Updater updater(&controller);
    redvault::app::Updater::setInstance(&updater);
    QObject::connect(&updater, &redvault::app::Updater::updateFound, &shell, [&shell](const QString& version) {
        shell.notify(QStringLiteral("redvaultmessages %1 is available").arg(version),
                     QStringLiteral("Open redvaultmessages and choose Update to install it."));
    });
    QObject::connect(&updater, &redvault::app::Updater::manifestExpired, &shell, [&shell](const QString& warning) {
        shell.notify(QStringLiteral("Update check"), warning);
    });

    // The design's typefaces (SIL Open Font License, in fonts/): IBM Plex Sans
    // for text, IBM Plex Mono for anything exact, Bricolage Grotesque for titles.
    for (const char* font : {"ibm-plex-sans-latin-400-normal", "ibm-plex-sans-latin-500-normal", "ibm-plex-sans-latin-600-normal",
                             "ibm-plex-mono-latin-400-normal", "ibm-plex-mono-latin-500-normal", "bricolage-grotesque-latin-700-normal"}) {
        QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/Redvault/fonts/%1.ttf").arg(QLatin1String(font)));
    }
    // Every Text, not only controls, uses it.
    QFont uiFont(QStringLiteral("IBM Plex Sans"));
    uiFont.setPixelSize(14);
    QGuiApplication::setFont(uiFont);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("avatar"), new redvault::app::AvatarImageProvider(&controller));
    engine.addImageProvider(QStringLiteral("channelavatar"),
                            new redvault::app::AvatarImageProvider(&controller, redvault::app::AvatarImageProvider::Kind::Channel));
    engine.addImageProvider(QStringLiteral("channelmedia"), new redvault::app::ChannelMediaImageProvider(&controller));
    engine.addImageProvider(QStringLiteral("community"), new redvault::app::CommunityImageProvider(&controller));
    engine.addImageProvider(QStringLiteral("qr"), new redvault::app::QrImageProvider);
    engine.addImageProvider(QStringLiteral("icon"), new redvault::app::IconImageProvider);
    engine.addImageProvider(QStringLiteral("chatimage"), new redvault::app::ChatImageProvider);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("Redvault", "Main");
    return QApplication::exec();
}
