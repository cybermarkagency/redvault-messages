#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <memory>

namespace redvault::test {

/// One QGuiApplication for the process, and settings that can never be the real ones.
struct Application {
    Application() {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QUICK_BACKEND", "software");
        // ctest runs every case as its own process, all at once: they mustn't share a compiled-QML cache.
        qputenv("QML_DISABLE_DISK_CACHE", "1");
        static int argc = 1;
        static char name[] = "redvault_app_tests";
        static char* argv[] = {name, nullptr};
        // Never destroyed: taking Qt down after main() returns, once QML has been loaded, crashes at exit.
        new QGuiApplication(argc, argv);
        QCoreApplication::setOrganizationName(QStringLiteral("Redvault Test"));
        QCoreApplication::setApplicationName(QStringLiteral("AppLockTest"));
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, folder.path());
    }
    QTemporaryDir folder;
};

inline Application& qt() {
    static Application instance;
    return instance;
}

inline bool waitFor(const std::function<bool()>& done, int timeoutMs = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

/// Lets queued work finish when the point is that nothing happens.
inline void settle(int ms = 250) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

}  // namespace redvault::test
