#include "redvault/model/group_call_link.hpp"

#include <algorithm>
#include <cctype>

namespace redvault::model::group_calls {

namespace {

constexpr std::string_view kJoinParam = "group-call=";

bool isHex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

/// Part of a longer word, so the id can't just be the front of something else.
bool continuesWord(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

bool isId(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : !isHex(text[i])) return false;
    }
    return true;
}

std::optional<std::string> parseCallLink(std::string_view text) {
    for (std::size_t at = text.find(kJoinParam); at != std::string_view::npos; at = text.find(kJoinParam, at + 1)) {
        // Only the group-call parameter itself, not e.g. "start-group-call=".
        if (at == 0 || (text[at - 1] != '?' && text[at - 1] != '&')) continue;
        const auto start = at + kJoinParam.size();
        const auto id = text.substr(start, 36);
        if (!isId(id)) continue;
        if (start + 36 < text.size() && continuesWord(text[start + 36])) continue;
        return lower(id);
    }
    return std::nullopt;
}

std::string joinUrl(std::string_view webAppBase, std::string_view callId) {
    return std::string(webAppBase) + "?" + std::string(kJoinParam) + lower(callId);
}

std::string startUrl(std::string_view webAppBase, std::string_view conversationId, std::string_view kind) {
    if (!isId(conversationId)) return {};
    return std::string(webAppBase) + "?start-group-call=" + lower(conversationId) + "&kind=" + (kind == "video" ? "video" : "voice");
}

std::string inviteText(std::string_view kind, std::string_view url) {
    // U+1F4DE (telephone receiver), as UTF-8 bytes.
    return std::string("\xF0\x9F\x93\x9E") + " Group " + std::string(kind) +
           " call started. To join, open Calls, tap \"Join with a link\" and paste this link:\n" + std::string(url);
}

}  // namespace redvault::model::group_calls

