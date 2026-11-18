// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// A manual live check (not part of ctest: it needs the internet and takes minutes): the app's real TorController
// starts Tor, switches to bridges through setBridges (what Settings › Network routing's Save button calls), waits
// until Tor is connected through them, then fetches the service's public routing info from its onion address
// through the app's real HTTP transport and Tor gate.
//
//   redvault_tor_bridges_live snowflake|obfs4|off|FILE-WITH-BRIDGE-LINES [ONION_HOST]
//
// Prints one line per state change and exits 0 only if the onion answered.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>

#include "redvault/net/curl_transport.hpp"
#include "tor_controller.hpp"

using redvault::app::TorController;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Redvault Live Test"));
    QCoreApplication::setApplicationName(QStringLiteral("TorBridgesLive"));
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s snowflake|obfs4|off|FILE [ONION_HOST]\n", argv[0]);
        return 2;
    }
    QString mode = QString::fromLocal8Bit(argv[1]);
    QString lines;
    if (mode != QStringLiteral("snowflake") && mode != QStringLiteral("obfs4") && mode != QStringLiteral("off")) {
        QFile file(mode);
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "can't read %s\n", argv[1]);
            return 2;
        }
        lines = QString::fromUtf8(file.readAll());
        mode = QStringLiteral("custom");
    }
    const QString onion = argc > 2 ? QString::fromLocal8Bit(argv[2])
                                   : QStringLiteral("4cydjhdc3lmhg7l6ykjont7dms2alunglmnlznlmjjwkyc67m5h325yd.onion");
    QTemporaryDir state;
    QElapsedTimer clock;
    clock.start();
    TorController tor(onion, state.path());
    std::printf("bridges available: %s\n", tor.bridgesAvailable() ? "yes" : "no");
    QObject::connect(&tor, &TorController::changed, [&] {
        std::printf("%5llds %s %d%% %s\n", static_cast<long long>(clock.elapsed() / 1000), qPrintable(tor.state()), tor.progress(),
                    qPrintable(tor.detail()));
        std::fflush(stdout);
    });

    const QString problem = tor.setBridges(mode, lines);
    std::printf("setBridges(%s) -> %s (mode now %s)\n", qPrintable(mode), problem.isEmpty() ? "saved" : qPrintable(problem),
                qPrintable(tor.bridgeMode()));
    std::fflush(stdout);
    if (!problem.isEmpty()) return 1;
    // Saved where the app keeps it, so the next start would use it too.
    std::printf("kept: network/bridgeMode=%s\n", qPrintable(QSettings().value(QStringLiteral("network/bridgeMode")).toString()));

    while (!tor.connected() && clock.elapsed() < 600'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    if (!tor.connected()) {
        std::printf("never connected\n");
        return 1;
    }

    redvault::net::CurlTransport::Options options;
    options.allowedHost = onion.toStdString();
    options.torGate = tor.gate();
    options.connectTimeout = std::chrono::seconds(90);
    redvault::net::CurlTransport http(options);
    for (int attempt = 1; attempt <= 4; ++attempt) {
        try {
            const auto response = http.send({"GET", "http://" + onion.toStdString() + "/redvaultmessages/api/public/routing", {}, {}});
            std::printf("%5llds GET public/routing through %s -> HTTP %d (%zu bytes, attempt %d)\n",
                        static_cast<long long>(clock.elapsed() / 1000), qPrintable(tor.bridgeMode()), response.status,
                        response.body.size(), attempt);
            tor.stop();
            return response.status > 0 ? 0 : 1;
        } catch (const std::exception& e) {
            std::printf("attempt %d failed: %s\n", attempt, e.what());
            std::fflush(stdout);
            QThread::sleep(5);
        }
    }
    tor.stop();
    return 1;
}
