// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/stories.hpp"

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>

#include <nlohmann/json.hpp>

#include "redvault/util/text.hpp"

namespace redvault::model::stories {

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::int64_t kSeenKeepMs = 48ll * 3600 * 1000;

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::size_t utf16Size(const std::string& text) {
    return util::utf16Length(util::decodeUtf8(text));
}

std::optional<std::string> str(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

bool isFraction(const Json& v) {
    return v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() >= 0 && v.get<double>() <= 1;
}

bool isB64(const std::optional<std::string>& v, std::size_t max) {
    static const std::regex kB64("^[A-Za-z0-9+/_=-]+$");
    return v && !v->empty() && v->size() <= max && std::regex_match(*v, kB64);
}

std::optional<std::uint64_t> positive(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    const auto value = it->get<std::int64_t>();
    // Number.isSafeInteger and > 0.
    if (value <= 0 || value > 9007199254740991ll) return std::nullopt;
    return static_cast<std::uint64_t>(value);
}

Overlay parseOverlay(const Json& o) {
    if (!o.is_object()) throw StoryFormatError();
    Overlay out;
    const auto type = str(o, "type");
    if (type != "text" && type != "emoji") throw StoryFormatError();
    out.type = *type;
    const auto text = str(o, "text");
    if (!text || text->empty() || utf16Size(*text) > kMaxOverlayText) throw StoryFormatError();
    out.text = *text;
    if (!o.contains("x") || !o.contains("y") || !isFraction(o["x"]) || !isFraction(o["y"])) throw StoryFormatError();
    out.x = o["x"].get<double>();
    out.y = o["y"].get<double>();
    if (const auto it = o.find("scale"); it != o.end() && it->is_number() && std::isfinite(it->get<double>())) {
        out.scale = std::clamp(it->get<double>(), 0.5, 4.0);
    }
    if (const auto it = o.find("rotation"); it != o.end() && it->is_number() && std::isfinite(it->get<double>())) {
        out.rotation = std::fmod(std::fmod(it->get<double>(), 360.0) + 360.0, 360.0);
    }
    if (const auto c = str(o, "color"); c && contains(kColors, *c)) out.color = *c;
    if (const auto it = o.find("boxed"); it != o.end() && it->is_boolean()) out.boxed = it->get<bool>();
    if (const auto f = str(o, "font"); f && contains(kFonts, *f)) out.font = *f;
    if (const auto a = str(o, "anim"); a && contains(kAnimations, *a)) out.anim = *a;
    return out;
}

/// Math.round: halves go up.
std::int64_t jsRound(double v) {
    return static_cast<std::int64_t>(std::floor(v + 0.5));
}

std::vector<Caption> captionsFrom(const Json& raw) {
    std::vector<Caption> out;
    if (!raw.is_array()) return out;
    std::size_t n = 0;
    for (const auto& c : raw) {
        if (n++ >= kMaxCaptions) break;
        if (!c.is_object()) continue;
        const auto t = c.find("text");
        if (t == c.end() || !t->is_string()) continue;
        const auto text = cleanStickerText(t->get<std::string>(), kMaxCaptionText);
        if (!text) continue;
        const auto s = c.find("startMs");
        const auto e = c.find("endMs");
        if (s == c.end() || e == c.end() || !s->is_number() || !e->is_number()) continue;
        const double sv = s->get<double>();
        const double ev = e->get<double>();
        if (!std::isfinite(sv) || !std::isfinite(ev)) continue;
        const std::int64_t startMs = jsRound(sv);
        const std::int64_t endMs = jsRound(ev);
        if (startMs < 0 || endMs <= startMs || endMs > kMaxCaptionEndMs) continue;
        out.push_back({startMs, endMs, *text});
    }
    std::stable_sort(out.begin(), out.end(), [](const Caption& a, const Caption& b) { return a.startMs < b.startMs; });
    return out;
}

std::optional<std::vector<int>> waveformFrom(const Json& raw) {
    if (!raw.is_array() || raw.size() < kWaveformMin || raw.size() > kWaveformMax) return std::nullopt;
    std::vector<int> out;
    for (const auto& v : raw) {
        if (!v.is_number()) return std::nullopt;
        const double d = v.get<double>();
        if (!std::isfinite(d) || d != std::floor(d) || d < 0 || d > 100) return std::nullopt;
        out.push_back(static_cast<int>(d));
    }
    return out;
}

OrderedJson captionsJson(const std::vector<Caption>& captions) {
    OrderedJson list = OrderedJson::array();
    for (const auto& c : captions) {
        OrderedJson item;
        item["startMs"] = c.startMs;
        item["endMs"] = c.endMs;
        item["text"] = c.text;
        list.push_back(std::move(item));
    }
    return list;
}

OrderedJson overlayJson(const Overlay& o) {
    OrderedJson item;
    item["type"] = o.type;
    item["text"] = o.text;
    item["x"] = o.x;
    item["y"] = o.y;
    item["scale"] = o.scale;
    item["rotation"] = o.rotation;
    item["color"] = o.color;
    item["boxed"] = o.boxed;
    item["font"] = o.font;
    if (!o.anim.empty()) item["anim"] = o.anim;
    return item;
}

std::vector<std::string> uuidList(const Json& j, const char* key) {
    static const std::regex kUuid("^[0-9a-fA-F-]{36}$");
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (!v.is_string()) continue;
        const auto s = v.get<std::string>();
        if (!std::regex_match(s, kUuid)) continue;
        const auto lower = util::toLowerAscii(s);
        if (!contains(out, lower)) out.push_back(lower);
        if (out.size() >= 5000) break;
    }
    return out;
}

}  // namespace

Meta parseMeta(std::string_view json, std::string_view kind) {
    Json r;
    try {
        r = Json::parse(json);
    } catch (const Json::exception&) {
        throw StoryFormatError();
    }
    if (!r.is_object()) throw StoryFormatError();
    if (!r.contains("v") || !r["v"].is_number_integer() || r["v"].get<int>() != 1) throw StoryFormatError();
    if (str(r, "kind") != std::string(kind)) throw StoryFormatError();
    Meta meta;
    meta.kind = std::string(kind);
    if (const auto it = r.find("overlays"); it != r.end() && it->is_array()) {
        std::size_t n = 0;
        for (const auto& o : *it) {
            if (n++ >= kMaxOverlays) break;
            meta.overlays.push_back(parseOverlay(o));
        }
    }
    if (const auto it = r.find("closeFriends"); it != r.end() && it->is_boolean()) meta.closeFriends = it->get<bool>();
    // Optional and forgiving: a sticker that doesn't fit is dropped, the story still shows.
    if (const auto it = r.find("stickers"); it != r.end()) meta.stickers = parseStickers(it->dump());
    if (kind == "text") {
        const auto text = str(r, "text");
        if (!text || util::trim(util::decodeUtf8(*text)).empty() || utf16Size(*text) > kMaxStoryText) throw StoryFormatError();
        meta.text = *text;
        if (const auto b = str(r, "background"); b && contains(kBackgrounds, *b)) meta.background = *b;
        if (const auto f = str(r, "font"); f && contains(kFonts, *f)) meta.font = *f;
        return meta;
    }
    if (kind != "image" && kind != "video" && kind != "audio") throw StoryFormatError();
    const bool audio = kind == "audio";
    const auto it = r.find("media");
    if (it == r.end() || !it->is_object()) throw StoryFormatError();
    const Json& m = *it;
    MediaRef media;
    const auto key = str(m, "key");
    const auto header = str(m, "header");
    const auto sha = str(m, "sha256");
    static const std::regex kSha("^[0-9a-f]{64}$");
    if (!isB64(key, 64) || !isB64(header, 64) || !sha || !std::regex_match(*sha, kSha)) throw StoryFormatError();
    const auto encryptedSize = positive(m, "encryptedSize");
    const auto chunkSize = positive(m, "chunkSize");
    // A voice story has no picture: its width and height are 1, whatever it says.
    const auto width = audio ? std::optional<std::uint64_t>(1) : positive(m, "width");
    const auto height = audio ? std::optional<std::uint64_t>(1) : positive(m, "height");
    if (!encryptedSize || !chunkSize || !width || !height) throw StoryFormatError();
    if (*width > 2147483647ull || *height > 2147483647ull) throw StoryFormatError();
    const std::string mime = str(m, "mime").value_or("");
    static const std::regex kImage("^image/(jpeg|png|webp)$");
    static const std::regex kVideo("^video/(mp4|webm|quicktime)$");
    static const std::regex kAudio("^audio/(webm|ogg|mp4|mpeg|wav|aac)$");
    if (!std::regex_match(mime, kind == "image" ? kImage : audio ? kAudio : kVideo)) throw StoryFormatError();
    media.key = *key;
    media.header = *header;
    media.sha256 = *sha;
    media.encryptedSize = *encryptedSize;
    media.chunkSize = *chunkSize;
    media.mime = mime;
    media.width = static_cast<int>(*width);
    media.height = static_cast<int>(*height);
    if (kind == "video" || audio) {
        const auto duration = positive(m, "durationMs");
        if (!duration) throw StoryFormatError();
        media.durationMs = static_cast<std::int64_t>(*duration);
    }
    meta.media = std::move(media);
    if (audio) {
        // A look to sit on, an optional title, and the bars that show its loudness.
        meta.background = "night";
        if (const auto b = str(r, "background"); b && contains(kBackgrounds, *b)) meta.background = *b;
        if (const auto f = str(r, "font"); f && contains(kFonts, *f)) meta.font = *f;
        if (const auto t = str(r, "text"); t && !util::trim(util::decodeUtf8(*t)).empty() && utf16Size(*t) <= kMaxStoryText) meta.text = *t;
        if (const auto w = r.find("waveform"); w != r.end()) meta.waveform = waveformFrom(*w);
    }
    if (kind != "image") {
        if (const auto c = r.find("captions"); c != r.end()) meta.captions = captionsFrom(*c);
    }
    return meta;
}

std::string toJson(const Meta& meta) {
    OrderedJson j;
    j["v"] = 1;
    j["kind"] = meta.kind;
    OrderedJson overlays = OrderedJson::array();
    for (const auto& o : meta.overlays) overlays.push_back(overlayJson(o));
    j["overlays"] = std::move(overlays);
    if (!meta.stickers.empty()) j["stickers"] = OrderedJson::parse(toJson(meta.stickers));
    j["closeFriends"] = meta.closeFriends;
    if (meta.kind == "text") {
        j["text"] = meta.text;
        j["background"] = meta.background;
        j["font"] = meta.font;
    } else if (meta.media) {
        if (meta.kind == "audio") {
            j["background"] = meta.background;
            j["font"] = meta.font;
            if (!meta.text.empty()) j["text"] = meta.text;
            if (meta.waveform) j["waveform"] = *meta.waveform;
        }
        if (meta.kind != "image" && !meta.captions.empty()) j["captions"] = captionsJson(meta.captions);
        const auto& m = *meta.media;
        OrderedJson media;
        media["key"] = m.key;
        media["header"] = m.header;
        media["sha256"] = m.sha256;
        media["encryptedSize"] = m.encryptedSize;
        media["chunkSize"] = m.chunkSize;
        media["mime"] = m.mime;
        media["width"] = meta.kind == "audio" ? 1 : m.width;
        media["height"] = meta.kind == "audio" ? 1 : m.height;
        if (m.durationMs) media["durationMs"] = *m.durationMs;
        j["media"] = std::move(media);
    }
    return j.dump();
}

std::vector<Overlay> parseOverlays(std::string_view jsonArray) {
    std::vector<Overlay> out;
    Json raw;
    try {
        raw = Json::parse(jsonArray);
    } catch (const Json::exception&) {
        return out;
    }
    if (!raw.is_array()) return out;
    for (const auto& item : raw) {
        if (out.size() >= kMaxOverlays) break;
        try {
            out.push_back(parseOverlay(item));
        } catch (const StoryFormatError&) {
            // A draft keeps what still fits; the overlay that doesn't is left out.
        }
    }
    return out;
}

bool isKnownKind(std::string_view kind) {
    return kind == "image" || kind == "video" || kind == "text" || kind == "audio";
}

std::vector<Caption> parseCaptions(std::string_view jsonArray) {
    try {
        return captionsFrom(Json::parse(jsonArray));
    } catch (const Json::exception&) {
        return {};
    }
}

std::string toJson(const std::vector<Caption>& captions) {
    return captionsJson(captions).dump();
}

std::optional<std::string> captionAt(const std::vector<Caption>& captions, std::int64_t timeMs) {
    for (const auto& c : captions) {
        if (timeMs >= c.startMs && timeMs < c.endMs) return c.text;
    }
    return std::nullopt;
}

std::optional<std::vector<int>> parseWaveform(std::string_view json) {
    try {
        return waveformFrom(Json::parse(json));
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::string toJson(const std::vector<Overlay>& overlays) {
    OrderedJson j = OrderedJson::array();
    for (const auto& o : overlays) j.push_back(overlayJson(o));
    return j.dump();
}

Prefs parsePrefs(std::string_view json) {
    Json r;
    try {
        r = Json::parse(json);
    } catch (const Json::exception&) {
        return {};
    }
    if (!r.is_object()) return {};
    Prefs p;
    p.closeFriends = uuidList(r, "closeFriends");
    p.hideFrom = uuidList(r, "hideFrom");
    p.muted = uuidList(r, "muted");
    p.shareViews = !(r.contains("shareViews") && r["shareViews"].is_boolean() && !r["shareViews"].get<bool>());
    p.allowReplies = !(r.contains("allowReplies") && r["allowReplies"].is_boolean() && !r["allowReplies"].get<bool>());
    return p;
}

std::string toJson(const Prefs& prefs) {
    OrderedJson j;
    j["closeFriends"] = prefs.closeFriends;
    j["hideFrom"] = prefs.hideFrom;
    j["muted"] = prefs.muted;
    j["shareViews"] = prefs.shareViews;
    j["allowReplies"] = prefs.allowReplies;
    return j.dump();
}

std::vector<std::string> recipients(const std::vector<std::string>& friendIds, const Prefs& prefs, bool closeFriendsOnly) {
    std::vector<std::string> out;
    for (const auto& id : friendIds) {
        if (contains(prefs.hideFrom, id)) continue;
        if (closeFriendsOnly && !contains(prefs.closeFriends, id)) continue;
        out.push_back(id);
    }
    return out;
}

std::vector<std::string> recipients(const std::vector<std::string>& friendIds, const Prefs& prefs, std::string_view audience,
                                    const std::vector<std::string>& customIds) {
    if (audience == "custom") {
        std::vector<std::string> out;
        for (const auto& id : friendIds) {
            if (!contains(prefs.hideFrom, id) && contains(customIds, id)) out.push_back(id);
        }
        return out;
    }
    return recipients(friendIds, prefs, audience == "close_friends");
}

std::string lifetimeLabel(int seconds) {
    if (seconds == 7 * 86400) return "7 days";
    const int hours = static_cast<int>(std::lround(seconds / 3600.0));
    return hours == 1 ? "1 hour" : std::to_string(hours) + " hours";
}

std::string age(std::int64_t createdAtMs, std::int64_t nowMs) {
    const std::int64_t mins = std::max<std::int64_t>(0, (nowMs - createdAtMs) / 60000);
    if (mins < 1) return "now";
    if (mins < 60) return std::to_string(mins) + "m";
    const std::int64_t hours = mins / 60;
    if (hours < 24) return std::to_string(hours) + "h";
    const std::int64_t days = hours / 24;
    return days < 7 ? std::to_string(days) + "d" : std::to_string(days / 7) + "w";
}

std::string preview(const Meta& meta) {
    if (meta.kind == "image") return "Photo";
    if (meta.kind == "video") return "Video";
    if (meta.kind == "audio" && meta.text.empty()) return "Voice";
    return util::encodeUtf8(util::takeUtf16(util::decodeUtf8(meta.text), 60));
}

std::string replyKind(const Meta& meta) {
    return meta.kind == "audio" ? "text" : meta.kind;
}

std::vector<SeenEntry> parseSeen(std::string_view json, std::int64_t nowMs) {
    std::vector<SeenEntry> out;
    Json r;
    try {
        r = Json::parse(json);
    } catch (const Json::exception&) {
        return out;
    }
    if (!r.is_object()) return out;
    for (const auto& [id, at] : r.items()) {
        if (!at.is_number_integer()) continue;
        const auto when = at.get<std::int64_t>();
        if (nowMs - when < kSeenKeepMs) out.push_back({id, when});
    }
    return out;
}

std::string toJson(const std::vector<SeenEntry>& seen) {
    Json j = Json::object();
    for (const auto& s : seen) j[s.storyId] = s.atMs;
    return j.dump();
}

}  // namespace redvault::model::stories
