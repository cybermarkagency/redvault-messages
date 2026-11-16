// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QTemporaryDir>

#include "file_preview.hpp"

using namespace redvault::app::file_preview;

namespace {

std::filesystem::path writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    f.close();
    return std::filesystem::path(path.toStdWString());
}

}  // namespace

TEST_CASE("file preview: the viewer shows only the listed types", "[file_preview]") {
    CHECK(kindFor("Photo.JPG") == "image");
    CHECK(kindFor("a.jpeg") == "image");
    CHECK(kindFor("a.png") == "image");
    CHECK(kindFor("a.gif") == "image");
    CHECK(kindFor("a.webp") == "image");
    CHECK(kindFor("notes.txt") == "text");
    CHECK(kindFor("table.csv") == "csv");
    CHECK(kindFor("data.json") == "json");
    CHECK(kindFor("song.mp3") == "audio");
    CHECK(kindFor("song.wav") == "audio");
    CHECK(kindFor("song.flac") == "audio");
    CHECK(kindFor("clip.mp4") == "video");
    CHECK(kindFor("clip.MOV") == "video");
    CHECK(kindFor("paper.pdf") == "pdf");
    // Active or unsupported content never previews.
    for (const char* name : {"page.html", "logo.svg", "app.js", "setup.exe", "report.docx", "archive.zip", "movie.mkv", "noext", "pdf"}) {
        INFO(name);
        CHECK(kindFor(name).isEmpty());
    }
    CHECK(limitFor("text") == kMaxTextBytes);
    CHECK(limitFor("") == 0);
}

TEST_CASE("file preview: text must be UTF-8 without NUL bytes", "[file_preview]") {
    CHECK(isPlainText("hello\nworld"));
    CHECK(isPlainText(QByteArray("caf\xC3\xA9")));
    CHECK(isPlainText(QByteArray("\xEF\xBB\xBF" "bom")));
    CHECK_FALSE(isPlainText(QByteArray("a\0b", 3)));
    CHECK_FALSE(isPlainText(QByteArray("\xFF\xFE\x00\x41", 4)));
    CHECK_FALSE(isPlainText(QByteArray("bad \xC3")));
}

TEST_CASE("file preview: CSV cells, quotes and line ends", "[file_preview]") {
    bool cut = true;
    const auto rows = parseCsv("name,note\r\n\"Doe, Jane\",\"said \"\"hi\"\"\"\nx,\"two\nlines\"\n", 100, 10, &cut);
    CHECK_FALSE(cut);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == QStringList{"name", "note"});
    CHECK(rows[1] == QStringList{"Doe, Jane", "said \"hi\""});
    CHECK(rows[2] == QStringList{"x", "two\nlines"});

    const auto noTrailingBreak = parseCsv("a,b\n1,2", 100, 10);
    REQUIRE(noTrailingBreak.size() == 2);
    CHECK(noTrailingBreak[1] == QStringList{"1", "2"});

    const auto limited = parseCsv("1,2,3\n4,5,6\n7,8,9\n", 2, 2, &cut);
    CHECK(cut);
    REQUIRE(limited.size() == 2);
    CHECK(limited[0] == QStringList{"1", "2"});
}

TEST_CASE("file preview: reading text, JSON and CSV files", "[file_preview]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    const auto text = readText(writeFile(dir, "a.txt", "\xEF\xBB\xBFhello"), "text");
    CHECK(text.ok);
    CHECK(text.text == "hello");

    const auto json = readText(writeFile(dir, "a.json", R"({"a":[1,2]})"), "json");
    CHECK(json.ok);
    CHECK(json.text.contains("\n"));  // pretty-printed
    CHECK(json.text.contains("\"a\""));

    const auto broken = readText(writeFile(dir, "b.json", "{not json"), "json");
    CHECK(broken.ok);
    CHECK(broken.text == "{not json");  // shown as it is

    const auto csv = readText(writeFile(dir, "a.csv", "x,y\n1,2\n"), "csv");
    CHECK(csv.ok);
    REQUIRE(csv.rows.size() == 2);
    CHECK(csv.rows[1].toStringList() == QStringList{"1", "2"});

    const auto binary = readText(writeFile(dir, "c.txt", QByteArray("\x89PNG\r\n\x1A\n\0\0", 10)), "text");
    CHECK_FALSE(binary.ok);
    CHECK_FALSE(binary.error.isEmpty());

    const auto big = readText(writeFile(dir, "big.txt", QByteArray(kMaxTextBytes + 1, 'a')), "text");
    CHECK_FALSE(big.ok);
}

TEST_CASE("file preview: PDFs are checked by their header", "[file_preview]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    CHECK(looksLikePdf(writeFile(dir, "a.pdf", "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n")));
    CHECK(looksLikePdf(writeFile(dir, "b.pdf", QByteArray(100, ' ') + "%PDF-1.4")));
    CHECK_FALSE(looksLikePdf(writeFile(dir, "c.pdf", "<html><script>alert(1)</script>")));
    CHECK_FALSE(looksLikePdf(dir.filePath("missing.pdf").toStdWString()));
}
