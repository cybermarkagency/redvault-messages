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
    return out;
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
    if (kind != "image" && kind != "video") throw StoryFormatError();
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
    const auto width = positive(m, "width");
    const auto height = positive(m, "height");
    if (!encryptedSize || !chunkSize || !width || !height) throw StoryFormatError();
    const std::string mime = str(m, "mime").value_or("");
    static const std::regex kImage("^image/(jpeg|png|webp)$");
    static const std::regex kVideo("^video/(mp4|webm|quicktime)$");
    if (!std::regex_match(mime, kind == "image" ? kImage : kVideo)) throw StoryFormatError();
    media.key = *key;
    media.header = *header;
    media.sha256 = *sha;
    media.encryptedSize = *encryptedSize;
    media.chunkSize = *chunkSize;
    media.mime = mime;
    media.width = static_cast<int>(*width);
    media.height = static_cast<int>(*height);
    if (kind == "video") {
        const auto duration = positive(m, "durationMs");
        if (!duration) throw StoryFormatError();
        media.durationMs = static_cast<std::int64_t>(*duration);
    }
    meta.media = std::move(media);
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
        const auto& m = *meta.media;
        OrderedJson media;
        media["key"] = m.key;
        media["header"] = m.header;
        media["sha256"] = m.sha256;
        media["encryptedSize"] = m.encryptedSize;
        media["chunkSize"] = m.chunkSize;
        media["mime"] = m.mime;
        media["width"] = m.width;
        media["height"] = m.height;
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
    return util::encodeUtf8(util::takeUtf16(util::decodeUtf8(meta.text), 60));
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
