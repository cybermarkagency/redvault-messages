// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The story studio's Qt side, with no window: the photo editor, the collage maker and the voice studio as QML uses
// them, and what they make sealed into stories exactly as StoriesSession seals them. The stories are written to
// <build>/tests/story-desktop-out/desktop_posted.json, which vectors/verify_desktop_posted.ts opens with the web app's
// own code (the other half of the check: what the desktop posts, the web and Android read).
#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>

#include <nlohmann/json.hpp>

#include "app_test_support.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/model/stories.hpp"
#include "redvault/studio/audio_dsp.hpp"
#include "redvault/studio/captions.hpp"
#include "redvault/studio/text_templates.hpp"
#include "redvault/util/text.hpp"
#include "studio_tools.hpp"

using namespace redvault;
using redvault::app::CollageMaker;
using redvault::app::PhotoEditor;
using redvault::app::VoiceStudio;
using Json = nlohmann::json;
namespace st = redvault::model::stories;

namespace {

/// A test picture: left half red, right half blue, a green square in the top-left corner.
QUrl writePicture(const QTemporaryDir& dir, const QString& name, int w, int h) {
    QImage image(w, h, QImage::Format_RGB32);
    QPainter p(&image);
    p.fillRect(0, 0, w / 2, h, QColor(220, 20, 20));
    p.fillRect(w / 2, 0, w - w / 2, h, QColor(20, 20, 220));
    p.fillRect(0, 0, w / 5, w / 5, QColor(20, 220, 20));
    p.end();
    const QString path = dir.filePath(name);
    REQUIRE(image.save(path, "PNG"));
    return QUrl::fromLocalFile(path);
}

QByteArray readAll(const QUrl& url) {
    QFile f(url.toLocalFile());
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

studio::Mono tone(double seconds, double rate) {
    studio::Mono m{std::vector<float>(static_cast<std::size_t>(seconds * rate)), rate};
    for (std::size_t i = 0; i < m.samples.size(); ++i) {
        m.samples[i] = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 220 * static_cast<double>(i) / rate) * (0.5 + 0.5 * std::sin(i / rate * 3)));
    }
    return m;
}

/// Seals a story the way StoriesSession::publish does: a fresh key, the media encrypted in 64 KiB chunks, the details
/// sealed for the story's id, the key wrapped for bob (and only bob).
Json seal(const std::string& name, st::Meta meta, const std::string& media, const crypto::KeyPair& alice, const crypto::KeyPair& bob) {
    const std::string id = util::randomUuid();
    const auto key = crypto::stories::newStoryKey();
    std::string ciphertext;
    if (meta.media) {
        std::istringstream in(media);
        std::ostringstream out;
        const auto encrypted = crypto::files::encrypt(in, out);
        ciphertext = out.str();
        meta.media->key = crypto::base64Encode(encrypted.key);
        meta.media->header = crypto::base64Encode(encrypted.header);
        meta.media->sha256 = encrypted.ciphertextSha256;
        meta.media->encryptedSize = encrypted.encryptedSize;
        meta.media->chunkSize = crypto::files::kChunkBytes;
    }
    const std::string json = st::toJson(meta);
    // What every reader will make of it: the desktop's own parser accepts it first.
    REQUIRE_NOTHROW(st::parseMeta(json, meta.kind));
    const auto sealed = crypto::stories::sealMeta(id, key, json);
    Json s;
    s["name"] = name;
    s["id"] = id;
    s["kind"] = meta.kind;
    s["metaCiphertext"] = crypto::base64Encode(sealed.ciphertext);
    s["metaNonce"] = crypto::base64Encode(sealed.nonce);
    s["hasMedia"] = meta.media.has_value();
    s["mediaSize"] = meta.media ? Json(meta.media->encryptedSize) : Json(nullptr);
    s["wrappedKeyForBob"] = crypto::base64Encode(crypto::stories::wrapStoryKey(id, key, bob.publicKey, alice.privateKey));
    if (meta.media) {
        s["media"] = crypto::base64Encode(crypto::ByteView(reinterpret_cast<const std::uint8_t*>(ciphertext.data()), ciphertext.size()));
        s["plainLength"] = media.size();
        s["plain"] = crypto::base64Encode(crypto::ByteView(reinterpret_cast<const std::uint8_t*>(media.data()), media.size()));
    }
    s["expected"] = Json::parse(json);
    return s;
}

st::MediaRef mediaRef(const std::string& mime, int w, int h, std::optional<std::int64_t> durationMs) {
    st::MediaRef m;
    m.key = "AA";
    m.header = "AA";
    m.sha256 = std::string(64, '0');
    m.encryptedSize = 1;
    m.chunkSize = 1;
    m.mime = mime;
    m.width = w;
    m.height = h;
    m.durationMs = durationMs;
    return m;
}

}  // namespace

TEST_CASE("the photo editor crops, turns, adjusts, draws, undoes and saves a fresh JPEG", "[studio]") {
    test::qt();
    QTemporaryDir dir;
    PhotoEditor editor;
    editor.setSource(writePicture(dir, "photo.png", 800, 600));
    REQUIRE(editor.loaded());
    CHECK(!editor.edited());
    CHECK(editor.previewUrl().startsWith("image://studio/"));
    CHECK(std::abs(editor.previewAspect() - 800.0 / 600.0) < 1e-6);

    editor.rotate(1);  // a quarter turn: 600 x 800
    CHECK(editor.rotation() == 90);
    CHECK(std::abs(editor.previewAspect() - 600.0 / 800.0) < 1e-6);
    editor.setAspect(1.0);  // a centred square
    CHECK(editor.hasCrop());
    editor.beginCropDrag();
    for (int i = 0; i < 10; ++i) editor.dragCrop("se", -0.01, -0.01);  // ten moves of one drag
    editor.endCropDrag();
    const auto afterDrag = editor.crop();
    editor.setFilter("noir");
    editor.beginAdjust();
    for (int v = 0; v <= 40; v += 5) editor.setAdjust("brightness", v);
    editor.endAdjust();
    CHECK(editor.brightness() == 40);
    editor.beginStroke(0.2, 0.2, "yellow", 0.03, false);
    editor.extendStroke(0.8, 0.8);
    editor.endStroke();
    CHECK(editor.strokeCount() == 1);

    // Each gesture is one step back: the stroke, the slider, the look, the crop drag, the square, the turn.
    editor.undo();
    CHECK(editor.strokeCount() == 0);
    editor.undo();
    CHECK(editor.brightness() == 0);
    editor.undo();
    CHECK(editor.filter() == "none");
    editor.undo();
    CHECK(editor.crop() != afterDrag);
    editor.redo();
    CHECK(editor.crop() == afterDrag);
    editor.redo();
    editor.redo();
    editor.redo();
    CHECK(editor.strokeCount() == 1);
    CHECK(!editor.canRedo());
    // A turn clears the drawing (it belongs to the finished picture).
    editor.flip();
    CHECK(editor.strokeCount() == 0);
    CHECK(!editor.hasCrop());
    editor.undo();

    const QUrl saved = editor.save();
    REQUIRE(saved.isLocalFile());
    QImage out(saved.toLocalFile());
    REQUIRE(!out.isNull());
    CHECK(readAll(saved).startsWith("\xFF\xD8\xFF"));  // a JPEG, made here
    // The square (600 x 600 of the turned 600 x 800 picture), its corner dragged in by a tenth of each side.
    CHECK(out.width() == 540);
    CHECK(out.height() == 520);
    QFile::remove(saved.toLocalFile());
}

TEST_CASE("the collage maker takes two to four photos and draws a 1080 by 1920 picture", "[studio]") {
    test::qt();
    QTemporaryDir dir;
    CollageMaker maker;
    CHECK(!maker.ready());
    maker.addPhotos({writePicture(dir, "a.png", 400, 300)});
    CHECK(!maker.ready());
    CHECK(maker.layouts().isEmpty());
    maker.addPhotos({writePicture(dir, "b.png", 300, 400), QUrl::fromLocalFile(dir.filePath("missing.png"))});
    CHECK(maker.count() == 2);
    CHECK(maker.error() == "One photo couldn't be opened.");
    CHECK(maker.ready());
    CHECK(maker.layout() == "stack2");
    maker.setLayout("side2");
    CHECK(maker.layout() == "side2");
    maker.setLayout("grid4");  // not for two photos
    CHECK(maker.layout() == "side2");
    maker.addPhotos({writePicture(dir, "c.png", 500, 500), writePicture(dir, "d.png", 500, 500), writePicture(dir, "e.png", 500, 500)});
    CHECK(maker.count() == 4);
    CHECK(maker.error() == "A collage takes up to four photos.");
    CHECK(maker.layout() == "grid4");
    maker.movePhoto(0, 1);
    maker.setGap(500);
    CHECK(maker.gap() == 40);
    maker.setBackground(1);
    const QUrl saved = maker.save();
    REQUIRE(saved.isLocalFile());
    const QImage out(saved.toLocalFile());
    CHECK(out.width() == 1080);
    CHECK(out.height() == 1920);
    // The gap between cells is the background (Light).
    const QColor middle = out.pixelColor(540, 960);
    CHECK(middle.red() > 230);
    CHECK(middle.green() > 230);
    QFile::remove(saved.toLocalFile());
}

TEST_CASE("the voice studio trims, changes the voice and finishes a WAV story", "[studio]") {
    test::qt();
    QTemporaryDir dir;
    VoiceStudio voice;
    CHECK(!voice.hasSound());
    // A WAV file as the studio opens one (an imported recording).
    const std::string wav = studio::audio::encodeWav(tone(3.0, 44100));
    QFile file(dir.filePath("voice.wav"));
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(wav.data(), static_cast<qint64>(wav.size()));
    file.close();
    voice.openFile(QUrl::fromLocalFile(file.fileName()));
    REQUIRE(voice.hasSound());
    CHECK(voice.durationMs() == 3000);
    CHECK(voice.waveform().size() == 48);
    voice.setTrim(500, 2500);
    voice.setFilter("robot");
    CHECK(voice.filter() == "robot");
    const QUrl listen = voice.preview();
    REQUIRE(listen.isLocalFile());
    const auto done = voice.finish();
    const QUrl url = done.value("url").toUrl();
    REQUIRE(url.isLocalFile());
    CHECK(done.value("durationMs").toDouble() == 2000);
    CHECK(done.value("waveform").toList().size() == 48);
    const QByteArray bytes = readAll(url);
    const auto decoded = studio::audio::decodeWav(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    REQUIRE(decoded);
    CHECK(decoded->sampleRate == studio::audio::kStoryAudioRate);
    // Too short is refused with a sentence.
    voice.setTrim(0, 100);
    CHECK(voice.finish().value("url").toUrl().isEmpty());
    CHECK(!voice.error().isEmpty());
    QFile::remove(url.toLocalFile());
    QFile::remove(listen.toLocalFile());
}

TEST_CASE("stories the desktop studio makes are written for the web app's code to open", "[studio]") {
    test::qt();
    QTemporaryDir dir;
    const auto alice = crypto::generateKeyPair();
    const auto bob = crypto::generateKeyPair();
    Json stories = Json::array();

    // A voice story: recorded sound through the studio's chain, a title, a look, subtitles, moving words.
    {
        VoiceStudio voice;
        voice.setSound(tone(4.0, 48000));
        voice.setTrim(250, 3750);
        voice.setFilter("echo");
        const auto done = voice.finish();
        const QByteArray bytes = readAll(done.value("url").toUrl());
        st::Meta meta;
        meta.kind = "audio";
        meta.text = "Desktop voice";
        meta.background = "ocean";
        meta.font = "modern";
        std::vector<int> bars;
        for (const auto& b : done.value("waveform").toList()) bars.push_back(b.toInt());
        meta.waveform = bars;
        meta.captions = studio::captions::evenCaptions({"first line", "second line"}, static_cast<std::int64_t>(done.value("durationMs").toDouble()));
        st::Overlay word;
        word.type = "text";
        word.text = "listen";
        word.y = 0.2;
        word.anim = "pulse";
        meta.overlays.push_back(word);
        meta.media = mediaRef("audio/wav", 1, 1, static_cast<std::int64_t>(done.value("durationMs").toDouble()));
        stories.push_back(seal("voice", meta, bytes.toStdString(), alice, bob));
    }
    // A photo from the editor.
    {
        PhotoEditor editor;
        editor.setSource(writePicture(dir, "edit.png", 900, 600));
        editor.setAspect(4.0 / 5.0);
        editor.setFilter("warm");
        editor.beginStroke(0.5, 0.5, "pink", 0.05, false);
        editor.endStroke();
        const QUrl saved = editor.save();
        const QByteArray bytes = readAll(saved);
        const QImage image(saved.toLocalFile());
        st::Meta meta;
        meta.kind = "image";
        meta.media = mediaRef("image/jpeg", image.width(), image.height(), std::nullopt);
        stories.push_back(seal("photo", meta, bytes.toStdString(), alice, bob));
    }
    // A collage.
    {
        CollageMaker maker;
        maker.addPhotos({writePicture(dir, "c1.png", 400, 300), writePicture(dir, "c2.png", 300, 400), writePicture(dir, "c3.png", 300, 300)});
        maker.setLayout("big1small2");
        const QUrl saved = maker.save();
        st::Meta meta;
        meta.kind = "image";
        meta.media = mediaRef("image/jpeg", 1080, 1920, std::nullopt);
        stories.push_back(seal("collage", meta, readAll(saved).toStdString(), alice, bob));
    }
    // A video with subtitles (the bytes stand in for a video: only the format of the details is under test).
    {
        std::string bytes(5000, '\0');
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 11 + 1) & 0xff);
        st::Meta meta;
        meta.kind = "video";
        meta.captions = studio::captions::parseSrt("1\n00:00:00,500 --> 00:00:02,000\n<i>Hello</i> there\n\n2\n00:00:02,500 --> 00:00:04,000\nfrom the desktop\n");
        meta.media = mediaRef("video/mp4", 1080, 1920, 6000);
        stories.push_back(seal("video", meta, bytes, alice, bob));
    }
    // A text story from a template, with an animated emoji and its sticker.
    {
        int n = 0;
        const auto t = studio::templates::apply(*studio::templates::byId("countdown"), [&] { return "desktopid" + std::to_string(100 + n++); },
                                                1'900'000'000'000LL);
        st::Meta meta;
        meta.kind = "text";
        meta.text = t.text;
        meta.background = t.background;
        meta.font = t.font;
        meta.overlays = t.overlays;
        meta.overlays.at(0).anim = "bounce";
        meta.stickers = t.stickers;
        stories.push_back(seal("template", meta, {}, alice, bob));
    }

    Json out;
    out["note"] = "Made by tests/studio_app_test.cpp with the desktop's own studio and story code; opened by vectors/verify_desktop_posted.ts with the web app's.";
    out["alice"] = {{"publicKey", crypto::base64Encode(alice.publicKey)}};
    out["bob"] = {{"privateKey", crypto::base64Encode(bob.privateKey.view())}};
    out["stories"] = stories;
    QDir().mkpath(QStringLiteral(REDVAULT_STUDIO_OUT));
    std::ofstream file(std::string(REDVAULT_STUDIO_OUT) + "/desktop_posted.json");
    file << out.dump(2);
    file.close();
    CHECK(QFileInfo(QStringLiteral(REDVAULT_STUDIO_OUT) + "/desktop_posted.json").size() > 1000);
    app::StudioTools().clearScratch();
}
