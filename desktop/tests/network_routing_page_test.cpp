// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Settings › Network routing's real QML (NetworkRoutingSection.qml), the Bridges part, over a stand-in
// TorController. Runs on Qt's offscreen platform. REDVAULT_PREVIEW_SHOTS (an env var naming a folder) saves a
// picture of each state, for a person to look at.

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>

#include "app_test_support.hpp"

using redvault::test::qt;
using redvault::test::settle;
using redvault::test::waitFor;

namespace {

class NoIcons : public QQuickImageProvider {
public:
    NoIcons() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        if (size != nullptr) *size = image.size();
        return image;
    }
};

/// The app's QML files as a module, TorController and App replaced by stand-ins.
struct Module {
    Module() {
        REQUIRE(dir.isValid());
        REQUIRE(QDir(dir.path()).mkpath(QStringLiteral("Redvault")));
        const QString out = dir.path() + QStringLiteral("/Redvault/");
        QString qmldir = QStringLiteral("module Redvault\nsingleton Theme 1.0 Theme.qml\nsingleton App 1.0 App.qml\n");
        const auto add = [&](const QString& folder) {
            for (const QFileInfo& file : QDir(folder).entryInfoList({QStringLiteral("*.qml"), QStringLiteral("*.js")}, QDir::Files)) {
                QFile::remove(out + file.fileName());
                REQUIRE(QFile::copy(file.absoluteFilePath(), out + file.fileName()));
                if (file.suffix() != QStringLiteral("qml") || file.baseName() == QStringLiteral("Theme") || file.baseName() == QStringLiteral("App")) continue;
                qmldir += file.baseName() + QStringLiteral(" 1.0 ") + file.fileName() + u'\n';
            }
        };
        add(QStringLiteral(REDVAULT_QML_DIR));
        add(QStringLiteral(REDVAULT_NETWORK_QML_STUBS));
        QFile index(out + QStringLiteral("qmldir"));
        REQUIRE(index.open(QIODevice::WriteOnly));
        index.write(qmldir.toUtf8());
    }
    QTemporaryDir dir;
};

struct Screen {
    Screen() {
        qt();
        engine.addImportPath(module.dir.path());
        engine.addImageProvider(QStringLiteral("icon"), new NoIcons);
        window.resize(760, 2000);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport Redvault\nRectangle {\n  color: Theme.bg\n"
                          "  property alias tor: t\n  TorController { id: t }\n"
                          "  NetworkRoutingSection { x: 24; y: 16; width: parent.width - 48; tor: t }\n}",
                          QUrl::fromLocalFile(module.dir.path() + QStringLiteral("/Redvault/Harness.qml")));
        root.reset(qobject_cast<QQuickItem*>(component.create()));
        INFO(component.errorString().toStdString());
        REQUIRE(root);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(760, 2000));
        window.show();
        tor = root->property("tor").value<QObject*>();
        REQUIRE(tor);
        settle(200);
    }
    ~Screen() {
        root.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    QQuickItem* named(const char* name) const { return find(window.contentItem(), QLatin1String(name)); }
    static QQuickItem* find(QQuickItem* item, QLatin1String name) {
        if (item == nullptr || !item->isVisible()) return nullptr;
        if (item->objectName() == name) return item;
        for (auto* child : item->childItems()) {
            if (auto* found = find(child, name)) return found;
        }
        return nullptr;
    }
    /// Any visible Text showing exactly or containing `text`.
    bool shows(const QString& text) const { return showsIn(window.contentItem(), text); }
    static bool showsIn(QQuickItem* item, const QString& text) {
        if (item == nullptr || !item->isVisible()) return false;
        if (item->property("text").toString().contains(text)) return true;
        for (auto* child : item->childItems()) {
            if (showsIn(child, text)) return true;
        }
        return false;
    }
    void choose(int index) {
        auto* choice = named("bridgeMode");
        REQUIRE(choice);
        choice->setProperty("currentIndex", index);
        QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, index));
        settle(100);
    }
    void save() {
        auto* button = named("saveBridges");
        REQUIRE(button);
        REQUIRE(button->property("enabled").toBool());
        QMetaObject::invokeMethod(button, "clicked");
        settle(100);
    }
    QVariantList calls() const { return tor->property("calls").toList(); }
    void shoot(const char* name) {
        const QByteArray folder = qgetenv("REDVAULT_PREVIEW_SHOTS");
        if (folder.isEmpty()) return;
        settle(200);
        window.grabWindow().copy(0, 0, 760, 1500).save(QString::fromLocal8Bit(folder) + QStringLiteral("/") + QLatin1String(name) + QStringLiteral(".png"));
    }

    struct Started {
        Started() { qt(); }
    } started;
    Module module;
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QQuickItem> root;
    QObject* tor = nullptr;
};

}  // namespace

TEST_CASE("Bridges: off by default, Save does nothing until something changes", "[network_routing_page]") {
    Screen s;
    REQUIRE(s.shows(QStringLiteral("BRIDGES")));
    REQUIRE(s.named("bridgeMode"));
    CHECK(s.named("bridgeMode")->property("currentIndex").toInt() == 0);
    CHECK_FALSE(s.named("bridgeLines"));  // only for "Bridges I enter myself"
    CHECK_FALSE(s.named("saveBridges")->property("enabled").toBool());
    s.shoot("bridges-off");
}

TEST_CASE("Bridges: choosing Snowflake saves it and says Tor is reconnecting", "[network_routing_page]") {
    Screen s;
    s.choose(1);
    s.save();
    const auto calls = s.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].toList()[0].toString() == QStringLiteral("snowflake"));
    CHECK(s.tor->property("bridgeMode").toString() == QStringLiteral("snowflake"));
    CHECK(s.shows(QStringLiteral("Saved. Tor is reconnecting")));
    CHECK_FALSE(s.named("saveBridges")->property("enabled").toBool());  // nothing left to save
    s.shoot("bridges-snowflake-saved");
}

TEST_CASE("Bridges: pasted lines go to Tor, and its refusal is shown", "[network_routing_page]") {
    Screen s;
    s.choose(3);
    auto* lines = s.named("bridgeLines");
    REQUIRE(lines);
    CHECK(s.shows(QStringLiteral("bridges.torproject.org")));
    lines->setProperty("text", QStringLiteral("obfs4 not-an-address"));
    settle(50);
    s.tor->setProperty("nextError", QStringLiteral("bridge 1 isn't a valid bridge line"));
    s.save();
    CHECK(s.shows(QStringLiteral("bridge 1 isn't a valid bridge line")));
    CHECK(s.tor->property("bridgeMode").toString() == QStringLiteral("off"));  // nothing changed
    s.shoot("bridges-custom-refused");

    const QString good = QStringLiteral(
        "obfs4 192.0.2.55:38114 316E643333645F6D79216558614D3931657A5F5F cert=YXJlIGZyZXF1ZW50bHkgZnVsbCBvZiBsaXR0bGUgbWVzc2FnZXMgeW91IGNhbiBmaW5kLg iat-mode=0");
    lines->setProperty("text", good + QStringLiteral("\n"));
    s.tor->setProperty("nextError", QString());
    settle(50);
    s.save();
    const auto calls = s.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].toList()[0].toString() == QStringLiteral("custom"));
    CHECK(calls[1].toList()[1].toString().trimmed() == good);
    CHECK(s.tor->property("bridgeLines").toString() == good);
    CHECK_FALSE(s.shows(QStringLiteral("isn't a valid bridge line")));
    s.shoot("bridges-custom-saved");
}

TEST_CASE("Bridges: a copy without the programs says so and can't turn them on", "[network_routing_page]") {
    Screen s;
    s.tor->setProperty("bridgesAvailable", false);
    settle(100);
    CHECK(s.shows(QStringLiteral("doesn't include bridge support")));
    CHECK_FALSE(s.named("bridgeMode")->property("enabled").toBool());
    s.shoot("bridges-unavailable");
}

TEST_CASE("Bridges: when Tor can't connect, the warning suggests Snowflake", "[network_routing_page]") {
    Screen s;
    s.tor->setProperty("state", QStringLiteral("unavailable"));
    settle(100);
    CHECK(s.shows(QStringLiteral("try Snowflake under Bridges")));
    s.shoot("tor-blocked-hint");
    // Already on bridges: no point suggesting them.
    s.tor->setProperty("bridgeMode", QStringLiteral("snowflake"));
    settle(100);
    CHECK(s.shows(QStringLiteral("Tor connection unavailable")));
    CHECK_FALSE(s.shows(QStringLiteral("try Snowflake under Bridges")));
}
