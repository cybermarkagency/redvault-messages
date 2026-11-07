#include "redvault/model/text_format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace redvault::model::text_format {

namespace {

enum class Style { Bold, Italic, Strike, Spoiler };

struct Marker {
    std::string_view token;
    Style style;
};

// Messages are untrusted: a formatted span is at most this long, and styles
// nest at most this deep (deeper markers are shown as typed).
constexpr std::size_t kMaxSpan = 4000;
constexpr int kMaxDepth = 6;

// "||" before the one-character markers, so a spoiler isn't read as two empty ones.
constexpr std::array<Marker, 4> kMarkers = {{
    {"||", Style::Spoiler},
    {"*", Style::Bold},
    {"_", Style::Italic},
    {"~", Style::Strike},
}};

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// Letters, digits and any non-ASCII byte (part of a word in another script).
bool isWordByte(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) != 0;
}

bool canOpenAfter(std::string_view s, std::size_t i) {
    if (i == 0) return true;
    const char p = s[i - 1];
    return isSpace(p) || std::strchr("([{\"'", p) != nullptr;
}

bool canCloseBefore(std::string_view s, std::size_t j) {
    return j >= s.size() || isSpace(s[j]) || !isWordByte(s[j]);
}

bool startsWithNoCase(std::string_view s, std::size_t at, std::string_view prefix) {
    if (s.size() - at < prefix.size()) return false;
    for (std::size_t k = 0; k < prefix.size(); ++k) {
        if (std::tolower(static_cast<unsigned char>(s[at + k])) != prefix[k]) return false;
    }
    return true;
}

/// End of an http(s) link starting at `i`, or `i` if there's none.
std::size_t linkEnd(std::string_view s, std::size_t i) {
    if (i > 0 && isWordByte(s[i - 1])) return i;
    std::size_t scheme = 0;
    if (startsWithNoCase(s, i, "https://")) scheme = 8;
    else if (startsWithNoCase(s, i, "http://")) scheme = 7;
    else return i;
    std::size_t end = i + scheme;
    while (end < s.size() && !isSpace(s[end]) && std::strchr("<>\"`", s[end]) == nullptr) ++end;
    // Sentence punctuation and formatting markers right after a link aren't part of it.
    int open = 0;
    for (std::size_t k = i; k < end; ++k) open += s[k] == '(' ? 1 : s[k] == ')' ? -1 : 0;
    while (end > i + scheme) {
        const char last = s[end - 1];
        if (last == ')' && open < 0) {
            ++open;
            --end;
        } else if (std::strchr(".,;:!?'*_~|", last) != nullptr) {
            --end;
        } else {
            break;
        }
    }
    return end > i + scheme ? end : i;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(b[k]))) return false;
    }
    return true;
}

/// Length of the handle mentioned by the "@" at `i` (the longest that fits), or 0.
std::size_t mentionAt(std::string_view s, std::size_t i, const std::vector<std::string>& handles, std::string* which) {
    if (s[i] != '@' || (i > 0 && isWordByte(s[i - 1]))) return 0;
    std::size_t best = 0;
    for (const auto& handle : handles) {
        if (handle.empty() || handle.size() <= best || i + 1 + handle.size() > s.size()) continue;
        if (!equalsNoCase(s.substr(i + 1, handle.size()), handle)) continue;
        // "@bob" doesn't mention "bob" inside "@bobby".
        const std::size_t after = i + 1 + handle.size();
        if (after < s.size() && isWordByte(s[after])) continue;
        best = handle.size();
        if (which != nullptr) *which = handle;
    }
    return best;
}

/// Where `marker` (opening at `i`) closes on the same line, or npos.
std::size_t findClose(std::string_view s, std::size_t i, std::string_view token) {
    const std::size_t limit = std::min(s.size(), i + token.size() + kMaxSpan);
    for (std::size_t j = i + token.size() + 1; j + token.size() <= limit; ++j) {
        if (s[j] == '\n') return std::string_view::npos;
        if (s.compare(j, token.size(), token) == 0 && !isSpace(s[j - 1]) && canCloseBefore(s, j + token.size())) {
            return j;
        }
    }
    return std::string_view::npos;
}

class Renderer {
public:
    Renderer(const Options& options, bool plain) : options_(options), plain_(plain) {}

    void render(std::string_view s) {
        std::size_t pos = 0;
        std::size_t i = 0;
        while (i < s.size()) {
            // `monospace`: nothing inside is formatted.
            if (s[i] == '`') {
                const auto close = s.find('`', i + 1);
                const auto newline = s.find('\n', i + 1);
                if (close != std::string_view::npos && close > i + 1 && close < newline) {
                    text(s.substr(pos, i - pos));
                    open("<span style=\"font-family:'" + options_.monoFamily + "'\">");
                    text(s.substr(i + 1, close - i - 1));
                    close_("</span>");
                    i = pos = close + 1;
                    continue;
                }
            }
            if (const auto end = linkEnd(s, i); end > i) {
                text(s.substr(pos, i - pos));
                const auto url = s.substr(i, end - i);
                if (plain_) {
                    out_ += url;
                } else {
                    out_ += "<a href=\"" + escape(url) + "\" style=\"color:" + options_.linkColor + "\">" + escape(url) + "</a>";
                    formatted_ = true;
                }
                i = pos = end;
                continue;
            }
            if (s[i] == '@' && !options_.mentions.empty()) {
                std::string handle;
                if (const auto length = mentionAt(s, i, options_.mentions, &handle); length > 0) {
                    text(s.substr(pos, i - pos));
                    const bool self = !options_.selfHandle.empty() && equalsNoCase(handle, options_.selfHandle);
                    open("<span style=\"color:" + options_.mentionColor + ";font-weight:600" +
                         (self ? ";background-color:" + options_.selfMentionBackground : std::string()) + "\">");
                    text(s.substr(i, length + 1));
                    close_("</span>");
                    i = pos = i + 1 + length;
                    continue;
                }
            }
            bool matched = false;
            for (const auto& marker : kMarkers) {
                if (depth_ >= kMaxDepth) break;
                const auto& token = marker.token;
                if (s.compare(i, token.size(), token) != 0 || !canOpenAfter(s, i)) continue;
                const std::size_t inner = i + token.size();
                if (inner >= s.size() || isSpace(s[inner]) || s[inner] == token[0]) continue;
                const auto close = findClose(s, i, token);
                if (close == std::string_view::npos) continue;
                text(s.substr(pos, i - pos));
                styled(marker.style, s.substr(inner, close - inner));
                i = pos = close + token.size();
                matched = true;
                break;
            }
            if (!matched) ++i;
        }
        text(s.substr(pos));
    }

    std::string result() const { return out_; }
    bool formatted() const { return formatted_; }

private:
    void styled(Style style, std::string_view inner) {
        if (style == Style::Spoiler) {
            if (plain_) {
                out_ += "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";  // •••
            } else if (options_.revealSpoilers) {
                open("<span style=\"background-color:" + options_.spoilerColor + "\">");
                ++depth_;
                render(inner);
                --depth_;
                close_("</span>");
            } else {
                // Hidden: the same colour on the same background, nothing nested (a link would show through).
                open("<a href=\"spoiler:\" style=\"text-decoration:none\"><span style=\"color:" + options_.spoilerColor +
                     ";background-color:" + options_.spoilerColor + "\">");
                text(inner);
                close_("</span></a>");
            }
            return;
        }
        static constexpr std::array<std::string_view, 3> kTags = {"b", "i", "s"};
        const auto tag = kTags[static_cast<std::size_t>(style)];
        open("<" + std::string(tag) + ">");
        ++depth_;
        render(inner);
        --depth_;
        close_("</" + std::string(tag) + ">");
    }

    void open(const std::string& tag) {
        if (plain_) return;
        out_ += tag;
        formatted_ = true;
    }
    void close_(const std::string& tag) {
        if (!plain_) out_ += tag;
    }

    void text(std::string_view s) { out_ += plain_ ? std::string(s) : escape(s); }

    static std::string escape(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (const char c : s) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&#39;"; break;
                case '\n': out += "<br>"; break;
                case '\r': break;
                default: out += c;
            }
        }
        return out;
    }

    // A copy: toPlain passes a temporary, which a reference would outlive (ASan: stack-use-after-scope).
    const Options options_;
    bool plain_;
    std::string out_;
    bool formatted_ = false;
    int depth_ = 0;
};

}  // namespace

std::string toHtml(std::string_view text, const Options& options) {
    Renderer renderer(options, false);
    renderer.render(text);
    if (!renderer.formatted()) return {};
    // Keeps runs of spaces as typed.
    return "<span style=\"white-space:pre-wrap\">" + renderer.result() + "</span>";
}

bool hasFormatting(std::string_view text) {
    return !toHtml(text).empty();
}

bool mentions(std::string_view text, std::string_view handle) {
    const std::vector<std::string> handles{std::string(handle)};
    for (std::size_t i = text.find('@'); i != std::string_view::npos; i = text.find('@', i + 1)) {
        if (mentionAt(text, i, handles, nullptr) > 0) return true;
    }
    return false;
}

std::string toPlain(std::string_view text) {
    Renderer renderer(Options{}, true);
    renderer.render(text);
    return renderer.result();
}

}  // namespace redvault::model::text_format
