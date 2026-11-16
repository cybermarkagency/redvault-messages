// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The Stories creation studio's maths (subtitles, voice, photo editing, collages, templates, motion) against what the
// web app's own code makes of the same inputs: vectors/studio_vectors.json, made by
// android/app/src/test/resources/vectors/make_android_studio_vectors.ts (branch paid-communities) and shared with
// Android's StudioVectorsTest and CollageTemplatesMotionTest. Sound is compared sample by sample to 2e-5 (JavaScript
// and C++ can differ in the last bit of a sine); everything else is exact, as on Android.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/model/stories.hpp"
#include "redvault/studio/audio_dsp.hpp"
#include "redvault/studio/captions.hpp"
#include "redvault/studio/collage.hpp"
#include "redvault/studio/image_edit.hpp"
#include "redvault/studio/story_motion.hpp"
#include "redvault/studio/text_templates.hpp"

using namespace redvault;
using namespace redvault::studio;
using Json = nlohmann::json;
using Catch::Matchers::WithinAbs;
namespace st = redvault::model::stories;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_STUDIO_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

std::string bytes(const Json& j) {
    const auto b = crypto::base64Decode(j.get<std::string>());
    return std::string(b.begin(), b.end());
}

std::vector<float> floats(const Json& j) {
    const std::string b = bytes(j);
    std::vector<float> out(b.size() / 4);
    // memcpy with a null pointer is undefined even for 0 bytes (UBSan, RV-1115-13): skip empty vectors.
    if (!out.empty()) std::memcpy(out.data(), b.data(), out.size() * 4);  // little-endian, as on every machine this runs on
    return out;
}

Mono mono(const Json& j) {
    return Mono{floats(j["samples"]), j["rate"].get<double>()};
}

void checkMono(const std::string& at, const Json& expected, const Mono& actual, double tolerance = 2e-5) {
    INFO(at);
    const Mono want = mono(expected);
    CHECK(want.sampleRate == actual.sampleRate);
    REQUIRE(want.samples.size() == actual.samples.size());
    std::size_t worst = 0;
    double worstDiff = 0;
    for (std::size_t i = 0; i < want.samples.size(); ++i) {
        const double d = std::abs(static_cast<double>(want.samples[i]) - static_cast<double>(actual.samples[i]));
        if (d > worstDiff) {
            worstDiff = d;
            worst = i;
        }
    }
    INFO("worst sample " << worst << " off by " << worstDiff);
    CHECK(worstDiff <= tolerance);
}

const std::map<std::string, Mono>& inputs() {
    static const std::map<std::string, Mono> m = [] {
        std::map<std::string, Mono> out;
        for (const auto& [name, v] : vectors()["audio"]["inputs"].items()) out[name] = mono(v);
        return out;
    }();
    return m;
}

const Mono& input(const Json& name) {
    return inputs().at(name.get<std::string>());
}

void checkCaptions(const std::string& at, const Json& expected, const std::vector<st::Caption>& actual) {
    INFO(at);
    REQUIRE(expected.size() == actual.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CHECK(actual[i].startMs == expected[i]["startMs"].get<std::int64_t>());
        CHECK(actual[i].endMs == expected[i]["endMs"].get<std::int64_t>());
        CHECK(actual[i].text == expected[i]["text"].get<std::string>());
    }
}

Rect rect(const Json& j) {
    return Rect{j["x"].get<double>(), j["y"].get<double>(), j["w"].get<double>(), j["h"].get<double>()};
}

Point point(const Json& j) {
    return Point{j["x"].get<double>(), j["y"].get<double>()};
}

Stroke stroke(const Json& j) {
    Stroke s;
    s.color = j["color"];
    s.size = j["size"];
    s.eraser = j["eraser"];
    for (const auto& p : j["points"]) s.points.push_back(point(p));
    return s;
}

EditState state(const Json& j) {
    EditState s;
    s.rotation = j["rotation"];
    s.flipX = j["flipX"];
    if (!j["crop"].is_null()) s.crop = rect(j["crop"]);
    s.adjust = Adjust{j["adjust"]["brightness"], j["adjust"]["contrast"], j["adjust"]["saturation"], j["adjust"]["blur"]};
    s.filter = j["filter"];
    for (const auto& k : j["strokes"]) s.strokes.push_back(stroke(k));
    return s;
}

void checkRect(const std::string& at, const Json& expected, const std::optional<Rect>& actual) {
    INFO(at);
    if (expected.is_null()) {
        CHECK(!actual);
        return;
    }
    REQUIRE(actual);
    const Rect w = rect(expected);
    CHECK_THAT(actual->x, WithinAbs(w.x, 1e-12));
    CHECK_THAT(actual->y, WithinAbs(w.y, 1e-12));
    CHECK_THAT(actual->w, WithinAbs(w.w, 1e-12));
    CHECK_THAT(actual->h, WithinAbs(w.h, 1e-12));
}

void checkParams(const std::string& at, const Json& e, const ColorParams& a) {
    INFO(at);
    CHECK_THAT(a.brightness, WithinAbs(e["brightness"].get<double>(), 1e-12));
    CHECK_THAT(a.contrast, WithinAbs(e["contrast"].get<double>(), 1e-12));
    CHECK_THAT(a.saturate, WithinAbs(e["saturate"].get<double>(), 1e-12));
    CHECK_THAT(a.grayscale, WithinAbs(e["grayscale"].get<double>(), 1e-12));
    CHECK_THAT(a.sepia, WithinAbs(e["sepia"].get<double>(), 1e-12));
    CHECK_THAT(a.hueRotate, WithinAbs(e["hueRotate"].get<double>(), 1e-12));
    CHECK_THAT(a.blurPx, WithinAbs(e["blurPx"].get<double>(), 1e-12));
}

}  // namespace

// --- Subtitles ---------------------------------------------------------------------------------------------------

TEST_CASE("clock texts are read and written as the web reads and writes them", "[studio][captions]") {
    const auto& c = vectors()["captions"];
    REQUIRE(c["parseClock"].size() > 20);
    for (const auto& e : c["parseClock"]) {
        INFO("parseClock(\"" << e["text"].get<std::string>() << "\")");
        const auto got = captions::parseClock(e["text"].get<std::string>());
        if (e["ms"].is_null()) CHECK(!got);
        else CHECK(got == e["ms"].get<std::int64_t>());
    }
    REQUIRE(c["formatClock"].size() > 600);
    for (const auto& e : c["formatClock"]) {
        INFO("formatClock(" << e["ms"].get<double>() << ")");
        CHECK(captions::formatClock(e["ms"].get<double>()) == e["text"].get<std::string>());
    }
    // The web's own rounding, kept: 59.95 seconds is written as a minute that has sixty seconds; an exact .x5 rounds up.
    CHECK(captions::formatClock(59'950) == "0:60.0");
    CHECK(captions::formatClock(59'949) == "0:59.9");
    CHECK(captions::formatClock(250) == "0:00.3");
    CHECK(captions::formatClock(-5) == "0:00.0");
    CHECK(captions::formatClock(83'400) == "1:23.4");
}

TEST_CASE("srt files are read, written and spread over a recording as the web does it", "[studio][captions]") {
    const auto& c = vectors()["captions"];
    int n = 0;
    for (const auto& e : c["parseSrt"]) checkCaptions("srt " + std::to_string(n++), e["captions"], captions::parseSrt(e["srt"].get<std::string>()));
    std::vector<st::Caption> list;
    for (const auto& k : c["toSrt"]["captions"]) list.push_back({k["startMs"], k["endMs"], k["text"]});
    CHECK(captions::toSrt(list) == c["toSrt"]["srt"].get<std::string>());
    CHECK(captions::parseSrt(captions::toSrt(list)) == list);  // what is written reads back
    n = 0;
    for (const auto& e : c["evenCaptions"]) {
        checkCaptions("even " + std::to_string(n++), e["captions"], captions::evenCaptions(e["lines"].get<std::vector<std::string>>(), e["durationMs"]));
    }
}

// --- Voice -------------------------------------------------------------------------------------------------------

TEST_CASE("resampling, trimming, fading, evening out and gain match the web", "[studio][voice]") {
    const auto& a = vectors()["audio"];
    for (const auto& e : a["resample"]) checkMono("resample " + e["input"].get<std::string>(), e["result"], audio::resample(input(e["input"]), e["rate"]));
    for (const auto& e : a["trim"]) checkMono("trim " + e["input"].get<std::string>(), e["result"], audio::trim(input(e["input"]), e["startMs"], e["endMs"]));
    for (const auto& e : a["fade"]) checkMono("fade " + e["input"].get<std::string>(), e["result"], audio::fade(input(e["input"]), e["inMs"], e["outMs"]));
    for (const auto& e : a["normalize"]) {
        const Mono got = e["target"].is_null() ? audio::normalize(input(e["input"])) : audio::normalize(input(e["input"]), e["target"].get<double>());
        checkMono("normalize " + e["input"].get<std::string>(), e["result"], got);
    }
    for (const auto& e : a["gain"]) checkMono("gain " + e["input"].get<std::string>(), e["result"], audio::gain(input(e["input"]), e["factor"]));
    for (const auto& e : a["duration"]) CHECK(audio::durationMs(input(e["input"])) == e["ms"].get<std::int64_t>());
    std::vector<std::vector<float>> channels;
    for (const auto& c : a["mixToMono"]["channels"]) channels.push_back(floats(c));
    checkMono("mixToMono", a["mixToMono"]["result"], audio::mixToMono(channels, a["mixToMono"]["rate"]));
}

TEST_CASE("every voice effect sounds the way the web makes it", "[studio][voice]") {
    const auto& filters = vectors()["audio"]["filters"];
    REQUIRE(filters.size() >= 8);
    for (const auto& e : filters) {
        const auto f = filterFromWire(e["filter"].get<std::string>());
        REQUIRE(f);
        checkMono(e["filter"].get<std::string>() + " on " + e["input"].get<std::string>(), e["result"], audio::applyVoiceFilter(input(e["input"]), *f));
    }
    // The six the web offers, in its order.
    std::vector<std::string> wires;
    for (const auto f : kVoiceFilters) wires.emplace_back(filterWire(f));
    CHECK(wires == std::vector<std::string>{"none", "chipmunk", "deep", "robot", "echo", "radio"});
}

TEST_CASE("the loudness bars are the web's", "[studio][voice]") {
    for (const auto& e : vectors()["audio"]["waveform"]) {
        INFO(e["input"].get<std::string>() << " x " << e["bars"].get<int>());
        CHECK(audio::waveformBars(input(e["input"]).samples, e["bars"]) == e["result"].get<std::vector<int>>());
    }
}

TEST_CASE("a WAV is written byte for byte as the web writes it, and read back", "[studio][voice]") {
    const auto& w = vectors()["audio"]["wav"];
    const std::string want = bytes(w["bytes"]);
    CHECK(audio::encodeWav(mono(w["source"])) == want);
    const auto decoded = audio::decodeWav(want);
    REQUIRE(decoded);
    checkMono("decoded", w["decoded"], *decoded, 1e-6);
    CHECK(!audio::decodeWav(std::string(10, '\0')));
    CHECK(!audio::decodeWav(want.substr(0, 43)));
    std::string stereo = want;
    stereo[22] = 2;
    CHECK(!audio::decodeWav(stereo));
    std::string eightBit = want;
    eightBit[34] = 8;
    CHECK(!audio::decodeWav(eightBit));
    // A size that claims more than there is reads only what is there.
    std::string longer = want;
    const std::uint32_t big = 1'000'000;
    std::memcpy(longer.data() + 40, &big, 4);
    CHECK(audio::decodeWav(longer)->samples.size() == (want.size() - 44) / 2);
}

TEST_CASE("a finished voice is the web's finished voice", "[studio][voice]") {
    for (const auto& e : vectors()["audio"]["finish"]) {
        const auto f = filterFromWire(e["filter"].get<std::string>());
        REQUIRE(f);
        const std::string at = "finish " + e["input"].get<std::string>() + " " + e["filter"].get<std::string>();
        INFO(at);
        const FinishedVoice done = audio::finishVoice(input(e["input"]), e["startMs"], e["endMs"], *f);
        CHECK(done.durationMs == e["durationMs"].get<std::int64_t>());
        CHECK(done.waveform == e["waveform"].get<std::vector<int>>());
        const std::string want = bytes(e["wav"]);
        REQUIRE(want.size() == done.wav.size());
        // The header is identical; each 16-bit sample may differ by a step or two from the last bit of a sine.
        CHECK(want.substr(0, 44) == done.wav.substr(0, 44));
        int worst = 0;
        for (std::size_t k = 44; k + 1 < want.size(); k += 2) {
            std::int16_t a = 0;
            std::int16_t b = 0;
            std::memcpy(&a, want.data() + k, 2);
            std::memcpy(&b, done.wav.data() + k, 2);
            worst = std::max(worst, std::abs(a - b));
        }
        CHECK(worst <= 2);
    }
}

// --- Photos ------------------------------------------------------------------------------------------------------

TEST_CASE("turning and sizing match the web", "[studio][photo]") {
    const auto& im = vectors()["image"];
    for (const auto& e : im["rotatedSize"]) {
        CHECK(image::rotatedSize(e["w"], e["h"], e["rotation"]) == std::pair<int, int>{e["result"]["w"], e["result"]["h"]});
    }
    for (const auto& e : im["rotateBy"]) CHECK(image::rotateBy(e["rotation"], e["quarterTurns"]) == e["result"].get<int>());
    int n = 0;
    for (const auto& e : im["cropPixels"]) {
        INFO("cropPixels " << n++);
        const Pixels p = image::cropPixels(e["w"], e["h"], state(e["state"]));
        CHECK(std::vector<int>{p.sx, p.sy, p.sw, p.sh} ==
              std::vector<int>{e["result"]["sx"], e["result"]["sy"], e["result"]["sw"], e["result"]["sh"]});
    }
    n = 0;
    for (const auto& e : im["outputSize"]) {
        INFO("outputSize " << n++);
        CHECK(image::outputSize(e["w"], e["h"], state(e["state"]), e["maxSide"]) == std::pair<int, int>{e["result"]["w"], e["result"]["h"]});
    }
}

TEST_CASE("crops are kept inside and dragged as the web drags them", "[studio][photo]") {
    const auto& im = vectors()["image"];
    int n = 0;
    for (const auto& e : im["clampRect"]) checkRect("clampRect " + std::to_string(n++), e["result"], image::clampRect(rect(e["rect"])));
    n = 0;
    for (const auto& e : im["aspectCrop"]) {
        const auto aspect = e["aspect"].is_null() ? std::nullopt : std::optional<double>(e["aspect"].get<double>());
        checkRect("aspectCrop " + std::to_string(n++), e["result"], image::aspectCrop(aspect, e["w"], e["h"]));
    }
    REQUIRE(im["dragRect"].size() == 36);
    n = 0;
    for (const auto& e : im["dragRect"]) {
        checkRect("dragRect " + std::to_string(n++) + " " + e["handle"].get<std::string>(), e["result"],
                  image::dragRect(rect(e["rect"]), e["handle"].get<std::string>(), e["dx"], e["dy"]));
    }
}

TEST_CASE("colour filters, sliders and pixels match the web", "[studio][photo]") {
    const auto& im = vectors()["image"];
    REQUIRE(im["colorParams"].size() >= 10);
    int n = 0;
    for (const auto& e : im["colorParams"]) checkParams("colorParams " + std::to_string(n++), e["result"], image::colorParams(state(e["state"]), e["outputHeight"]));
    const std::string original = bytes(im["pixels"]["rgba"]);
    n = 0;
    for (const auto& run : im["pixels"]["runs"]) {
        const ColorParams c = image::colorParams(state(run["state"]), 800.0);
        checkParams("pixel run " + std::to_string(n), run["params"], c);
        std::vector<std::uint8_t> data(original.begin(), original.end());
        image::adjustPixels(data, c);
        const std::string want = bytes(run["result"]);
        INFO("pixel run " << n++);
        CHECK(std::string(data.begin(), data.end()) == want);
    }
    // The same changes on Qt's 0xAARRGGBB pixels (hue turn left out, as the web's by-hand path does): the same bytes.
    for (const auto& run : im["pixels"]["runs"]) {
        const ColorParams c = image::colorParams(state(run["state"]), 800.0);
        std::vector<std::uint32_t> argb;
        for (std::size_t i = 0; i + 3 < original.size(); i += 4) {
            const auto b = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<std::uint8_t>(original[i + k])); };
            argb.push_back((b(3) << 24) | (b(0) << 16) | (b(1) << 8) | b(2));
        }
        image::adjustArgb(argb.data(), argb.size(), c, false);
        const std::string want = bytes(run["result"]);
        for (std::size_t p = 0; p < argb.size(); ++p) {
            CHECK(((argb[p] >> 16) & 0xff) == static_cast<std::uint8_t>(want[p * 4]));
            CHECK(((argb[p] >> 8) & 0xff) == static_cast<std::uint8_t>(want[p * 4 + 1]));
            CHECK((argb[p] & 0xff) == static_cast<std::uint8_t>(want[p * 4 + 2]));
            CHECK((argb[p] >> 24) == static_cast<std::uint8_t>(want[p * 4 + 3]));
        }
    }
}

TEST_CASE("edits, strokes and their limits match the web", "[studio][photo]") {
    const auto& im = vectors()["image"];
    int n = 0;
    for (const auto& e : im["hasEdits"]) {
        INFO("hasEdits " << n++);
        CHECK(image::hasEdits(state(e["state"])) == e["result"].get<bool>());
    }
    Stroke s = image::newStroke("blue", 0.2, false, Point{-1.0, 2.0});
    n = 0;
    for (const auto& step : im["strokeSteps"]) {
        INFO("step " << n++);
        s = image::extendStroke(s, point(step["point"]));
        const Stroke want = stroke(step["stroke"]);
        CHECK(s.size == want.size);
        CHECK(s.points == want.points);
    }
    Stroke l = image::newStroke("white", 0.001, true, Point{0, 0});
    for (int i = 1; i < 2100; ++i) l = image::extendStroke(l, Point{(i % 700) / 700.0, (i / 700) / 3.0});
    CHECK(l.points.size() == im["longStroke"]["points"].get<std::size_t>());
    CHECK(l.points.front() == point(im["longStroke"]["first"]));
    CHECK(l.points.back() == point(im["longStroke"]["last"]));
    EditState many;
    for (int i = 0; i < 410; ++i) many = image::addStroke(many, image::newStroke("green", 0.01, i % 2 == 0, Point{(i % 10) / 10.0, 0.5}));
    CHECK(many.strokes.size() == im["manyStrokes"]["count"].get<std::size_t>());
    CHECK(many.strokes.front().eraser == im["manyStrokes"]["firstEraser"].get<bool>());
    CHECK(many.strokes.back().eraser == im["manyStrokes"]["lastEraser"].get<bool>());
    for (const auto& e : im["newStroke"]) {
        const Stroke made = image::newStroke(e["color"].get<std::string>(), e["size"], e["eraser"], Point{0.3, 0.7});
        const Stroke want = stroke(e["result"]);
        CHECK(made == want);
    }
    const auto& g = im["withGeometry"];
    const EditState after = image::withGeometry(state(g["before"]), 180, true, state(g["before"]).crop);
    CHECK(after == state(g["result"]));
    CHECK(after.strokes.empty());
    const auto& k = im["constants"];
    CHECK(k["minCrop"].get<double>() == image::kMinCrop);
    CHECK(k["minPen"].get<double>() == image::kMinPen);
    CHECK(k["maxPen"].get<double>() == image::kMaxPen);
    CHECK(k["maxStrokes"].get<std::size_t>() == image::kMaxStrokes);
    CHECK(k["maxPoints"].get<std::size_t>() == image::kMaxPointsPerStroke);
    CHECK(k["filters"].get<std::vector<std::string>>() == image::filters());
    for (const auto& [name, hex] : k["penColors"].items()) CHECK(image::penColor(name) == hex.get<std::string>());
    CHECK(!image::hasEdits(EditState{}));
}

TEST_CASE("undo and redo keep plain steps, one per drag", "[studio][photo]") {
    EditHistory<int> h(0);
    CHECK(!h.canUndo());
    h.push(1);
    h.push(2);
    h.push(2);  // the same value is no new step
    CHECK(h.pastSize() == 2);
    h.undo();
    CHECK(h.present() == 1);
    CHECK(h.canRedo());
    h.redo();
    CHECK(h.present() == 2);
    h.undo();
    h.push(7);  // a new step drops what could have been redone
    CHECK(!h.canRedo());
    // A slider dragged through many values is one step back to where it began.
    const int before = h.present();
    for (int v = 10; v < 60; ++v) h.replace(v);
    h.commitFrom(before);
    h.undo();
    CHECK(h.present() == 7);
    // Only the last 60 steps are kept.
    EditHistory<int> deep(0);
    for (int i = 1; i <= 100; ++i) deep.push(i);
    CHECK(deep.pastSize() == 60);
    for (int i = 0; i < 100; ++i) deep.undo();
    CHECK(deep.present() == 40);
}

// --- Collages, templates, motion ---------------------------------------------------------------------------------

TEST_CASE("the collage layouts and maths are the web's, cell for cell", "[studio][collage]") {
    const auto& c = vectors()["collage"];
    CHECK(c["max"].get<int>() == collage::kMaxPhotos);
    REQUIRE(c["layouts"].size() == collage::layouts().size());
    for (std::size_t i = 0; i < collage::layouts().size(); ++i) {
        const auto& mine = collage::layouts()[i];
        const auto& web = c["layouts"][i];
        CHECK(mine.id == web["id"].get<std::string>());
        CHECK(mine.label == web["label"].get<std::string>());
        REQUIRE(mine.cells.size() == web["cells"].size());
        for (std::size_t j = 0; j < mine.cells.size(); ++j) {
            CHECK(mine.cells[j].x == web["cells"][j]["x"].get<double>());
            CHECK(mine.cells[j].y == web["cells"][j]["y"].get<double>());
            CHECK(mine.cells[j].w == web["cells"][j]["w"].get<double>());
            CHECK(mine.cells[j].h == web["cells"][j]["h"].get<double>());
        }
    }
    for (const auto& e : c["layoutsFor"]) {
        std::vector<std::string> ids;
        for (const auto& l : collage::layoutsFor(e["count"])) ids.push_back(l.id);
        CHECK(ids == e["ids"].get<std::vector<std::string>>());
    }
    CHECK(collage::layoutsFor(1).empty());
    CHECK(collage::layoutsFor(5).empty());
    REQUIRE(c["cellRects"].size() == 150);
    for (const auto& e : c["cellRects"]) {
        const auto* layout = collage::layoutById(e["layout"].get<std::string>());
        REQUIRE(layout);
        const auto mine = collage::cellRects(*layout, e["w"], e["h"], e["gap"]);
        REQUIRE(mine.size() == e["result"].size());
        for (std::size_t i = 0; i < mine.size(); ++i) {
            INFO(layout->id << " cell " << i);
            CHECK(mine[i].x == e["result"][i]["x"].get<double>());
            CHECK(mine[i].y == e["result"][i]["y"].get<double>());
            CHECK(mine[i].w == e["result"][i]["w"].get<double>());
            CHECK(mine[i].h == e["result"][i]["h"].get<double>());
        }
    }
    for (const auto& e : c["fitCover"]) {
        const auto f = collage::fitCover(e["imageW"], e["imageH"], e["cellW"], e["cellH"]);
        CHECK(f.sx == e["result"]["sx"].get<double>());
        CHECK(f.sy == e["result"]["sy"].get<double>());
        CHECK(f.sw == e["result"]["sw"].get<double>());
        CHECK(f.sh == e["result"]["sh"].get<double>());
    }
}

TEST_CASE("every template starts a story the way the web's does", "[studio][templates]") {
    const auto& web = vectors()["templates"];
    REQUIRE(web.size() == templates::all().size());
    const auto madeAt = vectors()["templatesMadeAt"].get<std::int64_t>();
    for (std::size_t i = 0; i < web.size(); ++i) {
        const auto& w = web[i];
        const auto& t = templates::all()[i];
        INFO(t.id);
        CHECK(t.id == w["id"].get<std::string>());
        CHECK(t.label == w["label"].get<std::string>());
        const auto applied = templates::apply(t, [] { return std::string("fixedid00000"); }, madeAt);
        CHECK(applied.text == w["text"].get<std::string>());
        CHECK(applied.background == w["background"].get<std::string>());
        CHECK(applied.font == w["font"].get<std::string>());
        // The stickers, read back by the parser a viewer uses from what the web made: the same, field for field.
        CHECK(st::toJson(applied.stickers) == st::toJson(st::parseStickers(w["stickers"].dump())));
        REQUIRE(applied.overlays.size() == w["overlays"].size());
        for (std::size_t k = 0; k < applied.overlays.size(); ++k) {
            const auto& o = w["overlays"][k];
            const auto& mine = applied.overlays[k];
            CHECK(mine.type == o["type"].get<std::string>());
            CHECK(mine.text == o["text"].get<std::string>());
            CHECK(mine.x == o["x"].get<double>());
            CHECK(mine.y == o["y"].get<double>());
            CHECK(mine.scale == o["scale"].get<double>());
            CHECK(mine.rotation == o["rotation"].get<double>());
            CHECK(mine.color == o["color"].get<std::string>());
            CHECK(mine.font == o["font"].get<std::string>());
            CHECK(mine.boxed == o["boxed"].get<bool>());
            CHECK(mine.anim.empty());
        }
    }
    CHECK(templates::offered().size() == templates::all().size() - 1);
    // Fresh ids each time, and a countdown a week after it was used.
    int counter = 0;
    const auto make = [&] { return "id" + std::to_string(100000000 + counter++); };
    const auto a = templates::apply(*templates::byId("this-or-that"), make, 0);
    const auto b = templates::apply(*templates::byId("this-or-that"), make, 0);
    REQUIRE(a.stickers.size() == 1);
    CHECK(a.stickers[0].id != b.stickers[0].id);
    const auto c = templates::apply(*templates::byId("countdown"), make, 1000);
    CHECK(c.stickers.at(0).when == 1000.0 + 7 * 86'400'000.0);
}

TEST_CASE("words and emoji move along the web's curves", "[studio][motion]") {
    CHECK(motion::ease(0.0) == 0.0);
    CHECK(motion::ease(1.0) == 1.0);
    CHECK_THAT(motion::ease(0.5), WithinAbs(0.5, 1e-6));
    for (const double x : {0.1, 0.25, 0.4}) CHECK_THAT(motion::ease(x) + motion::ease(1 - x), WithinAbs(1.0, 1e-6));
    CHECK_THAT(motion::ease(0.25), WithinAbs(0.1291, 0.0005));
    // Pulse: 1.12 at halfway, every 1.4 s.
    CHECK_THAT(motion::frame("pulse", 0).scale, WithinAbs(1.0, 0.002));
    CHECK_THAT(motion::frame("pulse", 700).scale, WithinAbs(1.12, 1e-6));
    CHECK_THAT(motion::frame("pulse", 350).scale, WithinAbs(1.06, 1e-4));
    CHECK_THAT(motion::frame("pulse", 1400 * 5 + 700).scale, WithinAbs(1.12, 1e-6));
    CHECK_THAT(motion::frame("pulse", -700).scale, WithinAbs(1.12, 1e-6));
    // Bounce: up 14% of its height by 40%, resting from 60%.
    CHECK_THAT(motion::frame("bounce", 400).lift, WithinAbs(-0.14, 1e-6));
    CHECK_THAT(motion::frame("bounce", 200).lift, WithinAbs(-0.07, 1e-4));
    CHECK_THAT(motion::frame("bounce", 500).lift, WithinAbs(-0.07, 1e-4));
    CHECK_THAT(motion::frame("bounce", 800).lift, WithinAbs(0.0, 0.002));
    CHECK(motion::frame("bounce", 400).scale == 1.0);
    // Fade: 35% at halfway over 2.2 s.
    CHECK_THAT(motion::frame("fade", 1100).alpha, WithinAbs(0.35, 1e-6));
    CHECK_THAT(motion::frame("fade", 550).alpha, WithinAbs(0.675, 1e-4));
    CHECK(motion::frame("", 12'345) == motion::Frame{});
    CHECK(motion::frame("spin", 12'345) == motion::Frame{});
    // The button goes still, pulse, bounce, fade and round again.
    std::string a;
    std::vector<std::string> seen;
    for (int i = 0; i < 5; ++i) seen.push_back(a = motion::next(a));
    CHECK(seen == std::vector<std::string>{"pulse", "bounce", "fade", "", "pulse"});
    CHECK(motion::label("") == "Still");
    CHECK(motion::label("fade") == "Fade");
}
