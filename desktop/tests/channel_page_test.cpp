// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The channel page in its real QML (app/qml) and over the real ChannelPostModel, with stand-ins for the C++ session
// types (tests/qml_channel_stubs). Nothing here is a person clicking; what it pins is what must never drift: a reader
// has no composer, a post looks like the channel's and claims no encryption, and the thread runs as a chat does.
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrl>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_test_support.hpp"
#include "channel_post_model.hpp"

using redvault::app::ChannelPostModel;
using redvault::test::Application;
using redvault::test::qt;
using redvault::test::settle;
using redvault::test::waitFor;
namespace channels = redvault::api::channels;

namespace {

/// `daysAgo` days before today, at noon (local), so tests can't straddle midnight.
std::string iso(int daysAgo, int minute = 0) {
    QDateTime at(QDate::currentDate().addDays(-daysAgo), QTime(12, minute));
    return at.toUTC().toString(Qt::ISODateWithMs).toStdString();
}

channels::Post textPost(const std::string& id, const std::string& body, int daysAgo, int minute = 0) {
    channels::Post post;
    post.id = id;
    post.body = body;
    post.createdAt = iso(daysAgo, minute);
    post.views = 7;
    return post;
}

ChannelPostModel::VoteLookup noVotes() {
    return [](const std::string&) { return std::nullopt; };
}

/// Icons aren't what's under test: a blank one for each.
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

/// The app's QML files as a module, with stand-ins replacing the types that are C++ in the app.
struct Module {
    Module() {
        REQUIRE(dir.isValid());
        REQUIRE(QDir(dir.path()).mkpath(QStringLiteral("Redvault")));
        const QString out = dir.path() + QStringLiteral("/Redvault/");
        QString qmldir = QStringLiteral("module Redvault\nsingleton Theme 1.0 Theme.qml\nsingleton App 1.0 App.qml\n");
        const auto add = [&](const QString& folder) {
            for (const QFileInfo& file : QDir(folder).entryInfoList({QStringLiteral("*.qml"), QStringLiteral("*.js")}, QDir::Files)) {
                QFile::remove(out + file.fileName());  // a stand-in replaces the real file
                REQUIRE(QFile::copy(file.absoluteFilePath(), out + file.fileName()));
                if (file.suffix() != QStringLiteral("qml") || file.baseName() == QStringLiteral("Theme") || file.baseName() == QStringLiteral("App")) continue;
                qmldir += file.baseName() + QStringLiteral(" 1.0 ") + file.fileName() + u'\n';
            }
        };
        add(QStringLiteral(REDVAULT_QML_DIR));
        add(QStringLiteral(REDVAULT_CHANNEL_QML_STUBS));
        qmldir += QStringLiteral("ChannelPostModel 1.0 ChannelUploads.qml\n");
        QFile index(out + QStringLiteral("qmldir"));
        REQUIRE(index.open(QIODevice::WriteOnly));
        index.write(qmldir.toUtf8());
    }
    QTemporaryDir dir;
};

struct Page {
    explicit Page(bool owner, int width = 1000, int height = 700) : posts(noVotes()) {
        engine.addImportPath(module.dir.path());
        engine.addImageProvider(QStringLiteral("icon"), new NoIcons);
        QQmlComponent stub(&engine);
        stub.setData("import Redvault\nChannelsSession {}", QUrl());
        INFO(stub.errorString().toStdString());
        session.reset(stub.create());
        REQUIRE(session);
        session->setProperty("posts", QVariant::fromValue(static_cast<QObject*>(&posts)));
        session->setProperty("isMine", owner);

        window.resize(width, height);
        QQmlComponent component(&engine, QUrl::fromLocalFile(module.dir.path() + QStringLiteral("/Redvault/ChannelPage.qml")));
        INFO(component.errorString().toStdString());
        root.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties({{QStringLiteral("channels"), QVariant::fromValue(session.get())}})));
        INFO(component.errorString().toStdString());
        REQUIRE(root);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(width, height));
        window.show();
    }
    ~Page() {
        root.reset();
        // Delegates a list dropped: gone now, while what they refer to still exists, rather than as the process exits.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    QQuickItem* named(const char* name) const { return root->findChild<QQuickItem*>(QString::fromLatin1(name)); }
    QQuickItem* thread() const { return named("channelThread"); }
    void layOut() {
        settle(100);
        QMetaObject::invokeMethod(thread(), "forceLayout");
        settle(50);
    }
    /// Everything drawn on the page: a list's delegates aren't QObject children of it, only visual ones.
    std::vector<QQuickItem*> shown() const {
        std::vector<QQuickItem*> out;
        const std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
            if (!item->isVisible()) return;
            out.push_back(item);
            for (QQuickItem* child : item->childItems()) walk(child);
        };
        walk(root.get());
        return out;
    }
    /// The scene y of the bubble showing this text, or a negative number when none does.
    qreal bubbleY(const QString& text) const {
        const auto items = shown();
        for (QQuickItem* item : items) {
            if (item->objectName() != QStringLiteral("channelPostBubble")) continue;
            for (QQuickItem* inside : items) {
                // The body is rich text: its markup holds the words.
                if (item->isAncestorOf(inside) && inside->metaObject()->indexOfProperty("selectByMouse") >= 0 && inside->property("text").toString().contains(text)) {
                    return item->mapToScene(QPointF(0, 0)).y();
                }
            }
        }
        return -1;
    }
    /// Every piece of text on the page.
    QStringList texts() const {
        QStringList out;
        for (QQuickItem* item : shown()) {
            const QString text = item->property("text").toString();
            if (!text.isEmpty()) out << text;
        }
        return out;
    }

    Application& application = qt();  // before the engine
    Module module;
    QQmlEngine engine;
    ChannelPostModel posts;
    QQuickWindow window;
    std::unique_ptr<QObject> session;
    std::unique_ptr<QQuickItem> root;
};

/// A text box or field, by what QML's text input types have and nothing else does.
bool isTextInput(QObject* object) {
    return object->metaObject()->indexOfProperty("cursorPosition") >= 0 && !object->property("readOnly").toBool();
}

}  // namespace

TEST_CASE("a reader's channel page has no composer and nothing to type in", "[app][channelpage]") {
    Page p(false);
    p.posts.reset({textPost("a", "Stalls open at six.", 0)});
    settle(100);
    CHECK(p.named("channelComposer") == nullptr);
    CHECK(p.named("channelPostInput") == nullptr);
    CHECK(p.named("channelSendButton") == nullptr);
    CHECK(p.named("channelAttachButton") == nullptr);
    CHECK(p.named("channelOwnerExtras") == nullptr);
    QQuickItem* bar = p.named("channelReaderBar");
    REQUIRE(bar);
    CHECK(bar->isVisible());
    // Nothing that's on screen takes typing: the suggest dialog's form only shows once asked for, and the info panel is closed.
    for (QQuickItem* item : p.shown()) {
        if (isTextInput(item)) FAIL("a text box is showing: " << item->metaObject()->className());
    }
    CHECK(p.texts().contains(QStringLiteral("Only the owner posts here.")));
    REQUIRE(p.named("channelSuggestButton"));
    CHECK(p.named("channelSuggestButton")->isVisible());
    // A reader still follows from the bar.
    QQuickItem* follow = p.named("channelFollowButton");
    REQUIRE(follow);
    CHECK(follow->property("text").toString() == QStringLiteral("Follow"));
    QMetaObject::invokeMethod(follow, "clicked");
    CHECK(p.session->property("follows").toInt() == 1);
    CHECK(follow->property("text").toString() == QStringLiteral("Following"));
}

TEST_CASE("a reader isn't offered suggesting where the channel doesn't take suggestions", "[app][channelpage]") {
    Page p(false);
    QVariantMap channel = p.session->property("channel").toMap();
    channel[QStringLiteral("acceptSuggestions")] = false;
    p.session->setProperty("channel", channel);
    settle(50);
    REQUIRE(p.named("channelSuggestButton"));
    CHECK_FALSE(p.named("channelSuggestButton")->isVisible());
}

TEST_CASE("control: the owner's channel page has the composer, and no reader bar", "[app][channelpage]") {
    Page p(true);
    REQUIRE(waitFor([&] { return p.named("channelComposer") != nullptr; }));
    CHECK(p.named("channelComposer")->isVisible());
    REQUIRE(p.named("channelPostInput"));
    REQUIRE(p.named("channelAttachButton"));
    REQUIRE(p.named("channelSendButton"));
    CHECK_FALSE(p.named("channelSendButton")->isEnabled());  // nothing to post yet
    REQUIRE(p.named("channelReaderBar"));
    CHECK_FALSE(p.named("channelReaderBar")->isVisible());
    // It posts what's typed, and clears.
    p.named("channelPostInput")->setProperty("text", QStringLiteral("Doors at six"));
    CHECK(p.named("channelSendButton")->isEnabled());
    QMetaObject::invokeMethod(p.named("channelComposer"), "submit");
    CHECK(p.session->property("lastBody").toString() == QStringLiteral("Doors at six"));
    CHECK(p.named("channelPostInput")->property("text").toString().isEmpty());
}

TEST_CASE("the composer still schedules, and carries a poll", "[app][channelpage]") {
    Page p(true);
    REQUIRE(waitFor([&] { return p.named("channelComposer") != nullptr; }));
    QQuickItem* composer = p.named("channelComposer");
    QMetaObject::invokeMethod(composer, "chooseExtra", Q_ARG(QVariant, QStringLiteral("poll")));
    CHECK(composer->property("panelOpen").toBool());
    composer->setProperty("options", QStringList{QStringLiteral("Yes"), QStringLiteral("No")});
    QMetaObject::invokeMethod(composer, "submit");
    const QVariantMap extra = p.session->property("lastExtra").toMap();
    CHECK(extra.value(QStringLiteral("type")).toString() == QStringLiteral("poll"));
    CHECK(extra.value(QStringLiteral("options")).toStringList() == QStringList{QStringLiteral("Yes"), QStringLiteral("No")});

    QMetaObject::invokeMethod(composer, "toggleSchedule");
    CHECK(composer->property("scheduling").toBool());
    p.named("channelPostInput")->setProperty("text", QStringLiteral("Later"));
    QMetaObject::invokeMethod(composer, "submit");
    CHECK_FALSE(p.session->property("lastPublishAt").toString().isEmpty());
}

TEST_CASE("the header says what a channel is, and a post claims no encryption", "[app][channelpage]") {
    Page p(false);
    p.posts.reset({textPost("a", "Stalls open at six.", 0)});
    p.layOut();
    QQuickItem* subtitle = p.named("channelSubtitle");
    REQUIRE(subtitle);
    CHECK(subtitle->property("text").toString() == QStringLiteral("Public channel · not end-to-end encrypted"));
    QVariantMap channel = p.session->property("channel").toMap();
    channel[QStringLiteral("isPrivate")] = true;
    p.session->setProperty("channel", channel);
    CHECK(subtitle->property("text").toString() == QStringLiteral("Private channel · not end-to-end encrypted"));

    // The only "encrypted" on the page is that negative disclosure.
    p.layOut();
    for (const QString& text : p.texts()) {
        if (text.contains(QStringLiteral("ncrypt"), Qt::CaseInsensitive)) CHECK(text.contains(QStringLiteral("not end-to-end encrypted")));
    }
    // No lock, safety or delivery icon anywhere on a post.
    REQUIRE(p.bubbleY(QStringLiteral("Stalls open at six.")) >= 0);
    bool footerSeen = false;
    for (QQuickItem* item : p.shown()) {
        const QString name = item->property("name").toString();
        CHECK_FALSE(name.contains(QStringLiteral("lock")));
        CHECK_FALSE(name.contains(QStringLiteral("shield")));
        CHECK_FALSE(name.contains(QStringLiteral("check-check")));
        // The footer: the time and the views.
        if (item->objectName() == QStringLiteral("channelPostFooter")) {
            footerSeen = true;
            CHECK(item->property("text").toString().endsWith(QStringLiteral(" · 7 views")));
        }
    }
    CHECK(footerSeen);
}

TEST_CASE("the thread runs as a chat: oldest on top, newest at the bottom, with a header per day", "[app][channelpage]") {
    Page p(false);
    p.posts.reset({textPost("new", "Today's post", 0), textPost("old", "Yesterday's post", 1)});
    p.layOut();
    const qreal older = p.bubbleY(QStringLiteral("Yesterday's post"));
    const qreal newer = p.bubbleY(QStringLiteral("Today's post"));
    REQUIRE(older >= 0);
    REQUIRE(newer >= 0);
    CHECK(older < newer);
    // A short thread sits on the composer's side of the page, as the end of a chat does.
    CHECK(newer > 300);

    const QStringList texts = p.texts();
    CHECK(texts.contains(QStringLiteral("Today")));
    CHECK(texts.contains(QStringLiteral("Yesterday")));

    // A new post lands at the bottom, under the others.
    p.posts.prepend(textPost("newest", "Just now", 0, 30));
    p.layOut();
    CHECK(p.bubbleY(QStringLiteral("Just now")) > p.bubbleY(QStringLiteral("Today's post")));
}

TEST_CASE("Show older posts is at the top, and older posts don't move what's on screen", "[app][channelpage]") {
    Page p(false, 1000, 500);
    std::vector<channels::Post> page;
    for (int i = 0; i < 12; ++i) page.push_back(textPost("p" + std::to_string(i), "Post " + std::to_string(i), 0, 59 - i));
    p.posts.reset(page);
    p.session->setProperty("hasOlder", true);
    p.layOut();
    // Opens on the newest: it's on screen, in the thread's part of the page.
    QQuickItem* thread = p.thread();
    const qreal newest = p.bubbleY(QStringLiteral("Post 0"));
    REQUIRE(newest >= 0);
    CHECK(newest < thread->mapToScene(QPointF(0, thread->height())).y());
    CHECK(newest > thread->mapToScene(QPointF(0, 0)).y());

    // Scrolled to the top, the button's there; older posts arrive above it.
    QMetaObject::invokeMethod(thread, "positionViewAtEnd");
    p.layOut();
    QQuickItem* button = p.named("channelOlderButton");
    REQUIRE(button);
    CHECK(button->isVisible());
    QMetaObject::invokeMethod(button, "clicked");
    CHECK(p.session->property("olderLoads").toInt() == 1);

    const qreal topPost = p.bubbleY(QStringLiteral("Post 11"));
    REQUIRE(topPost >= 0);
    std::vector<channels::Post> older;
    for (int i = 0; i < 12; ++i) older.push_back(textPost("q" + std::to_string(i), "Older " + std::to_string(i), 1, 59 - i));
    p.posts.append(older);
    p.layOut();
    CHECK(p.bubbleY(QStringLiteral("Post 11")) == topPost);

    // Nor does a new post, for someone reading further up.
    p.posts.prepend(textPost("new", "Fresh post", 0, 59));
    p.layOut();
    CHECK(p.bubbleY(QStringLiteral("Post 11")) == topPost);
}

TEST_CASE("a post's day header follows its neighbours", "[app][channelpage]") {
    qt();
    ChannelPostModel model(noVotes());
    const auto day = [&](int row) { return model.data(model.index(row), ChannelPostModel::DayHeaderRole).toString(); };

    model.reset({textPost("c", "c", 0, 2), textPost("b", "b", 0, 1)});
    CHECK(day(1) == QStringLiteral("Today"));  // the oldest loaded starts the day
    CHECK(day(0).isEmpty());

    // An older page arrives from the same day: the header moves up to its oldest post.
    QStringList changed;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&](const QModelIndex& at) { changed << model.data(at, ChannelPostModel::IdRole).toString(); });
    model.append({textPost("a", "a", 0, 0), textPost("y", "y", 1)});
    CHECK(changed == QStringList{QStringLiteral("b")});
    CHECK(day(1).isEmpty());
    CHECK(day(2) == QStringLiteral("Today"));
    CHECK(day(3) == QStringLiteral("Yesterday"));

    // Deleting the post that started a day hands the header to the next one.
    changed.clear();
    model.remove("a");
    CHECK(changed == QStringList{QStringLiteral("b")});
    CHECK(day(1) == QStringLiteral("Today"));
}

// --- The links paid communities and payouts open ---
