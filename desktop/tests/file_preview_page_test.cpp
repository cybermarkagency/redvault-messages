// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The Files viewer's real QML (FilePreviewDialog.qml, PdfPreview.qml) showing
// real files of each kind, over a stand-in FilesSession. Runs on Qt's
// offscreen platform. REDVAULT_PREVIEW_SHOTS (an env var naming a folder)
// saves a picture of each, for a person to look at.

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
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

/// The app's QML files as a module, FilesSession replaced by its stand-in.
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
        add(QStringLiteral(REDVAULT_FILES_QML_STUBS));
        QFile index(out + QStringLiteral("qmldir"));
        REQUIRE(index.open(QIODevice::WriteOnly));
        index.write(qmldir.toUtf8());
    }
    QTemporaryDir dir;
};

struct Viewer {
    Viewer() {
        qt();
        engine.addImportPath(module.dir.path());
        engine.addImageProvider(QStringLiteral("icon"), new NoIcons);
        window.resize(1100, 760);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport Redvault\nItem {\n"
                          "  property alias files: f\n  property alias dialog: d\n"
                          "  FilesSession { id: f }\n  FilePreviewDialog { id: d; files: f }\n}",
                          QUrl::fromLocalFile(module.dir.path() + QStringLiteral("/Redvault/Harness.qml")));
        root.reset(qobject_cast<QQuickItem*>(component.create()));
        INFO(component.errorString().toStdString());
        REQUIRE(root);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(1100, 760));
        window.show();
        files = root->property("files").value<QObject*>();
        dialog = root->property("dialog").value<QObject*>();
        REQUIRE(files);
        REQUIRE(dialog);
    }
    ~Viewer() {
        root.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void show(const QVariantMap& viewer) {
        files->setProperty("viewer", viewer);
        QMetaObject::invokeMethod(dialog, "open");
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
        settle(300);
    }
    /// A visible item by objectName, anywhere in the window's item tree (the popup sits in the overlay).
    QQuickItem* named(const char* name) const { return find(window.contentItem(), QLatin1String(name)); }
    static QQuickItem* find(QQuickItem* item, QLatin1String name) {
        if (item == nullptr || !item->isVisible()) return nullptr;
        if (item->objectName() == name) return item;
        for (auto* child : item->childItems()) {
            if (auto* found = find(child, name)) return found;
        }
        return nullptr;
    }
    void shoot(const char* name) {
        const QByteArray folder = qgetenv("REDVAULT_PREVIEW_SHOTS");
        if (folder.isEmpty()) return;
        settle(200);
        window.grabWindow().save(QString::fromLocal8Bit(folder) + QStringLiteral("/") + QLatin1String(name) + QStringLiteral(".png"));
    }

    struct Started {
        Started() { qt(); }  // the application before the engine
    } started;
    Module module;
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QQuickItem> root;
    QObject* files = nullptr;
    QObject* dialog = nullptr;
};

QString write(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    QFile f(dir.filePath(name));
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    return QUrl::fromLocalFile(f.fileName()).toString();
}

QVariantMap ready(const QString& kind, const QString& name) {
    return {{"id", "e1"}, {"name", name}, {"kind", kind}, {"state", "ready"}, {"progress", 1.0}, {"sizeText", "12 KB"}};
}

}  // namespace

TEST_CASE("Files viewer: a picture fits the window", "[file_preview_page]") {
    Viewer v;
    QTemporaryDir dir;
    QImage picture(800, 500, QImage::Format_RGB32);
    picture.fill(QColor("#9CC9E8"));
    QPainter(&picture).fillRect(100, 100, 300, 200, QColor("#A72640"));
    REQUIRE(picture.save(dir.filePath("photo.png")));
    auto viewer = ready("image", "photo.png");
    viewer["url"] = QUrl::fromLocalFile(dir.filePath("photo.png")).toString();
    v.show(viewer);
    auto* image = v.named("previewImage");
    REQUIRE(image);
    REQUIRE(waitFor([&] { return image->property("status").toInt() == 1; }, 5000));  // AnimatedImage.Ready
    CHECK(image->property("sourceSize").toSize() == QSize(800, 500));
    v.shoot("image");
}

TEST_CASE("Files viewer: text is shown as plain text", "[file_preview_page]") {
    Viewer v;
    auto viewer = ready("text", "notes.txt");
    viewer["text"] = QStringLiteral("<b>not bold</b>\nSecond line");
    v.show(viewer);
    auto* text = v.named("previewText");
    REQUIRE(text);
    CHECK(text->property("text").toString() == QStringLiteral("<b>not bold</b>\nSecond line"));
    v.shoot("text");
}

TEST_CASE("Files viewer: JSON and CSV", "[file_preview_page]") {
    Viewer v;
    auto json = ready("json", "data.json");
    json["text"] = QStringLiteral("{\n    \"name\": \"RedVault\",\n    \"tags\": [\n        \"a\",\n        \"b\"\n    ]\n}\n");
    v.show(json);
    REQUIRE(v.named("previewText"));
    v.shoot("json");
    QMetaObject::invokeMethod(v.dialog, "close");
    settle(300);

    auto csv = ready("csv", "table.csv");
    csv["rows"] = QVariantList{QStringList{"Name", "City", "Amount"}, QStringList{"Jane Doe", "Manila", "1,200"}, QStringList{"Sam", "Cebu", "75"}};
    v.show(csv);
    auto* rows = v.named("previewCsv");
    REQUIRE(rows);
    CHECK(rows->property("count").toInt() == 3);
    v.shoot("csv");
}

TEST_CASE("Files viewer: audio gets the player", "[file_preview_page]") {
    Viewer v;
    QTemporaryDir dir;
    // A second of silence: 8 kHz, 8-bit mono WAV.
    QByteArray wav("RIFF");
    const auto le32 = [](quint32 n) { return QByteArray(reinterpret_cast<const char*>(&n), 4); };
    const auto le16 = [](quint16 n) { return QByteArray(reinterpret_cast<const char*>(&n), 2); };
    wav += le32(36 + 8000) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(8000) + le32(8000) + le16(1) + le16(8) + "data" + le32(8000) +
           QByteArray(8000, char(0x80));
    auto viewer = ready("audio", "song.wav");
    viewer["url"] = write(dir, "song.wav", wav);
    v.show(viewer);
    REQUIRE(v.named("previewPlay"));
    v.shoot("audio");
}

TEST_CASE("Files viewer: an error offers Download", "[file_preview_page]") {
    Viewer v;
    QVariantMap viewer{{"id", "e1"}, {"name", "huge.mp4"}, {"kind", "video"}, {"state", "error"}, {"message", "This file is too big to preview here."}};
    v.show(viewer);
    auto* error = v.named("previewError");
    REQUIRE(error);
    CHECK(error->property("text").toString() == QStringLiteral("This file is too big to preview here."));
    v.shoot("error");
}

TEST_CASE("Files viewer: closing tells the session (it deletes the copy)", "[file_preview_page]") {
    Viewer v;
    auto viewer = ready("text", "notes.txt");
    viewer["text"] = QStringLiteral("hi");
    v.show(viewer);
    QMetaObject::invokeMethod(v.dialog, "close");
    REQUIRE(waitFor([&] { return v.files->property("closes").toInt() == 1; }, 5000));
}

#ifdef REDVAULT_HAS_PDF
TEST_CASE("Files viewer: a PDF, page by page", "[file_preview_page]") {
    Viewer v;
    QTemporaryDir dir;
    {
        QPdfWriter pdf(dir.filePath("paper.pdf"));
        QPainter painter(&pdf);
        QFont font = painter.font();
        font.setPointSize(28);
        painter.setFont(font);
        painter.drawText(QRect(0, 0, pdf.width(), pdf.height() / 3), Qt::AlignCenter, QStringLiteral("Page one"));
        pdf.newPage();
        painter.drawText(QRect(0, 0, pdf.width(), pdf.height() / 3), Qt::AlignCenter, QStringLiteral("Page two"));
    }
    auto viewer = ready("pdf", "paper.pdf");
    viewer["url"] = QUrl::fromLocalFile(dir.filePath("paper.pdf")).toString();
    v.show(viewer);
    QQuickItem* pages = nullptr;
    REQUIRE(waitFor([&] { return (pages = v.named("pdfPages")) != nullptr && pages->property("count").toInt() == 2; }, 10000));
    settle(800);
    INFO("contentY " << pages->property("contentY").toDouble() << " contentHeight " << pages->property("contentHeight").toDouble()
                     << " height " << pages->height());
    CHECK(pages->property("contentY").toDouble() == pages->property("originY").toDouble());  // opens at the first page    v.shoot("pdf");
}
#endif
