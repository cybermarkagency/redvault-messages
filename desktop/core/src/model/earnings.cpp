// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/earnings.hpp"

#include <cstdio>
#include <regex>

#include <nlohmann/json.hpp>

#include "redvault/util/text.hpp"

namespace redvault::model::earnings {

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

std::optional<std::string> str(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

std::optional<std::uint64_t> positive(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    const auto value = it->get<std::int64_t>();
    // Number.isSafeInteger and > 0.
    if (value <= 0 || value > 9007199254740991ll) return std::nullopt;
    return static_cast<std::uint64_t>(value);
}

bool okB64(const std::optional<std::string>& v) {
    return v && !v->empty() && v->size() <= 64;
}

}  // namespace

PostBody parseBody(std::string_view json) {
    Json r;
    try {
        r = Json::parse(json);
    } catch (const Json::exception&) {
        throw PostFormatError();
    }
    if (!r.is_object() || !r.contains("v") || !r["v"].is_number_integer() || r["v"].get<int>() != 1) throw PostFormatError();
    const auto text = str(r, "text");
    if (!text || util::utf16Length(util::decodeUtf8(*text)) > kMaxPostText) throw PostFormatError();
    PostBody body;
    body.text = *text;
    const auto it = r.find("media");
    if (it == r.end()) return body;
    if (!it->is_object()) throw PostFormatError();
    const Json& m = *it;
    const auto key = str(m, "key");
    const auto header = str(m, "header");
    const auto sha = str(m, "sha256");
    static const std::regex kSha("^[0-9a-f]{64}$");
    if (!okB64(key) || !okB64(header) || !sha || !std::regex_match(*sha, kSha)) throw PostFormatError();
    const auto encryptedSize = positive(m, "encryptedSize");
    const auto chunkSize = positive(m, "chunkSize");
    if (!encryptedSize || !chunkSize) throw PostFormatError();
    const std::string mime = str(m, "mime").value_or("");
    static const std::regex kMime("^(image/(jpeg|png|webp)|video/(mp4|webm|quicktime))$");
    if (!std::regex_match(mime, kMime)) throw PostFormatError();
    body.media = PostMedia{*key, *header, *sha, *encryptedSize, *chunkSize, mime};
    return body;
}

std::string toJson(const PostBody& body) {
    OrderedJson j;
    j["v"] = 1;
    j["text"] = body.text;
    if (body.media) {
        const auto& m = *body.media;
        OrderedJson media;
        media["key"] = m.key;
        media["header"] = m.header;
        media["sha256"] = m.sha256;
        media["encryptedSize"] = m.encryptedSize;
        media["chunkSize"] = m.chunkSize;
        media["mime"] = m.mime;
        j["media"] = std::move(media);
    }
    return j.dump();
}

std::string formatUsd(std::int64_t minor) {
    const bool negative = minor < 0;
    const std::uint64_t abs = negative ? static_cast<std::uint64_t>(-(minor + 1)) + 1 : static_cast<std::uint64_t>(minor);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s$%llu.%02llu", negative ? "-" : "", static_cast<unsigned long long>(abs / 100),
                  static_cast<unsigned long long>(abs % 100));
    return buf;
}

std::optional<std::int64_t> parseUsd(std::string_view text) {
    std::string s(text);
    // Trim spaces and an optional leading "$".
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return std::nullopt;
    const auto last = s.find_last_not_of(" \t");
    s = s.substr(first, last - first + 1);
    if (!s.empty() && s.front() == '$') s.erase(0, 1);
    static const std::regex kAmount("^([0-9]{1,7})(?:\\.([0-9]{0,2}))?$");
    std::smatch m;
    if (!std::regex_match(s, m, kAmount)) return std::nullopt;
    std::int64_t cents = std::stoll(m[1].str()) * 100;
    if (m[2].matched && !m[2].str().empty()) {
        const std::string frac = m[2].str();
        cents += frac.size() == 1 ? (frac[0] - '0') * 10 : std::stoi(frac);
    }
    return cents;
}

std::string statusName(std::string_view status) {
    if (status == "checkout_pending") return "Awaiting payment";
    if (status == "paid") return "Paid";
    if (status == "consuming") return "Sending";
    if (status == "consumed") return "Delivered";
    if (status == "refund_pending") return "Refunding";
    if (status == "refunded") return "Refunded";
    if (status == "disputed") return "Disputed";
    if (status == "expired") return "Expired";
    if (status == "canceled") return "Canceled";
    if (status == "failed") return "Failed";
    return std::string(status);
}

std::string productName(std::string_view product) {
    if (product == "paid_message") return "Paid messages";
    if (product == "tip") return "Tips";
    if (product == "exclusive_post") return "Exclusive posts";
    return std::string(product);
}

}  // namespace redvault::model::earnings
