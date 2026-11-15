// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/studio/text_templates.hpp"

namespace redvault::studio::templates {

namespace st = model::stories;

namespace {

constexpr double kX = 0.5;
constexpr double kStickerY = 0.62;
constexpr std::int64_t kWeekMs = 7LL * 86'400'000;

st::Overlay emoji(const char* text, double x, double y, double scale) {
    st::Overlay o;
    o.type = "emoji";
    o.text = text;
    o.x = x;
    o.y = y;
    o.scale = scale;
    return o;
}

st::Sticker base(const std::string& id, const char* type) {
    st::Sticker s;
    s.type = type;
    s.id = id;
    s.x = kX;
    s.y = kStickerY;
    return s;
}

}  // namespace

const std::vector<Template>& all() {
    static const std::vector<Template> list = [] {
        std::vector<Template> t;
        t.push_back({"plain", "Plain", "", "sunset", "classic", {}, {}});
        t.push_back({"this-or-that", "This or that", "This or that?", "ocean", "strong",
                     {[](const std::string& id, std::int64_t) {
                         auto s = base(id, "poll");
                         s.text = "Which one?";
                         s.options = {"This", "That"};
                         s.resultsEveryone = true;
                         return s;
                     }},
                     {}});
        t.push_back({"ask-me", "Ask me anything", "Ask me anything", "berry", "modern",
                     {[](const std::string& id, std::int64_t) {
                         auto s = base(id, "question");
                         s.text = "Ask me anything";
                         s.anonymous = false;
                         return s;
                     }},
                     {}});
        t.push_back({"mood", "Mood check", "How are you today?", "mint", "modern",
                     {[](const std::string& id, std::int64_t) {
                         auto s = base(id, "slider");
                         s.text = "Mood";
                         s.emoji = "\xF0\x9F\x98\x8D";  // smiling face with heart-eyes
                         s.resultsEveryone = true;
                         return s;
                     }},
                     {}});
        t.push_back({"countdown", "Countdown", "Coming soon", "night", "strong",
                     {[](const std::string& id, std::int64_t now) {
                         auto s = base(id, "countdown");
                         s.text = "The big day";
                         s.when = static_cast<double>(now + kWeekMs);
                         return s;
                     }},
                     {emoji("\xF0\x9F\x8E\x89", 0.5, 0.18, 1.4)}});  // party popper
        t.push_back({"announcement", "Announcement", "Big news!", "ember", "strong", {}, {emoji("\xF0\x9F\x93\xA3", 0.5, 0.2, 1.6)}});  // megaphone
        t.push_back({"gratitude", "Grateful for", "Today I'm grateful for\xE2\x80\xA6", "forest", "classic", {},
                     {emoji("\xF0\x9F\x8C\xB1", 0.5, 0.78, 1.4)}});  // seedling
        t.push_back({"quote", "Quote", "\xE2\x80\x9CWrite something worth remembering.\xE2\x80\x9D", "slate", "typewriter", {}, {}});
        return t;
    }();
    return list;
}

std::vector<const Template*> offered() {
    std::vector<const Template*> out;
    for (const auto& t : all()) {
        if (t.id != "plain") out.push_back(&t);
    }
    return out;
}

const Template* byId(const std::string& id) {
    for (const auto& t : all()) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

Applied apply(const Template& t, const std::function<std::string()>& newId, std::int64_t nowMs) {
    std::vector<st::Sticker> made;
    for (const auto& make : t.stickers) made.push_back(make(newId(), nowMs));
    return Applied{t.text, t.background, t.font, st::parseStickers(st::toJson(made)), t.overlays};
}

}  // namespace redvault::studio::templates
