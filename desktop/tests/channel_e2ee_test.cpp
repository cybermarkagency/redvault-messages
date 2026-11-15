// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Private channels encrypted with the link (Part C1,
// docs/signal-integration/10-part-c1-private-channels.md): the crypto pinned
// to the shared vectors (vectors/channel_e2ee_vectors.json, which the
// Android tests read too), and the device side (data/private_channels.hpp)
// run against a small in-memory stand-in for the server's channel routes:
// posts, edits, scheduled posts, polls, checklists, media, suggestions, a
// link reset, both visibility switches and an old private channel's keys.
#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "test_helpers.hpp"
#include "redvault/api/channel_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/channel_crypto.hpp"
#include "redvault/crypto/channel_e2ee.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/data/private_channels.hpp"

using namespace redvault;
using redvault::test::b64;
using redvault::test::FakeTransport;
using Json = nlohmann::json;
namespace e2ee = redvault::crypto::channel_e2ee;
namespace links = redvault::crypto::channels;
namespace channels = redvault::api::channels;
namespace pc = redvault::data::private_channels;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_CHANNEL_E2EE_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

crypto::Bytes bytesOf(const crypto::SecretBytes& s) {
    return {s.data(), s.data() + s.size()};
}

}  // namespace

// --- The shared vectors ---

TEST_CASE("channel E2EE: wrapKey and the access token match the vectors", "[channels][e2ee][vectors]") {
    const auto& v = vectors();
    const std::string secret = v["secretText"];
    CHECK(bytesOf(e2ee::wrapKey(secret)) == b64(v["wrapKey"].get<std::string>()));
    CHECK(links::accessToken(secret) == v["accessToken"].get<std::string>());
    CHECK(e2ee::keyAd(v["channelId"].get<std::string>(), v["epoch"]) == v["keyAd"].get<std::string>());
    CHECK(e2ee::postAd(v["channelId"].get<std::string>(), v["epoch"]) == v["postAd"].get<std::string>());
    // The AD uses the lowercase id whatever case it came in.
    std::string upper = v["channelId"];
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    CHECK(e2ee::postAd(upper, v["epoch"]) == v["postAd"].get<std::string>());
}

TEST_CASE("channel E2EE: the wrapped key and the post reproduce and open", "[channels][e2ee][vectors]") {
    const auto& v = vectors();
    const std::string channelId = v["channelId"];
    const int epoch = v["epoch"];
    const auto wrapKey = e2ee::wrapKey(v["secretText"].get<std::string>());
    const auto contentKey = b64(v["contentKey"].get<std::string>());
    const auto wrapped = b64(v["wrapped"].get<std::string>());

    // Wrapping with the vector's nonce gives the same bytes.
    const crypto::Bytes wrapNonce(wrapped.begin(), wrapped.begin() + 24);
    CHECK(e2ee::wrap(contentKey, wrapKey, channelId, epoch, crypto::ByteView(wrapNonce)) == v["wrapped"].get<std::string>());

    const auto opened = e2ee::unwrap(v["wrapped"].get<std::string>(), wrapKey, channelId, epoch);
    REQUIRE(opened);
    CHECK(bytesOf(*opened) == contentKey);

    const auto& post = v["post"];
    const auto plaintext = e2ee::decrypt(post["nonce"].get<std::string>(), post["ciphertext"].get<std::string>(), *opened, channelId,
                                         post["keyEpoch"]);
    REQUIRE(plaintext);
    CHECK(*plaintext == v["plaintext"].get<std::string>());

    const auto nonce = b64(post["nonce"].get<std::string>());
    const auto sealed = e2ee::encrypt(v["plaintext"].get<std::string>(), contentKey, channelId, epoch, crypto::ByteView(nonce));
    CHECK(sealed.nonce == post["nonce"].get<std::string>());
    CHECK(sealed.ciphertext == post["ciphertext"].get<std::string>());

    // The plaintext is version 1, with the poll in the post's own shape.
    const auto content = pc::parsePlaintext(*plaintext);
    REQUIRE(content);
    CHECK(content->body == "Hello, **followers** \xF0\x9F\x91\x8B");
    REQUIRE(content->poll);
    CHECK(content->poll->question == "Next topic?");
    CHECK(content->poll->options == std::vector<std::string>{"A", "B"});
    CHECK_FALSE(content->poll->multiple);
    CHECK_FALSE(content->poll->quiz);
}

TEST_CASE("channel E2EE: every failure case refuses to open", "[channels][e2ee][vectors]") {
    const auto& v = vectors();
    const auto wrapKey = e2ee::wrapKey(v["secretText"].get<std::string>());
    const auto contentKey = b64(v["contentKey"].get<std::string>());
    REQUIRE(v["failures"].size() == 6);
    for (const auto& f : v["failures"]) {
        INFO(f["name"].get<std::string>());
        if (f["kind"] == "unwrap") {
            CHECK_FALSE(e2ee::unwrap(f["wrapped"].get<std::string>(), wrapKey, f["channelId"].get<std::string>(), f["epoch"]));
        } else {
            CHECK_FALSE(e2ee::decrypt(f["nonce"].get<std::string>(), f["ciphertext"].get<std::string>(), contentKey,
                                      f["channelId"].get<std::string>(), f["keyEpoch"]));
        }
    }
    // And a wrong link: another secret's wrapKey opens nothing.
    const auto other = e2ee::wrapKey(links::newSecret());
    CHECK_FALSE(e2ee::unwrap(v["wrapped"].get<std::string>(), other, v["channelId"].get<std::string>(), v["epoch"]));
}

TEST_CASE("channel E2EE: the plaintext round-trips and counts length as the server does", "[channels][e2ee]") {
    pc::Content c;
    c.body = "caf\xC3\xA9 \xF0\x9F\x91\x8B";
    c.checklist = channels::NewPost::NewChecklist{"Trip", {"Tickets", "Bags"}, true};
    const auto back = pc::parsePlaintext(pc::toPlaintext(c));
    REQUIRE(back);
    CHECK(back->body == c.body);
    REQUIRE(back->checklist);
    CHECK(back->checklist->items == std::vector<std::string>{"Tickets", "Bags"});
    CHECK(back->checklist->collaborative);
    CHECK(pc::utf16Length(c.body) == 7);  // c a f é space + a surrogate pair

    pc::Content media;
    channels::Media m;
    m.id = "AAAAAAAA-0000-4000-8000-000000000001";
    m.kind = "photo";
    m.mime = "image/jpeg";
    m.size = 3;
    m.filename = "a.jpg";
    m.width = 4;
    m.height = 2;
    m.secret = channels::MediaKey{"a2V5", "aGVhZGVy", "ABCDEF", 3};
    media.media.push_back(m);
    const auto mediaBack = pc::parsePlaintext(pc::toPlaintext(media));
    REQUIRE(mediaBack);
    REQUIRE(mediaBack->media.size() == 1);
    CHECK(mediaBack->media[0].id == "aaaaaaaa-0000-4000-8000-000000000001");
    CHECK(mediaBack->media[0].secret->sha256 == "abcdef");
    CHECK(mediaBack->media[0].filename == "a.jpg");

    CHECK_FALSE(pc::parsePlaintext(R"({"v":2,"body":"x","content":null})"));
    CHECK_FALSE(pc::parsePlaintext("not json"));
    CHECK_FALSE(pc::parsePlaintext(R"({"v":1,"content":null})"));
}

// --- The device side against a stand-in server ---

namespace {

const std::string kChannel = "cccccccc-1111-4111-8111-111111111111";
const std::string kSig(43, 'S');

std::string uuid(int n) {
    char out[40];
    std::snprintf(out, sizeof out, "00000000-0000-4000-8000-%012d", n);
    return out;
}

/// Just enough of server/src/services/channelService.ts and channelMediaService.ts for one channel and its owner.
class FakeChannelServer {
public:
    struct MediaItem {
        std::string bytes;
        bool encrypted = false;
        Json meta;
        bool used = false;
        bool uploaded = false;
    };
    struct PostRow {
        std::string id;
        std::string createdAt;
        bool scheduled = false;
        // Plain
        std::string body;
        Json content;  // null, {type: poll|checklist|media ...}
        // Encrypted
        Json encrypted;  // null or {keyEpoch, nonce, ciphertext}
        Json encMeta;    // {kind, ...}
        std::vector<std::string> media;
        std::map<std::string, std::vector<int>> votes;  // voter -> options
    };

    bool isPrivate = false;
    std::optional<int> keyEpoch;
    std::optional<std::string> accessToken;
    std::optional<std::string> secretSealed;
    std::optional<std::string> formerSecretSealed;
    std::vector<channels::WrappedKey> keys;
    std::vector<PostRow> posts;
    std::map<std::string, MediaItem> media;
    std::vector<Json> suggestions;
    /// The next N edits answer 429.
    int rateLimitEdits = 0;
    int editsRateLimited = 0;
    int counter = 100;

    std::string time() { return "2026-11-08T10:" + std::to_string(10 + (counter % 50)) + ":00.000Z"; }

    net::HttpResponse handle(const net::HttpRequest& r) {
        const std::string base = "https://example.com/api/";
        std::string path = r.url.substr(base.size());
        std::string query;
        if (const auto q = path.find('?'); q != std::string::npos) {
            query = path.substr(q + 1);
            path = path.substr(0, q);
        }
        const std::string access = FakeTransport::headerOf(r, "x-channel-access");
        const Json body = (r.body.empty() || (r.method == "PUT" && path.starts_with("channel-uploads/"))) ? Json() : Json::parse(r.body);
        const std::string ch = "channels/" + kChannel;
        const std::string pub = "public/channels/" + kChannel;
        const bool readable = !isPrivate || (accessToken && access == *accessToken);

        if (r.method == "GET" && path == pub) return readable ? ok(channelJson(false)) : notFound();
        if (r.method == "GET" && path == pub + "/posts") {
            if (!readable) return notFound();
            Json list = Json::array();
            for (auto it = posts.rbegin(); it != posts.rend(); ++it) {
                if (it->scheduled) continue;
                if (!isPrivate && !it->encrypted.is_null()) continue;
                list.push_back(postJson(*it));
            }
            return ok(list);
        }
        if (r.method == "GET" && path == pub + "/keys") {
            if (!isPrivate || !accessToken || access != *accessToken) return notFound();
            return ok(keysJson());
        }
        if (r.method == "GET" && path == "channels/mine") return ok(Json::array({channelJson(true)}));
        if (r.method == "GET" && path == ch + "/keys") return ok(keysJson());
        if (r.method == "PUT" && path == ch + "/visibility") return visibility(body);
        if (r.method == "POST" && path == ch + "/posts") return createPost(body);
        if (r.method == "GET" && path == ch + "/scheduled") {
            Json list = Json::array();
            for (const auto& p : posts) {
                if (p.scheduled) list.push_back(postJson(p));
            }
            return ok(list);
        }
        if (r.method == "GET" && path == ch + "/conversion") {
            Json list = Json::array();
            for (const auto& p : posts) {
                // Owner only: an open quiz's answer shows here (server fix 2026-11-08), so it survives the conversion.
                if (isPrivate == p.encrypted.is_null() && list.size() < 50) list.push_back(postJson(p, true));
            }
            return ok(Json{{"isPrivate", isPrivate}, {"keyEpoch", keyEpoch ? Json(*keyEpoch) : Json()}, {"posts", list}});
        }
        if (r.method == "POST" && (path == ch + "/media" || path == ch + "/suggestions/media")) return startUpload(body);
        if (r.method == "PUT" && path.starts_with("channel-uploads/")) {
            auto& m = media.at(path.substr(std::string("channel-uploads/").size()));
            if (static_cast<std::int64_t>(r.body.size()) != m.meta["size"].get<std::int64_t>()) return error(400, "validation_error");
            m.bytes = r.body;
            m.uploaded = true;
            return ok(Json{{"status", "uploaded"}});
        }
        if (r.method == "GET" && path.starts_with("channel-media/")) {
            const auto& m = media.at(path.substr(std::string("channel-media/").size()));
            const std::string range = FakeTransport::headerOf(r, "range");
            if (range.empty()) return net::HttpResponse{200, {}, m.bytes};
            std::smatch match;
            REQUIRE(std::regex_match(range, match, std::regex("bytes=([0-9]+)-([0-9]+)")));
            const auto first = std::stoull(match[1]);
            const auto last = std::min<std::uint64_t>(std::stoull(match[2]), m.bytes.size() - 1);
            return net::HttpResponse{206, {}, m.bytes.substr(first, last - first + 1)};
        }
        const std::regex postRoute("channels/" + kChannel + "/posts/([0-9a-f-]{36})(/.*)?");
        std::smatch m;
        if (std::regex_match(path, m, postRoute)) {
            auto* post = find(m[1]);
            if (!post) return notFound();
            const std::string rest = m[2];
            if (r.method == "PUT" && rest.empty()) return replacePost(*post, body);
            if (r.method == "POST" && rest == "/vote") {
                if (isPrivate && !readable) return notFound();
                post->votes["me"] = body["options"].get<std::vector<int>>();
                return ok(Json{{"poll", pollJson(*post)}, {"mine", post->votes["me"]}});
            }
            if (r.method == "POST" && rest == "/close") {
                post->encMeta["closed"] = true;
                return ok(postJson(*post));
            }
            if (r.method == "PUT" && rest.starts_with("/checklist/")) {
                const int index = std::stoi(rest.substr(std::string("/checklist/").size()));
                post->encMeta["done"][static_cast<std::size_t>(index)] = body["done"].get<bool>();
                return ok(Json{{"itemCount", post->encMeta["itemCount"]}, {"collaborative", post->encMeta["collaborative"]}, {"done", post->encMeta["done"]}});
            }
        }
        if (r.method == "POST" && path == ch + "/suggestions") {
            if (isPrivate && !body.contains("ciphertext")) return error(409, "encryption_required");
            Json s{{"id", uuid(++counter)}, {"body", ""}, {"status", "pending"}, {"createdAt", time()}, {"media", Json::array()},
                   {"encrypted", Json{{"keyEpoch", body["keyEpoch"]}, {"nonce", body["nonce"]}, {"ciphertext", body["ciphertext"]}}}};
            for (const auto& id : body.value("media", Json::array())) s["media"].push_back(mediaView(id));
            suggestions.push_back(s);
            return net::HttpResponse{201, {}, s.dump()};
        }
        if (r.method == "GET" && (path == ch + "/suggestions" || path == ch + "/suggestions/mine")) return ok(Json(suggestions));
        const std::regex approveRoute("channels/" + kChannel + "/suggestions/([0-9a-f-]{36})/approve");
        if (r.method == "POST" && std::regex_match(path, m, approveRoute)) {
            auto it = std::find_if(suggestions.begin(), suggestions.end(), [&](const Json& s) { return s["id"] == m[1].str(); });
            if (it == suggestions.end()) return notFound();
            Json enc = body.contains("ciphertext") ? Json{{"keyEpoch", body["keyEpoch"]}, {"nonce", body["nonce"]}, {"ciphertext", body["ciphertext"]}}
                                                   : (*it)["encrypted"];
            if (enc["keyEpoch"] != *keyEpoch) return error(409, "stale_key_epoch");
            PostRow row;
            row.id = uuid(++counter);
            row.createdAt = time();
            row.encrypted = enc;
            row.encMeta = Json{{"kind", "text"}};
            for (const auto& mv : (*it)["media"]) row.media.push_back(mv["id"]);
            if (!row.media.empty()) row.encMeta = Json{{"kind", "media"}};
            (*it)["status"] = "approved";
            posts.push_back(row);
            return net::HttpResponse{201, {}, postJson(row).dump()};
        }
        FAIL("Unexpected request " + r.method + " " + path);
        return {};
    }

    PostRow* find(const std::string& id) {
        for (auto& p : posts) {
            if (p.id == id) return &p;
        }
        return nullptr;
    }

    /// A plain post as it was before going private.
    PostRow& addPlain(const std::string& body, Json content = nullptr, std::vector<std::string> mediaIds = {}) {
        PostRow row;
        row.id = uuid(++counter);
        row.createdAt = time();
        row.body = body;
        row.content = std::move(content);
        row.media = std::move(mediaIds);
        posts.push_back(row);
        return posts.back();
    }

    std::string addPlainMedia(const std::string& bytes, const std::string& kind, const std::string& mime, const std::string& filename) {
        const std::string id = uuid(++counter);
        media[id] = MediaItem{bytes, false, Json{{"kind", kind}, {"mime", mime}, {"size", bytes.size()}, {"filename", filename}}, true, true};
        return id;
    }

private:
    static net::HttpResponse ok(const Json& j) { return net::HttpResponse{200, {}, j.dump()}; }
    static net::HttpResponse error(int status, const std::string& code) {
        return net::HttpResponse{status, {}, Json{{"error", code}, {"message", code}}.dump()};
    }
    static net::HttpResponse notFound() { return error(404, "not_found"); }

    Json channelJson(bool own) const {
        Json j{{"id", kChannel},        {"name", "Club"}, {"description", ""}, {"createdAt", "2026-11-01T00:00:00.000Z"},
               {"lastPostAt", nullptr}, {"isPrivate", isPrivate}, {"acceptSuggestions", true}, {"avatarUrl", nullptr}};
        j["keyEpoch"] = (isPrivate || own) && keyEpoch ? Json(*keyEpoch) : Json();
        if (own) {
            j["secretSealed"] = secretSealed ? Json(*secretSealed) : Json();
            j["formerSecretSealed"] = formerSecretSealed ? Json(*formerSecretSealed) : Json();
        }
        return j;
    }

    Json keysJson() const {
        Json list = Json::array();
        for (const auto& k : keys) list.push_back(Json{{"epoch", k.epoch}, {"wrapped", k.wrapped}});
        return Json{{"keyEpoch", keyEpoch ? Json(*keyEpoch) : Json()}, {"keys", list}};
    }

    net::HttpResponse visibility(const Json& body) {
        if (!body["isPrivate"].get<bool>()) {
            isPrivate = false;
            formerSecretSealed = secretSealed;
            secretSealed.reset();
            accessToken.reset();
            return ok(channelJson(true));
        }
        if (!body.contains("keys")) return error(400, "keys_required");
        const int n = keyEpoch.value_or(0);
        std::vector<int> epochs;
        for (const auto& k : body["keys"]) {
            epochs.push_back(k["epoch"]);
            REQUIRE(b64(k["wrapped"].get<std::string>()).size() == 72);
        }
        std::sort(epochs.begin(), epochs.end());
        std::vector<int> expected;
        for (int e = 1; e <= n + 1; ++e) expected.push_back(e);
        if (epochs != expected) return error(409, "wrong_key_epochs");
        keys.clear();
        for (const auto& k : body["keys"]) keys.push_back(channels::WrappedKey{k["epoch"], k["wrapped"]});
        std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.epoch < b.epoch; });
        isPrivate = true;
        keyEpoch = n + 1;
        accessToken = body["access"].get<std::string>();
        secretSealed = body["secretSealed"].get<std::string>();
        formerSecretSealed.reset();
        return ok(channelJson(true));
    }

    /// Checks a body in the channel's format and fills a row's payload; an error response when it's refused.
    std::optional<net::HttpResponse> shape(PostRow& row, const Json& body) {
        const bool encrypted = body.contains("ciphertext") || body.contains("keyEpoch") || body.contains("nonce");
        if (encrypted) {
            if (!isPrivate) return error(409, "plain_text_required");
            if (!keyEpoch) return error(409, "channel_keys_missing");
            if (body["keyEpoch"] != *keyEpoch) return error(409, "stale_key_epoch");
            REQUIRE(b64(body["nonce"].get<std::string>()).size() == 24);
            row.encrypted = Json{{"keyEpoch", body["keyEpoch"]}, {"nonce", body["nonce"]}, {"ciphertext", body["ciphertext"]}};
            row.body.clear();
            row.content = nullptr;
            if (body.contains("poll") && body["poll"].is_object()) {
                const Json before = row.encMeta;
                row.encMeta = Json{{"kind", "poll"},
                                   {"optionCount", body["poll"]["optionCount"]},
                                   {"multiple", body["poll"].value("multiple", false)},
                                   {"quiz", body["poll"].value("quiz", false)},
                                   {"closed", before.is_object() ? before.value("closed", false) : false}};
            } else if (body.contains("checklist") && body["checklist"].is_object()) {
                const int count = body["checklist"]["itemCount"];
                Json done = row.encMeta.is_object() && row.encMeta.contains("done") ? row.encMeta["done"] : Json(std::vector<bool>(count, false));
                row.encMeta = Json{{"kind", "checklist"}, {"itemCount", count}, {"collaborative", body["checklist"].value("collaborative", false)}, {"done", done}};
            } else {
                row.encMeta = Json{{"kind", body.value("media", Json::array()).empty() ? "text" : "media"}};
            }
        } else {
            if (isPrivate) return error(409, "encryption_required");
            row.encrypted = nullptr;
            row.encMeta = nullptr;
            row.body = body.value("body", "");
            row.content = nullptr;
            if (body.contains("poll") && body["poll"].is_object()) {
                Json p = body["poll"];
                p["type"] = "poll";
                row.content = p;
            } else if (body.contains("checklist") && body["checklist"].is_object()) {
                Json c{{"type", "checklist"}, {"title", body["checklist"]["title"]}, {"collaborative", body["checklist"].value("collaborative", false)}};
                c["items"] = Json::array();
                for (const auto& t : body["checklist"]["items"]) c["items"].push_back(Json{{"text", t}, {"done", false}});
                row.content = c;
            }
        }
        // Media: the post's new list, each id a finished upload in the right format.
        std::vector<std::string> list;
        for (const auto& id : body.value("media", Json::array())) {
            const std::string mid = id;
            const bool kept = std::find(row.media.begin(), row.media.end(), mid) != row.media.end();
            if (!kept) {
                const auto it = media.find(mid);
                if (it == media.end() || it->second.used || !it->second.uploaded || it->second.encrypted != encrypted) return error(400, "validation_error");
                it->second.used = true;
            }
            list.push_back(mid);
        }
        row.media = list;
        return std::nullopt;
    }

    net::HttpResponse createPost(const Json& body) {
        PostRow row;
        row.id = uuid(++counter);
        row.createdAt = time();
        if (auto refused = shape(row, body)) return *refused;
        row.scheduled = body.contains("publishAt") && !body["publishAt"].is_null();
        if (row.scheduled) row.createdAt = body["publishAt"];
        posts.push_back(row);
        return net::HttpResponse{201, {}, postJson(row).dump()};
    }

    net::HttpResponse replacePost(PostRow& post, const Json& body) {
        if (body.contains("publishAt")) return error(400, "validation_error");
        if (rateLimitEdits > 0) {
            --rateLimitEdits;
            ++editsRateLimited;
            return error(429, "rate_limited");
        }
        if (!isPrivate && post.encrypted.is_null()) return error(409, "post_not_encrypted");
        PostRow next = post;
        if (auto refused = shape(next, body)) return *refused;
        // Votes and ticks carry over (the fake keeps them in the row).
        post = next;
        return ok(postJson(post));
    }

    net::HttpResponse startUpload(const Json& body) {
        const bool encrypted = body.value("encrypted", false);
        if (isPrivate && !encrypted) return error(409, "encryption_required");
        if (!isPrivate && encrypted) return error(409, "plain_text_required");
        const std::string id = uuid(++counter);
        media[id] = MediaItem{{}, encrypted, encrypted ? Json{{"kind", "file"}, {"mime", "application/octet-stream"}, {"size", body["size"]}} : body};
        return net::HttpResponse{201, {}, Json{{"id", id}, {"kind", media[id].meta["kind"]}, {"size", body["size"]}, {"uploadUrl", "/api/channel-uploads/" + id}}.dump()};
    }

    Json mediaView(const std::string& id) const {
        const auto& m = media.at(id);
        return Json{{"id", id},
                    {"kind", m.meta.value("kind", "file")},
                    {"mime", m.meta.value("mime", "application/octet-stream")},
                    {"size", m.bytes.size()},
                    {"filename", m.encrypted ? Json() : m.meta.value("filename", Json())},
                    {"durationMs", nullptr},
                    {"width", nullptr},
                    {"height", nullptr},
                    {"url", "/api/channel-media/" + id + "?exp=1&sig=" + kSig}};
    }

    std::vector<int> counts(const PostRow& p, std::size_t n) const {
        std::vector<int> out(n, 0);
        for (const auto& [voter, options] : p.votes) {
            for (const int o : options) ++out[static_cast<std::size_t>(o)];
        }
        return out;
    }

    Json pollJson(const PostRow& p, bool revealQuiz = false) const {
        if (!p.encrypted.is_null()) {
            const int n = p.encMeta["optionCount"];
            return Json{{"optionCount", n},
                        {"multiple", p.encMeta["multiple"]},
                        {"quiz", p.encMeta["quiz"]},
                        {"closed", p.encMeta["closed"]},
                        {"counts", counts(p, static_cast<std::size_t>(n))},
                        {"voters", p.votes.size()}};
        }
        const auto& c = p.content;
        const bool quiz = c.contains("quiz") && c["quiz"].is_object();
        return Json{{"question", c["question"]},
                    {"options", c["options"]},
                    {"multiple", c.value("multiple", false)},
                    {"quiz", quiz},
                    {"closed", c.value("closed", false)},
                    {"counts", counts(p, c["options"].size())},
                    {"voters", p.votes.size()},
                    {"correct", quiz && (revealQuiz || c.value("closed", false)) ? c["quiz"]["correct"] : Json()},
                    {"explanation", quiz && (revealQuiz || c.value("closed", false)) ? c["quiz"].value("explanation", Json()) : Json()}};
    }

    Json postJson(const PostRow& p, bool revealQuiz = false) const {
        Json media = Json::array();
        for (const auto& id : p.media) media.push_back(mediaView(id));
        Json j{{"id", p.id},         {"body", p.body},       {"createdAt", p.createdAt}, {"views", 0},
               {"suggested", false}, {"poll", nullptr},      {"checklist", nullptr},     {"media", media},
               {"encrypted", nullptr}};
        if (!p.encrypted.is_null()) {
            Json e = p.encrypted;
            e["kind"] = p.encMeta["kind"];
            e["poll"] = p.encMeta["kind"] == "poll" ? pollJson(p) : Json();
            e["checklist"] = p.encMeta["kind"] == "checklist"
                                 ? Json{{"itemCount", p.encMeta["itemCount"]}, {"collaborative", p.encMeta["collaborative"]}, {"done", p.encMeta["done"]}}
                                 : Json();
            j["encrypted"] = e;
        } else if (p.content.is_object() && p.content["type"] == "poll") {
            j["poll"] = pollJson(p, revealQuiz);
        } else if (p.content.is_object() && p.content["type"] == "checklist") {
            j["checklist"] = Json{{"title", p.content["title"]}, {"items", p.content["items"]}, {"collaborative", p.content["collaborative"]}};
        }
        return j;
    }
};

struct Rig {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<api::ApiClient> api =
        std::make_shared<api::ApiClient>("https://example.com/api/", transport, std::make_shared<net::SessionCookieJar>());
    FakeChannelServer server;
    crypto::Bytes publicKey = crypto::Bytes(crypto_box_PUBLICKEYBYTES);
    crypto::Bytes privateKey = crypto::Bytes(crypto_box_SECRETKEYBYTES);

    Rig() {
        crypto::ensureSodium();
        crypto_box_keypair(publicKey.data(), privateKey.data());
        transport->handler = [this](const net::HttpRequest& r) { return server.handle(r); };
    }

    channels::Channel own() { return channels::mine(*api).front(); }

    pc::Switched goPrivate() { return pc::makePrivate(*api, own(), publicKey, privateKey); }

    /// What a reader holding `secret` sees: the posts opened with the keys the link unwraps.
    std::vector<channels::Post> read(const std::string& secret) {
        const auto keys = pc::readerKeys(*api, kChannel, secret);
        return pc::open(channels::posts(*api, kChannel, std::nullopt, links::accessToken(secret)), keys, kChannel);
    }

    pc::KeyringPtr ownerKeys() { return std::make_shared<pc::Keyring>(pc::ownerKeys(*api, own(), publicKey, privateKey)); }

    channels::Media uploadPhoto(const std::string& bytes, const pc::KeyringPtr&) {
        std::istringstream in(bytes);
        channels::Media meta;
        meta.kind = "photo";
        meta.mime = "image/png";
        meta.size = static_cast<std::int64_t>(bytes.size());
        meta.filename = "dot.png";
        meta.width = 1;
        meta.height = 1;
        return pc::uploadEncrypted(*api, [&](std::int64_t size) { return channels::startEncryptedMedia(*api, kChannel, size); }, in, meta);
    }

    channels::Post send(pc::Content content, pc::KeyringPtr& keys, std::optional<std::string> postId = std::nullopt,
                        std::optional<std::string> publishAt = std::nullopt) {
        return pc::send(*api, kChannel, pc::Draft{std::move(content), std::move(publishAt)}, keys, [this] { return ownerKeys(); }, postId);
    }
};

pc::Content text(const std::string& body) {
    pc::Content c;
    c.body = body;
    return c;
}

const channels::Post* byBody(const std::vector<channels::Post>& list, const std::string& body) {
    for (const auto& p : list) {
        if (p.body == body) return &p;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("private channel: posts, polls, checklists, media and scheduled posts go out encrypted", "[channels][e2ee][private]") {
    Rig rig;
    const auto made = rig.goPrivate();
    CHECK(made.channel.isPrivate);
    CHECK(made.channel.keyEpoch == 1);
    pc::KeyringPtr keys = made.keys;

    rig.send(text("Hello **club**"), keys);

    pc::Content poll;
    poll.body = "Vote";
    poll.poll = channels::NewPost::NewPoll{"Best day?", {"Sat", "Sun"}, false, channels::NewPost::NewPoll::Quiz{1, "Rest day"}};
    const auto quiz = rig.send(poll, keys);

    pc::Content list;
    list.checklist = channels::NewPost::NewChecklist{"Bring", {"Food", "Water", "Map"}, true};
    const auto checklist = rig.send(list, keys);

    const std::string photo = "\x89PNG fake photo bytes \x01\x02";
    pc::Content withMedia;
    withMedia.body = "A photo";
    withMedia.media.push_back(rig.uploadPhoto(photo, keys));
    rig.send(withMedia, keys);

    rig.send(text("Later"), keys, std::nullopt, std::string("2027-01-01T00:00:00.000Z"));

    // The server holds no plain text: bodies are empty, the ciphertext doesn't contain the words, media blobs are opaque.
    for (const auto& row : rig.server.posts) {
        CHECK(row.body.empty());
        CHECK(row.content.is_null());
        CHECK(row.encrypted["keyEpoch"] == 1);
    }
    for (const auto& r : rig.transport->requests) {
        if (r.method == "POST" || r.method == "PUT") {
            CHECK(r.body.find("Hello") == std::string::npos);
            CHECK(r.body.find("Best day") == std::string::npos);
            CHECK(r.body.find("Water") == std::string::npos);
            CHECK(r.body.find("fake photo") == std::string::npos);
        }
    }
    CHECK(rig.server.posts[1].encMeta["optionCount"] == 2);
    CHECK(rig.server.posts[1].encMeta["quiz"] == true);
    CHECK(rig.server.posts[2].encMeta["itemCount"] == 3);

    // A reader with the link opens everything published (the scheduled one isn't).
    const auto read = rig.read(made.secret);
    REQUIRE(read.size() == 4);
    CHECK(byBody(read, "Hello **club**"));
    const auto* p = byBody(read, "Vote");
    REQUIRE(p);
    REQUIRE(p->poll);
    CHECK(p->poll->question == "Best day?");
    CHECK(p->poll->options == std::vector<std::string>{"Sat", "Sun"});
    CHECK(p->poll->quiz);
    // The quiz's answer isn't shown before answering.
    CHECK_FALSE(p->poll->correct);
    CHECK(p->poll->sealedCorrect == 1);
    const auto* c = byBody(read, "");
    REQUIRE(c);
    REQUIRE(c->checklist);
    CHECK(c->checklist->title == "Bring");
    CHECK(c->checklist->items.size() == 3);
    const auto* m = byBody(read, "A photo");
    REQUIRE(m);
    REQUIRE(m->media.size() == 1);
    CHECK(m->media[0].kind == "photo");
    CHECK(m->media[0].filename == "dot.png");
    CHECK(pc::fetchDecrypted(*rig.api, m->media[0]) == photo);
    std::ostringstream saved;
    pc::downloadDecrypted(*rig.api, m->media[0], saved);
    CHECK(saved.str() == photo);
    CHECK(rig.server.media.at(m->media[0].id).bytes != photo);

    // The owner sees the scheduled post opened too.
    const auto scheduled = pc::open(channels::scheduled(*rig.api, kChannel), *keys, kChannel);
    REQUIRE(scheduled.size() == 1);
    CHECK(scheduled[0].body == "Later");

    // Voting answers with counts only; merged with the opened texts, and the quiz's answer shows now.
    const auto voted = channels::vote(*rig.api, kChannel, quiz.id, {1}, links::accessToken(made.secret));
    const auto merged = pc::mergePoll(*p->poll, voted.poll, true);
    CHECK(merged.question == "Best day?");
    CHECK(merged.counts == std::vector<int>{0, 1});
    CHECK(merged.correct == 1);
    CHECK(merged.explanation == "Rest day");

    // Ticking answers with ticks only.
    const auto ticked = channels::setChecklistItem(*rig.api, kChannel, checklist.id, 1, true, links::accessToken(made.secret));
    const auto mergedList = pc::mergeChecklist(*c->checklist, ticked);
    CHECK(mergedList.items[1].text == "Water");
    CHECK(mergedList.items[1].done);
    CHECK_FALSE(mergedList.items[0].done);
}

TEST_CASE("private channel: an edit is re-encrypted and keeps the post", "[channels][e2ee][private]") {
    Rig rig;
    const auto made = rig.goPrivate();
    pc::KeyringPtr keys = made.keys;
    const auto first = rig.send(text("Typo"), keys);
    const auto edited = rig.send(text("Fixed"), keys, first.id);
    CHECK(edited.id == first.id);
    CHECK(rig.server.posts.size() == 1);
    const auto read = rig.read(made.secret);
    REQUIRE(read.size() == 1);
    CHECK(read[0].body == "Fixed");
    // A public channel's own posts can't be edited, and edits can't be scheduled.
    CHECK_THROWS(channels::replacePost(*rig.api, kChannel, first.id, channels::NewEncryptedPost{{}, {}, {}, {}, std::string("2027-01-01T00:00:00Z")}));
}

TEST_CASE("private channel: suggestions are encrypted, and approved as they are or re-encrypted", "[channels][e2ee][private]") {
    Rig rig;
    const auto made = rig.goPrivate();
    const auto access = links::accessToken(made.secret);
    // A follower: keys from the link, a photo encrypted before upload.
    const auto readerKeys = std::make_shared<pc::Keyring>(pc::readerKeys(*rig.api, kChannel, made.secret));
    pc::Content suggestion;
    suggestion.body = "Try this";
    {
        const std::string photo = "photo!";
        std::istringstream in(photo);
        channels::Media meta;
        meta.kind = "photo";
        meta.mime = "image/jpeg";
        meta.size = static_cast<std::int64_t>(photo.size());
        suggestion.media.push_back(pc::uploadEncrypted(
            *rig.api, [&](std::int64_t size) { return channels::startEncryptedSuggestionMedia(*rig.api, kChannel, size, access); }, in, meta));
    }
    const auto sent = channels::suggest(*rig.api, kChannel, pc::sealSuggestion(suggestion, *readerKeys, kChannel),
                                        std::vector<std::string>{suggestion.media[0].id}, access);
    CHECK(sent.body.empty());
    CHECK(sent.encrypted);

    // The owner opens it.
    const auto ownerKeys = rig.ownerKeys();
    const auto waiting = pc::open(channels::suggestions(*rig.api, kChannel).front(), *ownerKeys, kChannel);
    CHECK(waiting.body == "Try this");
    REQUIRE(waiting.media.size() == 1);
    CHECK(pc::fetchDecrypted(*rig.api, waiting.media[0]) == "photo!");

    // Published as it is.
    const auto published = channels::approveSuggestionSealed(*rig.api, kChannel, sent.id, std::nullopt, std::nullopt);
    CHECK(pc::open(published, *ownerKeys, kChannel).body == "Try this");

    // Or re-encrypted with the owner's edit.
    const auto second = channels::suggest(*rig.api, kChannel, pc::sealSuggestion(text("Draft"), *readerKeys, kChannel), {}, access);
    const auto edited = channels::approveSuggestionSealed(*rig.api, kChannel, second.id,
                                                          pc::sealSuggestion(text("Polished"), *ownerKeys, kChannel), std::nullopt);
    CHECK(pc::open(edited, *ownerKeys, kChannel).body == "Polished");
    // A plain suggestion is refused in a private channel.
    CHECK_THROWS_AS(channels::suggest(*rig.api, kChannel, std::string("plain"), {}, access), api::ApiError);
}

TEST_CASE("private channel: a link reset re-wraps every epoch; the new link reads all, the old one nothing new", "[channels][e2ee][private]") {
    Rig rig;
    const auto first = rig.goPrivate();
    pc::KeyringPtr keys = first.keys;
    rig.send(text("Before the reset"), keys);

    const auto reset = rig.goPrivate();
    CHECK(reset.channel.keyEpoch == 2);
    CHECK(reset.secret != first.secret);
    CHECK(rig.server.keys.size() == 2);
    pc::KeyringPtr newKeys = reset.keys;
    rig.send(text("After the reset"), newKeys);
    CHECK(rig.server.posts.back().encrypted["keyEpoch"] == 2);

    // The new link opens the whole history.
    const auto read = rig.read(reset.secret);
    REQUIRE(read.size() == 2);
    CHECK(byBody(read, "Before the reset"));
    CHECK(byBody(read, "After the reset"));

    // The old link: the server refuses its token, and even its old keys open nothing posted since.
    CHECK_THROWS_AS(pc::readerKeys(*rig.api, kChannel, first.secret), api::ApiError);
    const auto newPosts = channels::posts(*rig.api, kChannel, std::nullopt, links::accessToken(reset.secret));
    const auto withOldKeys = pc::open(newPosts, *first.keys, kChannel);
    const auto newest = std::find_if(withOldKeys.begin(), withOldKeys.end(), [](const auto& p) { return p.encrypted->sealed.keyEpoch == 2; });
    REQUIRE(newest != withOldKeys.end());
    CHECK(newest->locked);
    CHECK(newest->body.empty());

    // A post sealed under the old epoch after the reset is refused, and send() fetches the keys again and retries.
    pc::KeyringPtr stale = first.keys;
    const auto retried = rig.send(text("Raced the reset"), stale);
    CHECK(stale->keyEpoch == 2);
    CHECK(rig.server.posts.back().encrypted["keyEpoch"] == 2);
    CHECK(retried.encrypted->sealed.keyEpoch == 2);
}

TEST_CASE("public to private: old plain posts and media are converted, oldest first, through rate limits", "[channels][e2ee][private]") {
    Rig rig;
    const std::string bytes = "plain photo bytes";
    const auto photoId = rig.server.addPlainMedia(bytes, "photo", "image/jpeg", "beach.jpg");
    rig.server.addPlain("First");
    rig.server.addPlain("Look", nullptr, {photoId});
    rig.server.addPlain("", Json{{"type", "poll"}, {"question", "Q?"}, {"options", {"x", "y", "z"}}, {"multiple", true}, {"quiz", nullptr}, {"closed", false}});
    rig.server.addPlain("", Json{{"type", "checklist"}, {"title", "Do"}, {"items", Json::array({Json{{"text", "a"}, {"done", true}}})}, {"collaborative", false}});

    const auto made = rig.goPrivate();
    // Until converted, readers with the link see them as they were.
    CHECK(rig.read(made.secret).size() == 4);

    rig.server.rateLimitEdits = 2;
    std::vector<int> waits;
    std::vector<int> progress;
    const auto result = pc::convert(*rig.api, kChannel, *made.keys,
                                    pc::ConversionHooks{{}, [&](int attempt) { waits.push_back(attempt); }, [&](int n) { progress.push_back(n); }});
    CHECK(result.finished);
    CHECK(result.converted == 4);
    CHECK(waits == std::vector<int>{1, 2});
    CHECK(progress == std::vector<int>{1, 2, 3, 4});
    for (const auto& row : rig.server.posts) CHECK_FALSE(row.encrypted.is_null());
    CHECK(channels::conversion(*rig.api, kChannel).posts.empty());

    const auto read = rig.read(made.secret);
    REQUIRE(read.size() == 4);
    const auto* look = byBody(read, "Look");
    REQUIRE(look);
    REQUIRE(look->media.size() == 1);
    CHECK(look->media[0].id != photoId);
    CHECK(look->media[0].filename == "beach.jpg");
    CHECK(pc::fetchDecrypted(*rig.api, look->media[0]) == bytes);
    const auto poll = std::find_if(read.begin(), read.end(), [](const auto& p) { return p.poll.has_value(); });
    REQUIRE(poll != read.end());
    CHECK(poll->poll->question == "Q?");
    CHECK(poll->poll->multiple);
    CHECK(poll->poll->options.size() == 3);
    // Oldest first: the first edit was the oldest post.
    const auto firstPut = std::find_if(rig.transport->requests.begin(), rig.transport->requests.end(),
                                       [](const auto& r) { return r.method == "PUT" && r.url.find("/posts/") != std::string::npos; });
    REQUIRE(firstPut != rig.transport->requests.end());
    CHECK(firstPut->url.find(rig.server.posts.front().id) != std::string::npos);

    // Cancelled before anything: nothing happens, and it can resume later.
    const auto again = pc::convert(*rig.api, kChannel, *made.keys, pc::ConversionHooks{[] { return true; }, {}, {}});
    CHECK(again.converted == 0);
}

TEST_CASE("private to public: encrypted posts are decrypted back to plain text with their media", "[channels][e2ee][private]") {
    Rig rig;
    const auto made = rig.goPrivate();
    pc::KeyringPtr keys = made.keys;
    rig.send(text("Secret news"), keys);
    pc::Content withMedia;
    withMedia.body = "Pic";
    withMedia.media.push_back(rig.uploadPhoto("PIXELS", keys));
    rig.send(withMedia, keys);
    pc::Content quiz;
    quiz.poll = channels::NewPost::NewPoll{"2+2?", {"3", "4"}, false, channels::NewPost::NewPoll::Quiz{1, ""}};
    rig.send(quiz, keys);

    const auto channel = channels::setPublic(*rig.api, kChannel);
    CHECK_FALSE(channel.isPrivate);
    CHECK(channel.formerSecretSealed);
    CHECK(channel.keyEpoch == 1);
    // Encrypted leftovers never show on reader routes while public.
    CHECK(channels::posts(*rig.api, kChannel).empty());

    // The owner's keys come from the former link's secret.
    const auto ownerKeys = rig.ownerKeys();
    CHECK(ownerKeys->canSeal());
    const auto result = pc::convert(*rig.api, kChannel, *ownerKeys);
    CHECK(result.finished);
    CHECK(result.converted == 3);

    const auto plain = channels::posts(*rig.api, kChannel);
    REQUIRE(plain.size() == 3);
    const auto* news = byBody(plain, "Secret news");
    REQUIRE(news);
    CHECK_FALSE(news->encrypted);
    const auto* pic = byBody(plain, "Pic");
    REQUIRE(pic);
    REQUIRE(pic->media.size() == 1);
    CHECK(channels::fetchMedia(*rig.api, pic->media[0].url) == "PIXELS");
    CHECK(rig.server.media.at(pic->media[0].id).meta["filename"] == "dot.png");
    const auto pollPost = std::find_if(plain.begin(), plain.end(), [](const auto& p) { return p.poll.has_value(); });
    REQUIRE(pollPost != plain.end());
    CHECK(pollPost->poll->question == "2+2?");
    CHECK(rig.server.find(pollPost->id)->content["quiz"]["correct"] == 1);

    // Going private again adds an epoch, wrapping the old one too.
    const auto again = rig.goPrivate();
    CHECK(again.channel.keyEpoch == 2);
    CHECK(again.keys->complete());
}

TEST_CASE("an old private channel gets epoch 1 under the link followers already hold", "[channels][e2ee][private]") {
    Rig rig;
    // Made private before C1: a link, no keys, plain posts.
    const std::string secret = links::newSecret();
    rig.server.isPrivate = true;
    rig.server.accessToken = links::accessToken(secret);
    rig.server.secretSealed = links::sealSecret(secret, rig.publicKey);
    rig.server.addPlain("Old plain post");

    const auto own = rig.own();
    CHECK(pc::needsKeys(own));
    // Posting is refused until then, either way.
    {
        pc::KeyringPtr none = std::make_shared<pc::Keyring>();
        CHECK_THROWS_AS(rig.send(text("x"), none), api::UserFacingError);
        CHECK_THROWS_AS(channels::post(*rig.api, kChannel, std::string("plain")), api::ApiError);
    }

    const auto upgraded = pc::setUpKeys(*rig.api, own, rig.publicKey, rig.privateKey);
    CHECK(upgraded.secret == secret);
    CHECK(upgraded.channel.keyEpoch == 1);
    CHECK(rig.server.accessToken == links::accessToken(secret));
    CHECK(rig.server.secretSealed == own.secretSealed);
    CHECK_FALSE(pc::needsKeys(rig.own()));

    const auto result = pc::convert(*rig.api, kChannel, *upgraded.keys);
    CHECK(result.finished);
    CHECK(result.converted == 1);

    // The link followers already had reads the converted post.
    const auto read = rig.read(secret);
    REQUIRE(read.size() == 1);
    CHECK(read[0].body == "Old plain post");
    CHECK(read[0].encrypted);

    // Running it again (another device, or a retry) changes nothing.
    const auto twice = pc::setUpKeys(*rig.api, rig.own(), rig.publicKey, rig.privateKey);
    CHECK(twice.channel.keyEpoch == 1);
    CHECK(twice.keys->canSeal());
}

TEST_CASE("a reader without a valid key sees posts as locked", "[channels][e2ee][private]") {
    Rig rig;
    const auto made = rig.goPrivate();
    pc::KeyringPtr keys = made.keys;
    rig.send(text("Members only"), keys);
    const auto posts = channels::posts(*rig.api, kChannel, std::nullopt, links::accessToken(made.secret));
    const auto opened = pc::open(posts, pc::Keyring{}, kChannel);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].locked);
    CHECK(opened[0].body.empty());
    // Keys under another channel's id don't open either.
    const auto foreign = pc::open(posts, *keys, "cccccccc-2222-4222-8222-222222222222");
    CHECK(foreign[0].locked);
}

TEST_CASE("an encrypted upload is the file crypto's format, streamed", "[channels][e2ee][private]") {
    Rig rig;
    const auto made = rig.goPrivate();
    // Exactly two chunks and a bit, and an empty file.
    for (const std::size_t size : {std::size_t{0}, crypto::files::kChunkBytes, crypto::files::kChunkBytes * 2 + 5}) {
        std::string bytes(size, '\0');
        for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>(i * 7);
        std::istringstream in(bytes);
        channels::Media meta;
        meta.kind = "file";
        meta.mime = "application/pdf";
        meta.size = static_cast<std::int64_t>(size);
        const auto entry = pc::uploadEncrypted(*rig.api, [&](std::int64_t n) { return channels::startEncryptedMedia(*rig.api, kChannel, n); }, in, meta);
        const auto& blob = rig.server.media.at(entry.id).bytes;
        CHECK(blob.size() == crypto::files::encryptedSizeFor(size));
        std::istringstream cipher(blob);
        std::ostringstream plain;
        crypto::files::decrypt(cipher, plain, b64(entry.secret->key), b64(entry.secret->header), blob.size(), entry.secret->sha256);
        CHECK(plain.str() == bytes);
    }
}

TEST_CASE("channel E2EE: a post in the Android app's layout opens, and this app writes the same layout", "[channels][e2ee]") {
    // Written as the Android app writes it (docs/signal-integration/10-part-c1-android-notes.md): false and null left out,
    // unknown fields present, a media id the post doesn't list.
    const std::string blobSize = std::to_string(crypto::files::encryptedSizeFor(12345));
    const std::string android = R"({"v":1,"body":"From Android","future":1,"content":{"media":[)"
                                R"({"id":"AAAAAAAA-0000-4000-8000-000000000001","kind":"photo","mime":"image/png","filename":"a.png",)"
                                R"("size":12345,"durationMs":1500,"width":640,"height":480,"fileKey":"a2V5","header":"aGVhZGVy",)"
                                R"("encryptedSize":)" + blobSize + R"(,"ciphertextSha256":"ABCD","extra":true},)"
                                R"({"id":"aaaaaaaa-0000-4000-8000-000000000002","kind":"photo","mime":"image/png","size":1,)"
                                R"("fileKey":"a2V5","header":"aGVhZGVy","encryptedSize":18,"ciphertextSha256":"ef"}]}})";
    const auto contentKey = e2ee::newContentKey();
    const auto sealed = e2ee::encrypt(android, contentKey, kChannel, 3);
    pc::Keyring keys;
    keys.keyEpoch = 3;
    keys.put(3, crypto::SecretBytes::copyOf(contentKey));

    channels::Post post;
    post.id = "p";
    post.encrypted = channels::EncryptedPost{{3, sealed.nonce, sealed.ciphertext}, "media", std::nullopt, std::nullopt};
    channels::Media listed;
    listed.id = "aaaaaaaa-0000-4000-8000-000000000001";
    listed.kind = "file";
    listed.url = "/api/channel-media/aaaaaaaa-0000-4000-8000-000000000001?exp=1&sig=" + std::string(43, 'S');
    post.media = {listed};
    const auto opened = pc::open(post, keys, kChannel);
    CHECK_FALSE(opened.locked);
    CHECK(opened.body == "From Android");
    REQUIRE(opened.media.size() == 1);  // the second entry isn't in the post's list
    CHECK(opened.media[0].kind == "photo");
    CHECK(opened.media[0].filename == "a.png");
    CHECK(opened.media[0].durationMs == 1500);
    CHECK(opened.media[0].width == 640);
    CHECK(opened.media[0].secret->key == "a2V5");
    CHECK(opened.media[0].secret->sha256 == "abcd");
    CHECK(opened.media[0].secret->plainSize == 12345);
    CHECK(opened.media[0].url == listed.url);

    // An entry whose encryptedSize doesn't fit its size is refused.
    CHECK(pc::parsePlaintext(R"({"v":1,"body":"","content":{"media":[{"id":"x","kind":"photo","mime":"m","size":5,"fileKey":"k",)"
                             R"("header":"h","encryptedSize":99,"ciphertextSha256":"00"}]}})")->media.empty());

    // A poll and checklist with the Android flags; and what this app writes leaves false and null out.
    const auto poll = pc::parsePlaintext(R"({"v":1,"body":"","content":{"poll":{"question":"Q","options":["a","b"],"multiple":true}}})");
    REQUIRE(poll);
    CHECK(poll->poll->multiple);
    CHECK_FALSE(poll->poll->quiz);
    pc::Content quiz;
    quiz.poll = channels::NewPost::NewPoll{"Q", {"a", "b"}, false, channels::NewPost::NewPoll::Quiz{1, "why"}};
    CHECK(Json::parse(pc::toPlaintext(quiz)) ==
          Json::parse(R"({"v":1,"body":"","content":{"poll":{"question":"Q","options":["a","b"],"quiz":{"correct":1,"explanation":"why"}}}})"));
    pc::Content list;
    list.checklist = channels::NewPost::NewChecklist{"T", {"x"}, false};
    CHECK(Json::parse(pc::toPlaintext(list)) == Json::parse(R"({"v":1,"body":"","content":{"checklist":{"title":"T","items":["x"]}}})"));
    pc::Content media;
    channels::Media m;
    m.id = "aaaaaaaa-0000-4000-8000-000000000001";
    m.kind = "photo";
    m.mime = "image/png";
    m.filename = "a.png";
    m.width = 640;
    m.height = 480;
    m.secret = channels::MediaKey{"a2V5", "aGVhZGVy", "abcd", 12345};
    media.media.push_back(m);
    CHECK(Json::parse(pc::toPlaintext(media))["content"]["media"][0] ==
          Json::parse(R"({"id":"aaaaaaaa-0000-4000-8000-000000000001","kind":"photo","mime":"image/png","filename":"a.png","size":12345,)"
                      R"("width":640,"height":480,"fileKey":"a2V5","header":"aGVhZGVy","encryptedSize":)" + blobSize +
                      R"(,"ciphertextSha256":"abcd"})"));
}

TEST_CASE("conversion stops after five rate limits in a row, to resume next time", "[channels][e2ee][private]") {
    Rig rig;
    rig.server.addPlain("One");
    const auto made = rig.goPrivate();
    rig.server.rateLimitEdits = 6;
    int waits = 0;
    CHECK_THROWS_AS(pc::convert(*rig.api, kChannel, *made.keys, pc::ConversionHooks{{}, [&](int) { ++waits; }, {}}), api::ApiError);
    CHECK(waits == 5);
    // Next time it picks up where it stopped.
    const auto later = pc::convert(*rig.api, kChannel, *made.keys);
    CHECK(later.finished);
    CHECK(later.converted == 1);
}

TEST_CASE("an open quiz keeps its answer through both conversions", "[channels][e2ee][private]") {
    Rig rig;
    rig.server.addPlain("", Json{{"type", "poll"}, {"question", "Capital?"}, {"options", {"Lyon", "Paris", "Nice"}}, {"multiple", false},
                                 {"quiz", Json{{"correct", 1}, {"explanation", "Since 987"}}}, {"closed", false}});
    // Readers don't see the answer while it's open.
    CHECK_FALSE(channels::posts(*rig.api, kChannel).front().poll->correct);

    const auto made = rig.goPrivate();
    CHECK(pc::convert(*rig.api, kChannel, *made.keys).converted == 1);
    // The sealed copy carries the real answer; readers still see it only once they've answered.
    const auto read = rig.read(made.secret);
    REQUIRE(read.size() == 1);
    REQUIRE(read[0].poll);
    CHECK(read[0].poll->sealedCorrect == 1);
    CHECK(read[0].poll->sealedExplanation == "Since 987");
    CHECK_FALSE(read[0].poll->correct);
    const auto content = pc::contentOf(read[0].encrypted->sealed, *made.keys, kChannel);
    REQUIRE(content);
    CHECK(content->poll->quiz->correct == 1);
    CHECK(content->poll->quiz->explanation == "Since 987");

    // Back to public: the real answer, not a stand-in.
    channels::setPublic(*rig.api, kChannel);
    CHECK(pc::convert(*rig.api, kChannel, *rig.ownerKeys()).converted == 1);
    const auto* row = &rig.server.posts.front();
    CHECK(row->encrypted.is_null());
    CHECK(row->content["quiz"]["correct"] == 1);
    CHECK(row->content["quiz"]["explanation"] == "Since 987");
}
