#include "redvault/api/channel_api.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace redvault::api::channels {

namespace {

using net::percentEncode;

constexpr const char* kUuid = "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}";

template <typename F>
auto parse(const Json& j, F&& f) -> decltype(f(j)) {
    try {
        return f(j);
    } catch (const Json::exception& e) {
        throw ApiError(0, "malformed_response", std::string("Unexpected response: ") + e.what());
    }
}

std::optional<std::string> optionalText(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

template <typename T>
std::optional<T> optionalNumber(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<T>();
}

bool flag(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

Channel channelFrom(const Json& j) {
    Channel c;
    c.id = j.at("id").get<std::string>();
    c.name = j.at("name").get<std::string>();
    c.description = j.value("description", std::string());
    c.createdAt = j.value("createdAt", std::string());
    c.lastPostAt = optionalText(j, "lastPostAt");
    c.isPrivate = flag(j, "isPrivate");
    c.avatarUrl = optionalText(j, "avatarUrl");
    c.secretSealed = optionalText(j, "secretSealed");
    c.acceptSuggestions = flag(j, "acceptSuggestions");
    return c;
}

Media mediaFrom(const Json& j) {
    Media m;
    m.id = j.at("id").get<std::string>();
    m.kind = j.at("kind").get<std::string>();
    m.mime = j.value("mime", std::string());
    m.size = j.value("size", std::int64_t{0});
    m.filename = optionalText(j, "filename");
    m.durationMs = optionalNumber<std::int64_t>(j, "durationMs");
    m.width = optionalNumber<int>(j, "width");
    m.height = optionalNumber<int>(j, "height");
    m.url = j.at("url").get<std::string>();
    return m;
}

std::vector<Media> mediaList(const Json& j) {
    std::vector<Media> out;
    if (const auto it = j.find("media"); it != j.end() && it->is_array()) {
        for (const auto& m : *it) out.push_back(mediaFrom(m));
    }
    return out;
}

Poll pollFrom(const Json& j) {
    Poll p;
    p.question = j.at("question").get<std::string>();
    p.options = j.at("options").get<std::vector<std::string>>();
    p.multiple = flag(j, "multiple");
    p.quiz = flag(j, "quiz");
    p.closed = flag(j, "closed");
    if (const auto it = j.find("counts"); it != j.end() && it->is_array()) p.counts = it->get<std::vector<int>>();
    p.counts.resize(p.options.size(), 0);
    p.voters = j.value("voters", 0);
    p.correct = optionalNumber<int>(j, "correct");
    p.explanation = optionalText(j, "explanation");
    return p;
}

Checklist checklistFrom(const Json& j) {
    Checklist c;
    c.title = j.at("title").get<std::string>();
    for (const auto& item : j.at("items")) c.items.push_back(ChecklistItem{item.at("text").get<std::string>(), flag(item, "done")});
    c.collaborative = flag(j, "collaborative");
    return c;
}

Post postFrom(const Json& j) {
    Post p;
    p.id = j.at("id").get<std::string>();
    p.body = j.value("body", std::string());
    p.createdAt = j.value("createdAt", std::string());
    if (const auto it = j.find("views"); it != j.end() && it->is_number_integer()) p.views = std::max<std::int64_t>(0, it->get<std::int64_t>());
    p.suggested = flag(j, "suggested");
    if (const auto it = j.find("poll"); it != j.end() && it->is_object()) p.poll = pollFrom(*it);
    if (const auto it = j.find("checklist"); it != j.end() && it->is_object()) p.checklist = checklistFrom(*it);
    p.media = mediaList(j);
    return p;
}

Suggestion suggestionFrom(const Json& j) {
    return Suggestion{j.at("id").get<std::string>(), j.value("body", std::string()), j.value("status", std::string("pending")),
                      j.value("createdAt", std::string()), mediaList(j)};
}

StartedUpload uploadFrom(const Json& j) {
    return StartedUpload{j.at("id").get<std::string>(), j.at("uploadUrl").get<std::string>()};
}

std::vector<Channel> channelList(const Json& body) {
    return parse(body, [](const Json& list) {
        std::vector<Channel> out;
        for (const auto& c : list) out.push_back(channelFrom(c));
        return out;
    });
}

std::vector<Post> postList(const Json& body) {
    return parse(body, [](const Json& list) {
        std::vector<Post> out;
        for (const auto& p : list) out.push_back(postFrom(p));
        return out;
    });
}

std::vector<Suggestion> suggestionList(const Json& body) {
    return parse(body, [](const Json& list) {
        std::vector<Suggestion> out;
        for (const auto& s : list) out.push_back(suggestionFrom(s));
        return out;
    });
}

std::string trimmed(const std::string& text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

std::string publicPath(const std::string& id, const char* rest = "") {
    return "public/channels/" + percentEncode(id) + rest;
}

std::string ownPath(const std::string& id, const std::string& rest = {}) {
    return "channels/" + percentEncode(id) + rest;
}

std::string postPath(const std::string& id, const std::string& postId, const std::string& rest = {}) {
    return ownPath(id, "/posts/" + percentEncode(postId) + rest);
}

/// A private channel's access token, in the header the web app sends it in.
net::Headers accessHeaders(const std::optional<std::string>& access) {
    if (!access || access->empty()) return {};
    return {{"x-channel-access", *access}};
}

Json metaJson(const UploadMeta& meta) {
    const auto orNull = [](const auto& value) { return value ? Json(*value) : Json(nullptr); };
    return Json{{"kind", meta.kind},
                {"mime", meta.mime},
                {"size", meta.size},
                {"filename", orNull(meta.filename)},
                {"durationMs", orNull(meta.durationMs)},
                {"width", orNull(meta.width)},
                {"height", orNull(meta.height)}};
}

/// A media link, relative to the API base: only the exact shape the server hands out.
std::string mediaPath(const std::string& url) {
    if (!isMediaUrl(url)) throw ApiError(0, "malformed_response", "Unexpected channel media URL");
    return url.substr(std::string_view("/api/").size());
}

/// Reads through another streambuf, counting bytes (upload progress).
class CountingInBuf : public std::streambuf {
public:
    CountingInBuf(std::streambuf* source, const std::function<void(std::int64_t)>& onCount) : source_(source), onCount_(onCount) {
        setg(buffer_, buffer_, buffer_);
    }

protected:
    int_type underflow() override {
        const std::streamsize n = source_->sgetn(buffer_, sizeof buffer_);
        if (n <= 0) return traits_type::eof();
        count_ += n;
        if (onCount_) onCount_(count_);
        setg(buffer_, buffer_, buffer_ + n);
        return traits_type::to_int_type(buffer_[0]);
    }

private:
    std::streambuf* source_;
    const std::function<void(std::int64_t)>& onCount_;
    char buffer_[64 * 1024];
    std::int64_t count_ = 0;
};

}  // namespace

std::vector<Channel> search(ApiClient& api, const std::string& query, int offset) {
    std::string path = "public/channels?limit=" + std::to_string(kPageSize) + "&offset=" + std::to_string(offset);
    if (const std::string q = trimmed(query); !q.empty()) path += "&q=" + percentEncode(q);
    return channelList(api.publicCall("GET", path));
}

Channel get(ApiClient& api, const std::string& id, const std::optional<std::string>& access) {
    return parse(api.publicCall("GET", publicPath(id), std::nullopt, accessHeaders(access)), channelFrom);
}

std::vector<Post> posts(ApiClient& api, const std::string& id, const std::optional<std::string>& before,
                        const std::optional<std::string>& access, int limit) {
    std::string path = publicPath(id, "/posts?limit=") + std::to_string(limit);
    if (before) path += "&before=" + percentEncode(*before);
    return postList(api.publicCall("GET", path, std::nullopt, accessHeaders(access)));
}

std::string avatar(ApiClient& api, const std::string& avatarUrl, const std::optional<std::string>& access) {
    // Exactly the shape channelDto's avatarUrl has, so a URL from the server can't point this request anywhere else.
    static const std::regex kAvatarUrl(std::string("^/api/public/channels/") + kUuid + "/avatar(\\?v=[0-9]{1,20})?$");
    if (!std::regex_match(avatarUrl, kAvatarUrl)) throw ApiError(0, "malformed_response", "Unexpected channel photo URL");
    return api.publicFetch(std::string_view(avatarUrl).substr(std::string_view("/api/").size()), accessHeaders(access));
}

void recordViews(ApiClient& api, const std::vector<std::string>& postIds) {
    if (postIds.empty()) return;
    if (postIds.size() > kMaxViewsPerReport) throw std::invalid_argument("Too many posts in one view report");
    api.publicCall("POST", "public/channels/views", Json{{"postIds", postIds}});
}

bool isMediaUrl(const std::string& url) {
    static const std::regex kMediaUrl(std::string("^/api/channel-media/") + kUuid + "\\?exp=[0-9]{1,20}&sig=[A-Za-z0-9_-]{43}$");
    return std::regex_match(url, kMediaUrl);
}

std::string fetchMedia(ApiClient& api, const std::string& url) {
    return api.publicFetch(mediaPath(url));
}

std::string fetchMediaRange(ApiClient& api, const std::string& url, std::uint64_t first, std::uint64_t last) {
    if (last < first) throw std::invalid_argument("Empty range");
    return api.publicFetch(mediaPath(url), {{"range", "bytes=" + std::to_string(first) + "-" + std::to_string(last)}});
}

void downloadMedia(ApiClient& api, const std::string& url, std::ostream& sink) {
    api.publicDownload(mediaPath(url), sink);
}

std::vector<Channel> mine(ApiClient& api) {
    return channelList(api.call("GET", "channels/mine"));
}

Channel create(ApiClient& api, const std::string& name, const std::string& description) {
    return parse(api.call("POST", "channels", Json{{"name", name}, {"description", description}}), channelFrom);
}

Channel update(ApiClient& api, const std::string& id, const std::string& name, const std::string& description) {
    return parse(api.call("PATCH", ownPath(id), Json{{"name", name}, {"description", description}}), channelFrom);
}

Channel setPublic(ApiClient& api, const std::string& id) {
    return parse(api.call("PUT", ownPath(id, "/visibility"), Json{{"isPrivate", false}}), channelFrom);
}

Channel setPrivate(ApiClient& api, const std::string& id, const std::string& access, const std::string& secretSealed) {
    return parse(api.call("PUT", ownPath(id, "/visibility"), Json{{"isPrivate", true}, {"access", access}, {"secretSealed", secretSealed}}),
                 channelFrom);
}

Channel setAcceptSuggestions(ApiClient& api, const std::string& id, bool accept) {
    return parse(api.call("PUT", ownPath(id, "/settings"), Json{{"acceptSuggestions", accept}}), channelFrom);
}

Channel setAvatar(ApiClient& api, const std::string& id, const std::string& imageBase64) {
    return parse(api.call("PUT", ownPath(id, "/avatar"), Json{{"image", imageBase64}}), channelFrom);
}

void removeAvatar(ApiClient& api, const std::string& id) {
    api.call("DELETE", ownPath(id, "/avatar"));
}

void remove(ApiClient& api, const std::string& id) {
    api.call("DELETE", ownPath(id));
}

Post post(ApiClient& api, const std::string& id, const NewPost& post) {
    Json body{{"body", post.body}};
    if (post.poll) {
        Json quiz = nullptr;
        if (post.poll->quiz) quiz = Json{{"correct", post.poll->quiz->correct}, {"explanation", post.poll->quiz->explanation}};
        body["poll"] = Json{{"question", post.poll->question}, {"options", post.poll->options}, {"multiple", post.poll->multiple}, {"quiz", quiz}};
    } else if (post.checklist) {
        body["checklist"] = Json{{"title", post.checklist->title}, {"items", post.checklist->items}, {"collaborative", post.checklist->collaborative}};
    } else if (!post.media.empty()) {
        body["media"] = post.media;
    }
    if (post.publishAt) body["publishAt"] = *post.publishAt;
    return parse(api.call("POST", ownPath(id, "/posts"), body), postFrom);
}

Post post(ApiClient& api, const std::string& id, const std::string& body) {
    return post(api, id, NewPost{body, std::nullopt, std::nullopt, {}, std::nullopt});
}

StartedUpload startMedia(ApiClient& api, const std::string& id, const UploadMeta& meta) {
    return parse(api.call("POST", ownPath(id, "/media"), metaJson(meta)), uploadFrom);
}

StartedUpload startSuggestionMedia(ApiClient& api, const std::string& id, const UploadMeta& meta, const std::optional<std::string>& access) {
    return parse(api.call("POST", ownPath(id, "/suggestions/media"), metaJson(meta), accessHeaders(access)), uploadFrom);
}

void uploadMedia(ApiClient& api, const std::string& uploadUrl, const std::filesystem::path& path, std::int64_t size,
                 const std::function<void(std::int64_t)>& onProgress) {
    // Only the shape uploadDto hands out, so the bytes (and the cookie) can't be sent anywhere else.
    static const std::regex kUploadUrl(std::string("^/api/channel-uploads/") + kUuid + "$");
    if (!std::regex_match(uploadUrl, kUploadUrl)) throw ApiError(0, "malformed_response", "Unexpected upload URL");
    if (size <= 0) throw std::invalid_argument("Nothing to upload");
    std::filebuf source;
    if (!source.open(path, std::ios::in | std::ios::binary)) throw UserFacingError("Couldn't read that file.");
    CountingInBuf counting(&source, onProgress);
    std::istream body(&counting);
    api.callUpload("PUT", std::string_view(uploadUrl).substr(std::string_view("/api/").size()), body, static_cast<std::uint64_t>(size),
                   "application/octet-stream");
}

std::vector<Post> scheduled(ApiClient& api, const std::string& id) {
    return postList(api.call("GET", ownPath(id, "/scheduled")));
}

Post publishNow(ApiClient& api, const std::string& id, const std::string& postId) {
    return parse(api.call("POST", postPath(id, postId, "/publish")), postFrom);
}

void removePost(ApiClient& api, const std::string& id, const std::string& postId) {
    api.call("DELETE", postPath(id, postId));
}

VoteResult vote(ApiClient& api, const std::string& id, const std::string& postId, const std::vector<int>& options,
                const std::optional<std::string>& access) {
    return parse(api.call("POST", postPath(id, postId, "/vote"), Json{{"options", options}}, accessHeaders(access)),
                 [](const Json& j) { return VoteResult{pollFrom(j.at("poll")), j.at("mine").get<std::vector<int>>()}; });
}

Post closePoll(ApiClient& api, const std::string& id, const std::string& postId) {
    return parse(api.call("POST", postPath(id, postId, "/close")), postFrom);
}

Checklist setChecklistItem(ApiClient& api, const std::string& id, const std::string& postId, int index, bool done,
                           const std::optional<std::string>& access) {
    if (index < 0) throw std::invalid_argument("No such item");
    return parse(api.call("PUT", postPath(id, postId, "/checklist/" + std::to_string(index)), Json{{"done", done}}, accessHeaders(access)),
                 checklistFrom);
}

Suggestion suggest(ApiClient& api, const std::string& id, const std::string& body, const std::vector<std::string>& media,
                   const std::optional<std::string>& access) {
    return parse(api.call("POST", ownPath(id, "/suggestions"), Json{{"body", body}, {"media", media}}, accessHeaders(access)), suggestionFrom);
}

std::vector<Suggestion> mySuggestions(ApiClient& api, const std::string& id) {
    return suggestionList(api.call("GET", ownPath(id, "/suggestions/mine")));
}

std::vector<Suggestion> suggestions(ApiClient& api, const std::string& id) {
    return suggestionList(api.call("GET", ownPath(id, "/suggestions")));
}

Post approveSuggestion(ApiClient& api, const std::string& id, const std::string& suggestionId, const std::optional<std::string>& body,
                       const std::optional<std::string>& publishAt) {
    Json edit = Json::object();
    if (body) edit["body"] = *body;
    edit["publishAt"] = publishAt ? Json(*publishAt) : Json(nullptr);
    return parse(api.call("POST", ownPath(id, "/suggestions/" + percentEncode(suggestionId) + "/approve"), edit), postFrom);
}

void rejectSuggestion(ApiClient& api, const std::string& id, const std::string& suggestionId) {
    api.call("POST", ownPath(id, "/suggestions/" + percentEncode(suggestionId) + "/reject"));
}

void report(ApiClient& api, const std::string& id, const std::string& reason) {
    api.call("POST", ownPath(id, "/report"), Json{{"reason", reason}});
}

}  // namespace redvault::api::channels
