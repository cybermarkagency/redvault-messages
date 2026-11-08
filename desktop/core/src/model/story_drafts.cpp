// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/story_drafts.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "redvault/util/text.hpp"

namespace redvault::model::stories {

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::optional<std::string> str(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

std::optional<std::uint64_t> positiveInt(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    const auto v = it->get<std::int64_t>();
    if (v <= 0 || v > 9007199254740991ll) return std::nullopt;
    return static_cast<std::uint64_t>(v);
}

}  // namespace

SealedDraft makeSealedDraft(DraftContent content, std::optional<DraftMediaRef> media) {
    SealedDraft out;
    out.kind = media ? media->kind : "text";
    // The web app's content.text.trim().slice(0, 60); never splitting an emoji.
    out.preview = media ? (media->kind == "video" ? "Video" : "Photo")
                        : util::encodeUtf8(util::takeUtf16(util::trim(util::decodeUtf8(content.text)), 60));
    out.stickerCount = static_cast<int>(content.stickers.size());
    out.content = std::move(content);
    out.media = std::move(media);
    return out;
}

std::string toJson(const SealedDraft& draft) {
    const DraftContent& c = draft.content;
    OrderedJson content;
    content["mode"] = c.mode;
    content["text"] = c.text;
    content["background"] = c.background;
    content["font"] = c.font;
    content["overlays"] = OrderedJson::parse(toJson(c.overlays));
    content["stickers"] = OrderedJson::parse(toJson(c.stickers));
    content["audience"] = c.audience;
    content["customIds"] = c.customIds;
    content["allowReplies"] = c.allowReplies;
    content["ttlSeconds"] = c.ttlSeconds;

    OrderedJson j;
    j["v"] = 1;
    j["content"] = std::move(content);
    j["summary"] = {{"kind", draft.kind}, {"preview", draft.preview}, {"stickerCount", draft.stickerCount}};
    if (draft.media) {
        const DraftMediaRef& m = *draft.media;
        OrderedJson media;
        media["kind"] = m.kind;
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
    } else {
        j["media"] = nullptr;
    }
    return j.dump();
}

std::optional<SealedDraft> parseSealedDraft(std::string_view json) {
    Json r;
    try {
        r = Json::parse(json);
    } catch (const Json::exception&) {
        return std::nullopt;
    }
    if (!r.is_object()) return std::nullopt;
    const auto v = r.find("v");
    if (v == r.end() || !v->is_number_integer() || v->get<int>() != 1) return std::nullopt;
    const auto contentIt = r.find("content");
    if (contentIt == r.end() || !contentIt->is_object()) return std::nullopt;
    const Json& c = *contentIt;

    SealedDraft out;
    // The photo or video first: a draft that says it has one but can't describe it opens as a text draft, as on the web.
    if (const auto mediaIt = r.find("media"); mediaIt != r.end() && mediaIt->is_object()) {
        const Json& m = *mediaIt;
        DraftMediaRef ref;
        const auto kind = str(m, "kind");
        const auto key = str(m, "key");
        const auto header = str(m, "header");
        const auto sha = str(m, "sha256");
        const auto encryptedSize = positiveInt(m, "encryptedSize");
        const auto chunkSize = positiveInt(m, "chunkSize");
        const auto mime = str(m, "mime");
        const auto width = positiveInt(m, "width");
        const auto height = positiveInt(m, "height");
        if (kind && (*kind == "image" || *kind == "video") && key && header && sha && encryptedSize && chunkSize && mime && width && height) {
            ref.kind = *kind;
            ref.key = *key;
            ref.header = *header;
            ref.sha256 = *sha;
            ref.encryptedSize = *encryptedSize;
            ref.chunkSize = *chunkSize;
            ref.mime = *mime;
            ref.width = static_cast<int>(*width);
            ref.height = static_cast<int>(*height);
            if (const auto d = positiveInt(m, "durationMs")) ref.durationMs = static_cast<std::int64_t>(*d);
            out.media = std::move(ref);
        }
    }

    out.content.mode = str(c, "mode") == std::string("media") && out.media ? "media" : "text";
    out.content.text = str(c, "text").value_or("");
    if (const auto b = str(c, "background"); b && contains(kBackgrounds, *b)) out.content.background = *b;
    if (const auto f = str(c, "font"); f && contains(kFonts, *f)) out.content.font = *f;
    if (const auto it = c.find("overlays"); it != c.end() && it->is_array()) out.content.overlays = parseOverlays(it->dump());
    if (const auto it = c.find("stickers"); it != c.end()) out.content.stickers = parseStickers(it->dump());
    if (const auto a = str(c, "audience"); a && (*a == "close_friends" || *a == "custom")) out.content.audience = *a;
    if (const auto it = c.find("customIds"); it != c.end() && it->is_array()) {
        for (const auto& id : *it) {
            if (id.is_string()) out.content.customIds.push_back(id.get<std::string>());
        }
    }
    if (const auto it = c.find("allowReplies"); it != c.end() && it->is_boolean()) out.content.allowReplies = it->get<bool>();
    if (const auto it = c.find("ttlSeconds"); it != c.end() && it->is_number() && std::isfinite(it->get<double>())) {
        out.content.ttlSeconds = static_cast<int>(it->get<double>());
    }

    out.kind = out.media ? out.media->kind : "text";
    if (const auto it = r.find("summary"); it != r.end() && it->is_object()) {
        out.preview = str(*it, "preview").value_or("");
        if (const auto n = it->find("stickerCount"); n != it->end() && n->is_number_integer()) out.stickerCount = n->get<int>();
    }
    return out;
}

}  // namespace redvault::model::stories
