// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "redvault/model/stories.hpp"

// Templates for a text story: a look (background, font) and often a sticker to fill in, as starting points; everything
// stays editable and nothing is fetched from anywhere. Port of the web app's ui/src/lib/studio/textTemplates.ts (by way
// of Android's studio/TextTemplates.kt), pinned to the web's own output by tests/vectors/studio_vectors.json.
// Animated words (overlay `anim`) are in story_motion.hpp.

namespace redvault::studio::templates {

struct Template {
    std::string id;
    std::string label;
    std::string text;
    std::string background;
    std::string font;
    /// Each makes a sticker given its fresh id and "now" (epoch ms).
    std::vector<std::function<model::stories::Sticker(const std::string& id, std::int64_t nowMs)>> stickers;
    std::vector<model::stories::Overlay> overlays;
};

/// What a template starts a text story from.
struct Applied {
    std::string text;
    std::string background;
    std::string font;
    std::vector<model::stories::Sticker> stickers;
    std::vector<model::stories::Overlay> overlays;
};

/// All eight, in the web's order ("plain" first).
const std::vector<Template>& all();
/// The ones offered as buttons: all but "plain" (which is what a new story already is).
std::vector<const Template*> offered();
const Template* byId(const std::string& id);
/// A template's content with fresh sticker ids, each sticker checked by the same parser a viewer uses (as the web does).
Applied apply(const Template& t, const std::function<std::string()>& newId, std::int64_t nowMs);

}  // namespace redvault::studio::templates
