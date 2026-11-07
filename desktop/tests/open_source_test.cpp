// Settings > About, "Open-Source Licenses and Source Code": the licence texts are built in (the same
// qt_add_resources files as the app, see tests/CMakeLists.txt) and the source link from the build is only ever
// shown as a plain https:// link. The configure-time check runs as its own ctest (source_settings_*).
#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QString>

#include "app_test_support.hpp"
#include "source_info.hpp"

namespace source_info = redvault::app::source_info;

namespace {

QString resourceText(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).remove(QLatin1Char('\r'));
}

}  // namespace

TEST_CASE("The AGPLv3 licence and the third-party notices are built in", "[open_source]") {
    redvault::test::qt();
    REQUIRE(QFile::exists(QStringLiteral(":/legal/LICENSE.txt")));
    REQUIRE(QFile::exists(QStringLiteral(":/legal/THIRD_PARTY_NOTICES.txt")));

    const QString license = resourceText(QStringLiteral(":/legal/LICENSE.txt"));
    CHECK(license.contains(QStringLiteral("GNU AFFERO GENERAL PUBLIC LICENSE")));
    CHECK(license.contains(QStringLiteral("Version 3, 19 November 2007")));

    const QString notices = resourceText(QStringLiteral(":/legal/THIRD_PARTY_NOTICES.txt"));
    CHECK(notices.contains(QStringLiteral("libsignal")));
    // The notices wrap their lines, so look for the phrase with the line breaks folded.
    CHECK(notices.left(4000).simplified().contains(
        QStringLiteral("not affiliated with, sponsored or endorsed by Signal Messenger LLC")));

    // What the dialog shows is the whole file, in pages.
    const QStringList pages = source_info::documentPages(QStringLiteral("license"));
    REQUIRE_FALSE(pages.isEmpty());
    CHECK(pages.join(QLatin1Char('\n')) == license);
    const QStringList noticePages = source_info::documentPages(QStringLiteral("notices"));
    CHECK(noticePages.size() > 1);
    CHECK(noticePages.join(QLatin1Char('\n')) == notices);

    // Only those two names: nothing else can be read from the resources through this.
    CHECK(source_info::documentPages(QStringLiteral("terms")).isEmpty());
    CHECK(source_info::documentPages(QStringLiteral(":/legal/LICENSE.txt")).isEmpty());
    CHECK(source_info::documentPages(QString()).isEmpty());
}

TEST_CASE("Long texts split into pages of whole lines", "[open_source]") {
    CHECK(source_info::pages(QString()).isEmpty());
    CHECK(source_info::pages(QStringLiteral("one"), 2) == QStringList{QStringLiteral("one")});
    const QString text = QStringLiteral("a\nb\nc\nd\ne");
    const QStringList pages = source_info::pages(text, 2);
    CHECK(pages == QStringList{QStringLiteral("a\nb"), QStringLiteral("c\nd"), QStringLiteral("e")});
    CHECK(pages.join(QLatin1Char('\n')) == text);
}

TEST_CASE("No source link until one is configured", "[open_source]") {
    CHECK(source_info::sourceCodeLink(QString(), QStringLiteral("0.6.0")).isEmpty());
    CHECK(source_info::sourceCodeLink(QStringLiteral("   "), QStringLiteral("0.6.0")).isEmpty());
}

TEST_CASE("An https source link is shown, with the version filled in", "[open_source]") {
    CHECK(source_info::sourceCodeLink(QStringLiteral("https://git.example.org/redvault"), QStringLiteral("0.6.0")) ==
          QStringLiteral("https://git.example.org/redvault"));
    CHECK(source_info::sourceCodeLink(QStringLiteral("https://git.example.org/redvault/-/tree/v{version}"),
                                      QStringLiteral("0.6.0")) ==
          QStringLiteral("https://git.example.org/redvault/-/tree/v0.6.0"));
}

TEST_CASE("Anything but a plain https link is never shown", "[open_source]") {
    const QString version = QStringLiteral("0.6.0");
    for (const char* bad : {"http://git.example.org/redvault", "ftp://git.example.org/redvault",
                            "javascript:alert(1)", "file:///C:/Windows", "https://", "https:///path",
                            "https://user:secret@git.example.org/", "git.example.org/redvault",
                            "HTTPS:/git.example.org"}) {
        INFO(bad);
        CHECK(source_info::sourceCodeLink(QString::fromLatin1(bad), version).isEmpty());
    }
}

TEST_CASE("The source contact is an email address or https link, else the Terms of Service", "[open_source]") {
    CHECK(source_info::sourceContact(QString()).isEmpty());
    CHECK(source_info::sourceContact(QStringLiteral("source@example.org")) == QStringLiteral("source@example.org"));
    CHECK(source_info::sourceContact(QStringLiteral("https://example.org/source")) ==
          QStringLiteral("https://example.org/source"));
    CHECK(source_info::sourceContact(QStringLiteral("http://example.org/source")).isEmpty());
    CHECK(source_info::sourceContact(QStringLiteral("not an address")).isEmpty());
}
