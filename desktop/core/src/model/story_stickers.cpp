#include "redvault/model/story_stickers.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

#include "redvault/util/text.hpp"

namespace redvault::model::stories {

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

/// JavaScript's \s and String.prototype.trim() set (the web app's cleaning and trimming use it).
bool isJsSpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

/// The characters the web app swaps for a space before tidying: C0 and C1 controls and the bidi overrides and isolates.
bool isStrippedControl(char32_t c) {
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

std::u32string jsTrim(std::u32string_view text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && isJsSpace(text[start])) ++start;
    while (end > start && isJsSpace(text[end - 1])) --end;
    return std::u32string(text.substr(start, end - start));
}

bool isFiniteNumber(const Json& v) {
    return v.is_number() && std::isfinite(v.get<double>());
}

bool isFraction(const Json* v) {
    return v && isFiniteNumber(*v) && v->get<double>() >= 0 && v->get<double>() <= 1;
}

/// Number.isInteger.
bool isInteger(const Json& v) {
    if (!isFiniteNumber(v)) return false;
    const double d = v.get<double>();
    return std::floor(d) == d;
}

const Json* field(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

/// A JSON number that is a sensible time: finite, above zero and below the largest date JavaScript has.
std::optional<double> whenOf(const Json* v) {
    if (!v || !isFiniteNumber(*v)) return std::nullopt;
    const double d = v->get<double>();
    return d > 0 && d < 8.64e15 ? std::optional<double>(d) : std::nullopt;
}

std::optional<std::string> cleanField(const Json* v, std::size_t maxUnits) {
    if (!v || !v->is_string()) return std::nullopt;
    return cleanStickerText(v->get<std::string>(), maxUnits);
}

bool validId(const std::string& id) {
    if (id.size() < 6 || id.size() > 16) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool isUuidLike(const std::string& s) {
    if (s.size() != 36) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

std::optional<Sticker> parseSticker(const Json& r) {
    if (!r.is_object()) return std::nullopt;
    const Json* id = field(r, "id");
    if (!id || !id->is_string() || !validId(id->get<std::string>())) return std::nullopt;
    const Json* x = field(r, "x");
    const Json* y = field(r, "y");
    if (!isFraction(x) || !isFraction(y)) return std::nullopt;

    Sticker s;
    s.id = id->get<std::string>();
    s.x = x->get<double>();
    s.y = y->get<double>();
    if (const Json* scale = field(r, "scale"); scale && isFiniteNumber(*scale)) s.scale = std::min(2.5, std::max(0.6, scale->get<double>()));
    if (const Json* rotation = field(r, "rotation"); rotation && isFiniteNumber(*rotation)) {
        s.rotation = std::fmod(std::fmod(rotation->get<double>(), 360.0) + 360.0, 360.0);
    }
    const Json* results = field(r, "results");
    s.resultsEveryone = !(results && results->is_string() && results->get<std::string>() == "author");

    const Json* type = field(r, "type");
    if (!type || !type->is_string()) return std::nullopt;
    s.type = type->get<std::string>();

    if (s.type == "poll" || s.type == "quiz") {
        const auto question = cleanField(field(r, "question"), kMaxStickerText);
        const Json* options = field(r, "options");
        if (!question || !options || !options->is_array() || options->size() < 2 || options->size() > kMaxStickerOptions) return std::nullopt;
        s.text = *question;
        for (const auto& o : *options) {
            const auto option = cleanField(&o, kMaxStickerOption);
            if (!option) return std::nullopt;
            s.options.push_back(*option);
        }
        if (s.type == "poll") {
            if (const auto closes = whenOf(field(r, "closesAt"))) s.closesAt = *closes;
            return s;
        }
        const Json* correct = field(r, "correct");
        if (!correct || !isInteger(*correct) || correct->get<double>() < 0 || correct->get<double>() >= static_cast<double>(s.options.size())) return std::nullopt;
        s.correct = static_cast<int>(correct->get<double>());
        if (const auto explanation = cleanField(field(r, "explanation"), kMaxStickerText)) s.explanation = *explanation;
        return s;
    }
    if (s.type == "question") {
        const auto prompt = cleanField(field(r, "prompt"), kMaxStickerText);
        if (!prompt) return std::nullopt;
        s.text = *prompt;
        const Json* anonymous = field(r, "anonymous");
        s.anonymous = anonymous && anonymous->is_boolean() && anonymous->get<bool>();
        return s;
    }
    if (s.type == "slider") {
        const auto prompt = cleanField(field(r, "prompt"), kMaxStickerText);
        const auto emoji = cleanField(field(r, "emoji"), 8);
        if (!prompt || !emoji) return std::nullopt;
        s.text = *prompt;
        s.emoji = *emoji;
        return s;
    }
    if (s.type == "countdown") {
        const auto title = cleanField(field(r, "title"), 60);
        const auto endsAt = whenOf(field(r, "endsAt"));
        if (!title || !endsAt) return std::nullopt;
        s.text = *title;
        s.when = *endsAt;
        return s;
    }
    if (s.type == "link") {
        const Json* url = field(r, "url");
        const auto link = url && url->is_string() ? safeStoryLink(url->get<std::string>()) : std::nullopt;
        const auto label = cleanField(field(r, "label"), 40);
        if (!link || !label) return std::nullopt;
        s.url = *link;
        s.text = *label;
        return s;
    }
    if (s.type == "mention") {
        const Json* userId = field(r, "userId");
        if (!userId || !userId->is_string() || !isUuidLike(userId->get<std::string>())) return std::nullopt;
        s.userId = util::toLowerAscii(userId->get<std::string>());
        return s;
    }
    if (s.type == "date") {
        const auto at = whenOf(field(r, "at"));
        const Json* style = field(r, "style");
        if (!at || !style || !style->is_string()) return std::nullopt;
        const std::string styleName = style->get<std::string>();
        if (styleName != "date" && styleName != "time" && styleName != "datetime") return std::nullopt;
        s.when = *at;
        s.style = styleName;
        return s;
    }
    if (s.type == "location") {
        const auto label = cleanField(field(r, "label"), 60);
        if (!label) return std::nullopt;
        s.text = *label;
        return s;
    }
    // A sticker type from a newer app: left out, the rest of the story still shows.
    return std::nullopt;
}

/// A whole number prints without a fraction ("1900000000000", not "1.9e12").
Json numberJson(double d) {
    if (std::floor(d) == d && std::fabs(d) < 9.0e15) return Json(static_cast<std::int64_t>(d));
    return Json(d);
}

OrderedJson stickerJson(const Sticker& s) {
    OrderedJson j;
    j["id"] = s.id;
    j["x"] = s.x;
    j["y"] = s.y;
    j["scale"] = s.scale;
    j["rotation"] = s.rotation;
    j["type"] = s.type;
    const char* results = s.resultsEveryone ? "everyone" : "author";
    if (s.type == "poll") {
        j["question"] = s.text;
        j["options"] = s.options;
        j["results"] = results;
        if (s.closesAt) j["closesAt"] = numberJson(*s.closesAt);
    } else if (s.type == "quiz") {
        j["question"] = s.text;
        j["options"] = s.options;
        j["correct"] = s.correct;
        j["results"] = results;
        if (!s.explanation.empty()) j["explanation"] = s.explanation;
    } else if (s.type == "question") {
        j["prompt"] = s.text;
        j["anonymous"] = s.anonymous;
    } else if (s.type == "slider") {
        j["prompt"] = s.text;
        j["emoji"] = s.emoji;
        j["results"] = results;
    } else if (s.type == "countdown") {
        j["title"] = s.text;
        j["endsAt"] = numberJson(s.when);
    } else if (s.type == "link") {
        j["url"] = s.url;
        j["label"] = s.text;
    } else if (s.type == "mention") {
        j["userId"] = s.userId;
    } else if (s.type == "date") {
        j["at"] = numberJson(s.when);
        j["style"] = s.style;
    } else if (s.type == "location") {
        j["label"] = s.text;
    }
    return j;
}

/// How many answers or options: a whole number from 0 to a million (the web app's `count`).
std::optional<int> countOf(const Json* v) {
    if (!v || !isInteger(*v)) return std::nullopt;
    const double d = v->get<double>();
    if (d < 0 || d > 1'000'000) return std::nullopt;
    return static_cast<int>(d);
}

bool isDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool isLabel(std::string_view label) {
    if (label.empty() || label.size() > 63) return false;
    if (label.front() == '-' || label.back() == '-') return false;
    return std::all_of(label.begin(), label.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::vector<std::string> splitOn(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = s.find(sep, start);
        if (at == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            return out;
        }
        out.emplace_back(s.substr(start, at - start));
        start = at + 1;
    }
}

/// A lowercase host that is either a name with a real top-level label or a plain dotted IPv4 address.
bool validHost(const std::string& host) {
    if (host.empty() || host.size() > 253 || host.find('.') == std::string::npos) return false;
    const auto labels = splitOn(host, '.');
    for (const auto& l : labels) {
        if (!isLabel(l)) return false;
    }
    const std::string& last = labels.back();
    if (!isDigits(last)) {
        return std::any_of(last.begin(), last.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    }
    // All digits at the end: only a canonical dotted quad is an address; "1.2.3" or "0x7f.1" style tricks are refused.
    if (labels.size() != 4) return false;
    for (const auto& l : labels) {
        if (!isDigits(l) || l.size() > 3 || (l.size() > 1 && l[0] == '0') || std::stoi(l) > 255) return false;
    }
    return true;
}

bool dotSegment(std::string segment) {
    for (auto& c : segment) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return segment == "." || segment == ".." || segment == "%2e" || segment == ".%2e" || segment == "%2e." || segment == "%2e%2e";
}

}  // namespace

bool isAnswerable(const Sticker& s) {
    return s.type == "poll" || s.type == "quiz" || s.type == "question" || s.type == "slider";
}

bool isAnonymousSticker(const Sticker& s) {
    return s.type == "question" && s.anonymous;
}

bool isFinalSticker(const Sticker& s) {
    return s.type == "quiz" || s.type == "question";
}

bool isClosed(const Sticker& s, std::int64_t nowMs) {
    return s.type == "poll" && s.closesAt && static_cast<double>(nowMs) >= *s.closesAt;
}

std::optional<std::string> cleanStickerText(std::string_view utf8, std::size_t maxUnits) {
    std::u32string text = util::decodeUtf8(utf8);
    for (auto& c : text) {
        if (isStrippedControl(c)) c = U' ';
    }
    // \s+ becomes one space (even a single non-breaking space), then the ends are trimmed.
    std::u32string tidy;
    tidy.reserve(text.size());
    bool inRun = false;
    for (const char32_t c : text) {
        if (isJsSpace(c)) {
            if (!inRun) tidy.push_back(U' ');
            inRun = true;
        } else {
            tidy.push_back(c);
            inRun = false;
        }
    }
    tidy = jsTrim(tidy);
    if (tidy.empty() || util::utf16Length(tidy) > maxUnits) return std::nullopt;
    return util::encodeUtf8(tidy);
}

std::optional<std::string> safeStoryLink(std::string_view raw) {
    if (util::utf16Length(util::decodeUtf8(raw)) > kMaxLinkLength) return std::nullopt;
    const std::string s = util::encodeUtf8(jsTrim(util::decodeUtf8(raw)));
    // Plain printable ASCII only, and none of the characters a URL parser would rewrite or reinterpret.
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F) return std::nullopt;
        if (std::string_view("\"<>`{}^'\\").find(c) != std::string_view::npos) return std::nullopt;
    }
    constexpr std::string_view kScheme = "https://";
    if (s.size() <= kScheme.size() || util::toLowerAscii(s.substr(0, kScheme.size())) != kScheme) return std::nullopt;
    const std::string rest = s.substr(kScheme.size());

    const std::size_t authorityEnd = rest.find_first_of("/?#");
    const std::string authority = rest.substr(0, authorityEnd);
    const std::string tail = authorityEnd == std::string::npos ? std::string() : rest.substr(authorityEnd);
    // Credentials and IPv6 literals are out; so is anything else with an "@" or a bracket.
    if (authority.find_first_of("@[]") != std::string::npos) return std::nullopt;

    std::string host = authority;
    std::optional<int> port;
    if (const std::size_t colon = authority.find(':'); colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string digits = authority.substr(colon + 1);
        if (!isDigits(digits) || digits.size() > 5) return std::nullopt;
        const int value = std::stoi(digits);
        if (value < 1 || value > 65535) return std::nullopt;
        port = value;
    }
    host = util::toLowerAscii(host);
    if (!validHost(host)) return std::nullopt;

    const std::string pathPart = tail.substr(0, tail.find_first_of("?#"));
    for (const auto& segment : splitOn(pathPart, '/')) {
        if (dotSegment(segment)) return std::nullopt;
    }

    std::string out = "https://" + host;
    if (port && *port != 443) out += ":" + std::to_string(*port);
    if (tail.empty() || tail[0] != '/') out += '/';
    out += tail;
    return out;
}

std::vector<Sticker> parseStickers(std::string_view jsonArray) {
    std::vector<Sticker> out;
    Json raw;
    try {
        raw = Json::parse(jsonArray);
    } catch (const Json::exception&) {
        return out;
    }
    if (!raw.is_array()) return out;
    std::set<std::string> seen;
    std::size_t looked = 0;
    for (const auto& item : raw) {
        if (looked++ >= kMaxStickers) break;
        auto sticker = parseSticker(item);
        if (sticker && seen.insert(sticker->id).second) out.push_back(std::move(*sticker));
    }
    return out;
}

std::string toJson(const Sticker& sticker) {
    return stickerJson(sticker).dump();
}

std::string toJson(const std::vector<Sticker>& stickers) {
    OrderedJson j = OrderedJson::array();
    for (const auto& s : stickers) j.push_back(stickerJson(s));
    return j.dump();
}

// --- Answers and totals ------------------------------------------------------------------------

Answer checkedAnswer(const Answer& answer, const Sticker& sticker) {
    if (!isAnswerable(sticker) || answer.kind != sticker.type) throw StoryFormatError();
    Answer out;
    out.kind = sticker.type;
    if (sticker.type == "poll" || sticker.type == "quiz") {
        if (answer.choice < 0 || answer.choice >= static_cast<int>(sticker.options.size())) throw StoryFormatError();
        out.choice = answer.choice;
    } else if (sticker.type == "slider") {
        if (answer.value < 0 || answer.value > 100) throw StoryFormatError();
        out.value = answer.value;
    } else {
        const std::u32string text = jsTrim(util::decodeUtf8(answer.text));
        if (text.empty() || util::utf16Length(text) > kMaxQuestionAnswer) throw StoryFormatError();
        out.text = util::encodeUtf8(text);
    }
    return out;
}

Answer parseAnswer(std::string_view json, const Sticker& sticker) {
    Json r;
    try {
        r = Json::parse(json);
    } catch (const Json::exception&) {
        throw StoryFormatError();
    }
    if (!r.is_object()) throw StoryFormatError();
    const Json* k = field(r, "k");
    if (!k || !k->is_string() || k->get<std::string>() != sticker.type) throw StoryFormatError();
    Answer a;
    a.kind = sticker.type;
    if (sticker.type == "poll" || sticker.type == "quiz") {
        const Json* choice = field(r, "choice");
        if (!choice || !isInteger(*choice) || choice->get<double>() < 0 || choice->get<double>() >= static_cast<double>(sticker.options.size())) {
            throw StoryFormatError();
        }
        a.choice = static_cast<int>(choice->get<double>());
    } else if (sticker.type == "slider") {
        const Json* value = field(r, "value");
        if (!value || !isInteger(*value) || value->get<double>() < 0 || value->get<double>() > 100) throw StoryFormatError();
        a.value = static_cast<int>(value->get<double>());
    } else if (sticker.type == "question") {
        const Json* text = field(r, "text");
        const std::u32string trimmed = text && text->is_string() ? jsTrim(util::decodeUtf8(text->get<std::string>())) : std::u32string();
        if (trimmed.empty() || util::utf16Length(trimmed) > kMaxQuestionAnswer) throw StoryFormatError();
        a.text = util::encodeUtf8(trimmed);
    } else {
        throw StoryFormatError();
    }
    return a;
}

std::string toJson(const Answer& answer) {
    OrderedJson j;
    j["k"] = answer.kind;
    if (answer.kind == "poll" || answer.kind == "quiz") j["choice"] = answer.choice;
    else if (answer.kind == "slider") j["value"] = answer.value;
    else j["text"] = answer.text;
    return j.dump();
}

Totals parseTotals(std::string_view json, const Sticker& sticker) {
    Json raw;
    try {
        raw = Json::parse(json);
    } catch (const Json::exception&) {
        throw StoryFormatError();
    }
    if (!raw.is_object()) throw StoryFormatError();
    const auto total = countOf(field(raw, "total"));
    if (!total) throw StoryFormatError();
    Totals t;
    t.total = *total;
    if (sticker.type == "poll" || sticker.type == "quiz") {
        const Json* counts = field(raw, "counts");
        if (!counts || !counts->is_array() || counts->size() != sticker.options.size()) throw StoryFormatError();
        for (const auto& c : *counts) {
            const auto n = countOf(&c);
            if (!n) throw StoryFormatError();
            t.counts.push_back(*n);
        }
        if (sticker.type == "quiz") {
            const auto correct = countOf(field(raw, "correct"));
            if (!correct) throw StoryFormatError();
            t.correct = *correct;
        }
    } else if (sticker.type == "slider") {
        const Json* average = field(raw, "average");
        if (!average || !isFiniteNumber(*average) || average->get<double>() < 0 || average->get<double>() > 100) throw StoryFormatError();
        t.average = average->get<double>();
    }
    return t;
}

std::string toJson(const Totals& totals) {
    OrderedJson j;
    j["total"] = totals.total;
    if (!totals.counts.empty()) j["counts"] = totals.counts;
    if (totals.correct) j["correct"] = *totals.correct;
    if (totals.average) j["average"] = *totals.average;
    return j.dump();
}

Totals tally(const Sticker& sticker, const std::vector<InboxEntry>& entries) {
    Totals t;
    if (sticker.type == "poll" || sticker.type == "quiz") t.counts.assign(sticker.options.size(), 0);
    if (sticker.type == "quiz") t.correct = 0;
    double sum = 0;
    int sliders = 0;
    for (const auto& e : entries) {
        if (e.stickerId != sticker.id) continue;
        if (sticker.type == "poll") {
            if (e.answer.kind != "poll") continue;
            if (sticker.closesAt && static_cast<double>(e.atMs) > *sticker.closesAt) continue;
            if (e.answer.choice < 0 || e.answer.choice >= static_cast<int>(t.counts.size())) continue;
            ++t.counts[static_cast<std::size_t>(e.answer.choice)];
            ++t.total;
        } else if (sticker.type == "quiz") {
            if (e.answer.kind != "quiz" || e.revision > 0) continue;
            if (e.answer.choice < 0 || e.answer.choice >= static_cast<int>(t.counts.size())) continue;
            ++t.counts[static_cast<std::size_t>(e.answer.choice)];
            ++t.total;
            if (e.answer.choice == sticker.correct) ++*t.correct;
        } else if (sticker.type == "slider") {
            if (e.answer.kind != "slider") continue;
            sum += e.answer.value;
            ++sliders;
        } else if (sticker.type == "question") {
            if (e.answer.kind == "question") ++t.total;
        }
    }
    if (sticker.type == "slider") {
        t.total = sliders;
        // Math.round((sum / n) * 10) / 10
        t.average = sliders > 0 ? std::floor((sum / sliders) * 10 + 0.5) / 10 : 0.0;
    }
    return t;
}

}  // namespace redvault::model::stories
