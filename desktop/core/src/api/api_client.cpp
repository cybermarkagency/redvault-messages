#include "redvault/api/api_client.hpp"

#include <regex>
#include <utility>

namespace redvault::api {

namespace {

using net::percentEncode;

ApiError malformed(const std::string& detail) {
    return ApiError(0, "malformed_response", "Unexpected response from the server: " + detail);
}

std::string path(std::string_view prefix, const std::string& id, std::string_view suffix = {}) {
    std::string out(prefix);
    out += percentEncode(id);
    out += suffix;
    return out;
}

}  // namespace

ApiClient::ApiClient(std::string baseUrl, std::shared_ptr<net::HttpTransport> transport,
                     std::shared_ptr<net::SessionCookieJar> cookies)
    : baseUrl_(std::move(baseUrl)), transport_(std::move(transport)), cookies_(std::move(cookies)) {
    if (!net::parseUrl(baseUrl_)) throw std::invalid_argument("API base must be an absolute http(s) URL");
    if (!baseUrl_.ends_with('/')) baseUrl_ += '/';
}

std::string ApiClient::resolve(std::string_view relativePath) const {
    while (relativePath.starts_with('/')) relativePath.remove_prefix(1);
    return baseUrl_ + std::string(relativePath);
}

net::HttpResponse ApiClient::exchange(std::string_view method, std::string_view relativePath,
                                      const std::optional<Json>& body, bool withSession, const net::Headers& extraHeaders,
                                      const net::StreamOptions& streams) {
    net::HttpRequest request;
    request.method = std::string(method);
    request.url = resolve(relativePath);
    // The base URL was validated, and every path segment from a caller is percent-encoded.
    const net::Url url = *net::parseUrl(request.url);
    if (const auto cookie = withSession ? cookies_->headerFor(url) : std::string(); !cookie.empty()) {
        request.headers.emplace_back("cookie", cookie);
    }
    if (body) {
        request.headers.emplace_back("content-type", "application/json");
        request.body = body->dump();
    }
    request.headers.emplace_back("accept", "application/json");
    for (const auto& header : extraHeaders) request.headers.push_back(header);
    return perform(request, url, withSession, streams);
}

net::HttpResponse ApiClient::perform(const net::HttpRequest& request, const net::Url& url, bool withSession,
                                     const net::StreamOptions& streams) {
    net::HttpResponse response;
    try {
        response = streams.upload != nullptr || streams.download != nullptr ? transport_->sendStreaming(request, streams)
                                                                             : transport_->send(request);
    } catch (const net::TorUnavailableError&) {
        // Tor routing is on and Tor isn't connected: nothing went out (the outbox keeps sends).
        throw ApiError(0, "tor_unavailable", "Tor isn't connected. Your messages wait until it is.");
    } catch (const net::NetworkError&) {
        throw ApiError(0, "network_error", "Can't reach the server. Check your connection.");
    }
    // Errors can set cookies too (a failed MFA verify clearing the half session, say).
    if (withSession) cookies_->saveFromResponse(url, response.headersNamed("set-cookie"));

    if (response.status < 200 || response.status > 299) {
        ErrorBody parsed;
        try {
            parsed = Json::parse(response.body).get<ErrorBody>();
        } catch (const Json::exception&) {
            // Not the usual error shape (a proxy's HTML page, say): fall back to the status.
        }
        // A 502-504 without the API's own message is nginx's page: the API behind it is restarting or down.
        const bool proxyDown = response.status >= 502 && response.status <= 504;
        throw ApiError(response.status, parsed.error.value_or("unknown_error"),
                       parsed.message.value_or(proxyDown ? "The server is restarting or unavailable. Try again in a minute."
                                                         : "Request failed (HTTP " + std::to_string(response.status) + ")"));
    }
    return response;
}

Json ApiClient::call(std::string_view method, std::string_view relativePath, const std::optional<Json>& body,
                     const net::Headers& headers) {
    const auto response = exchange(method, relativePath, body, true, headers);
    if (response.body.empty()) return nullptr;
    try {
        return Json::parse(response.body);
    } catch (const Json::exception& e) {
        throw malformed(e.what());
    }
}

Json ApiClient::publicCall(std::string_view method, std::string_view relativePath, const std::optional<Json>& body,
                           const net::Headers& headers) {
    const auto response = exchange(method, relativePath, body, false, headers);
    if (response.body.empty()) return nullptr;
    try {
        return Json::parse(response.body);
    } catch (const Json::exception& e) {
        throw malformed(e.what());
    }
}

std::string ApiClient::publicFetch(std::string_view relativePath, const net::Headers& headers) {
    return exchange("GET", relativePath, std::nullopt, false, headers).body;
}

void ApiClient::publicDownload(std::string_view relativePath, std::ostream& sink, const net::Headers& headers) {
    exchange("GET", relativePath, std::nullopt, false, headers, net::StreamOptions{.download = &sink});
}

Json ApiClient::callUpload(std::string_view method, std::string_view relativePath, std::istream& source, std::uint64_t size,
                           std::string_view contentType) {
    const auto response = exchange(method, relativePath, std::nullopt, true, {{"content-type", std::string(contentType)}},
                                   net::StreamOptions{.upload = &source, .uploadSize = size});
    if (response.body.empty()) return nullptr;
    try {
        return Json::parse(response.body);
    } catch (const Json::exception& e) {
        throw malformed(e.what());
    }
}

std::string ApiClient::callText(std::string_view method, std::string_view relativePath) {
    return exchange(method, relativePath, std::nullopt).body;
}

template <typename T>
T ApiClient::callAs(std::string_view method, std::string_view relativePath, const std::optional<Json>& body) {
    const Json json = call(method, relativePath, body);
    try {
        return json.get<T>();
    } catch (const Json::exception& e) {
        throw malformed(e.what());
    }
}

std::string ApiClient::callRaw(std::string_view method, std::string_view relativePath, std::string body, std::string_view contentType) {
    net::HttpRequest request;
    request.method = std::string(method);
    request.url = resolve(relativePath);
    const net::Url url = *net::parseUrl(request.url);
    if (const auto cookie = cookies_->headerFor(url); !cookie.empty()) request.headers.emplace_back("cookie", cookie);
    if (!contentType.empty()) request.headers.emplace_back("content-type", std::string(contentType));
    request.headers.emplace_back("accept", "*/*");
    request.body = std::move(body);
    return perform(request, url, true).body;
}

std::string ApiClient::fetchAvatar(std::string_view avatarUrl) {
    // Exactly the shape the server's AvatarService.urlFor() produces, so a URL
    // from the server can't point this request anywhere else.
    static const std::regex kAvatarUrl(
        "^/api/users/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/avatar(\\?v=[0-9]{1,20})?$");
    if (!std::regex_match(avatarUrl.begin(), avatarUrl.end(), kAvatarUrl)) {
        throw ApiError(0, "malformed_response", "Unexpected avatar URL");
    }
    return exchange("GET", avatarUrl.substr(std::string_view("/api/").size()), std::nullopt).body;
}

std::string ApiClient::fetchCommunityImage(std::string_view path) {
    // Exactly the shapes the web app's communityImageUrl() and creatorAvatarUrl() produce
    // (an invitation token is base64url), so nothing else can be reached through here.
    static const std::regex kImagePath(
        "^/api/communities/(creators/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/avatar"
        "|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/(avatar|banner)(\\?invite=[A-Za-z0-9_-]{10,100})?)$");
    if (!std::regex_match(path.begin(), path.end(), kImagePath)) {
        throw ApiError(0, "malformed_response", "Unexpected community image path");
    }
    return exchange("GET", path.substr(std::string_view("/api/").size()), std::nullopt).body;
}

std::optional<std::string> ApiClient::uploadAvatarImage(const std::string& base64Jpeg) {
    return uploadAvatar({.image = base64Jpeg}).avatarUrl;
}

// --- Auth ---

AuthResult ApiClient::registerAccount(const RegisterRequest& body) {
    return callAs<AuthResult>("POST", "auth/register", Json(body));
}

PreLoginResult ApiClient::preLogin(const PreLoginRequest& body) {
    return callAs<PreLoginResult>("POST", "auth/prelogin", Json(body));
}

LoginResponse ApiClient::login(const LoginRequest& body) {
    return callAs<LoginResponse>("POST", "auth/login", Json(body));
}

LoginResponse ApiClient::verifyMfa(const MfaCodeRequest& body) {
    return callAs<LoginResponse>("POST", "auth/mfa/verify", Json(body));
}

void ApiClient::logout() {
    call("POST", "auth/logout");
}

MeResult ApiClient::me() {
    return callAs<MeResult>("GET", "auth/me");
}

UserResult ApiClient::getUser(const std::string& userId) {
    return callAs<UserResult>("GET", path("users/", userId));
}

// --- Conversations and messages ---

std::vector<ConversationDto> ApiClient::listConversations(bool includeCommunities) {
    return callAs<std::vector<ConversationDto>>("GET", includeCommunities ? "conversations?include=communities" : "conversations");
}

std::vector<ConversationDto> ApiClient::listConversationRequests() {
    return callAs<std::vector<ConversationDto>>("GET", "conversations/requests");
}

void ApiClient::respondToConversationRequest(const std::string& conversationId, bool accept) {
    call("POST", path("conversations/", conversationId, accept ? "/accept" : "/ignore"));
}

std::vector<ParticipantDto> ApiClient::listParticipants(const std::string& conversationId) {
    return callAs<std::vector<ParticipantDto>>("GET", path("conversations/", conversationId, "/participants"));
}

std::vector<MessageDto> ApiClient::listMessages(const std::string& conversationId, std::optional<int> limit,
                                                const std::optional<std::string>& before) {
    std::string query;
    if (limit) query += "limit=" + std::to_string(*limit);
    if (before) query += (query.empty() ? "" : "&") + std::string("before=") + percentEncode(*before);
    return callAs<std::vector<MessageDto>>(
        "GET", path("conversations/", conversationId, query.empty() ? "/messages" : "/messages?" + query));
}

MessageDto ApiClient::sendMessage(const std::string& conversationId, const SendMessageRequest& body) {
    return callAs<MessageDto>("POST", path("conversations/", conversationId, "/messages"), Json(body));
}

std::vector<ReceiptDto> ApiClient::listReceipts(const std::string& conversationId) {
    return callAs<std::vector<ReceiptDto>>("GET", path("conversations/", conversationId, "/receipts"));
}

void ApiClient::markRead(const std::string& conversationId, const MarkReadRequest& body) {
    call("POST", path("conversations/", conversationId, "/read"), Json(body));
}

void ApiClient::setDisappearing(const std::string& conversationId, std::optional<int> seconds) {
    Json body = {{"seconds", nullptr}};
    if (seconds) body["seconds"] = *seconds;
    call("PATCH", path("conversations/", conversationId, "/disappearing"), body);
}

void ApiClient::deleteMessage(const std::string& messageId) {
    call("DELETE", path("messages/", messageId));
}

VerificationDto ApiClient::getMessageVerification(const std::string& messageId) {
    return callAs<VerificationDto>("GET", path("messages/", messageId, "/verification"));
}

// --- Friends ---

UserLookupResult ApiClient::lookupUser(const std::string& username) {
    return callAs<UserLookupResult>("GET", "users/lookup?username=" + percentEncode(username));
}

FriendRequestDto ApiClient::sendFriendRequest(const UsernameRequest& body) {
    return callAs<FriendRequestDto>("POST", "friend-requests", Json(body));
}

std::vector<FriendRequestDto> ApiClient::incomingFriendRequests() {
    return callAs<std::vector<FriendRequestDto>>("GET", "friend-requests/incoming");
}

std::vector<FriendRequestDto> ApiClient::outgoingFriendRequests() {
    return callAs<std::vector<FriendRequestDto>>("GET", "friend-requests/outgoing");
}

CreatedConversation ApiClient::acceptFriendRequest(const std::string& requestId, const AcceptFriendRequest& body) {
    return callAs<CreatedConversation>("POST", path("friend-requests/", requestId, "/accept"), Json(body));
}

void ApiClient::denyFriendRequest(const std::string& requestId) {
    call("POST", path("friend-requests/", requestId, "/deny"));
}

void ApiClient::cancelFriendRequest(const std::string& requestId) {
    call("POST", path("friend-requests/", requestId, "/cancel"));
}

std::vector<FriendDto> ApiClient::listFriends() {
    return callAs<std::vector<FriendDto>>("GET", "friends");
}

// --- Account ---

void ApiClient::updateSettings(const SettingsRequest& body) {
    call("PATCH", "auth/settings", Json(body));
}

void ApiClient::changePassword(const ChangePasswordRequest& body) {
    call("PATCH", "auth/password", Json(body));
}

void ApiClient::changeUsername(const UsernameRequest& body) {
    call("PATCH", "auth/username", Json(body));
}

MfaSetupResult ApiClient::setupMfa() {
    return callAs<MfaSetupResult>("POST", "auth/mfa/setup");
}

void ApiClient::confirmMfa(const MfaCodeRequest& body) {
    call("POST", "auth/mfa/confirm", Json(body));
}

void ApiClient::disableMfa(const PasswordRequest& body) {
    call("POST", "auth/mfa/disable", Json(body));
}

void ApiClient::deleteAccount(const PasswordRequest& body) {
    call("POST", "auth/delete-account", Json(body));
}

AvatarResult ApiClient::uploadAvatar(const AvatarUploadRequest& body) {
    return callAs<AvatarResult>("PUT", "users/me/avatar", Json(body));
}

void ApiClient::removeAvatar() {
    call("DELETE", "users/me/avatar");
}

void ApiClient::submitReport(const ReportRequest& body) {
    call("POST", "reports", Json(body));
}

// --- Public ---

std::vector<TransparencyEventDto> ApiClient::transparencyFeed(int limit) {
    return callAs<std::vector<TransparencyEventDto>>("GET", "public/transparency-feed?limit=" + std::to_string(limit));
}

// --- Files ---

FileConfigDto ApiClient::fileConfig() {
    return callAs<FileConfigDto>("GET", "v1/files/config");
}

FileStorageUsageDto ApiClient::fileStorageUsage() {
    return callAs<FileStorageUsageDto>("GET", "v1/files/storage");
}

BillingConfigDto ApiClient::billingConfig() {
    return callAs<BillingConfigDto>("GET", "billing/config");
}

PlanStatusDto ApiClient::planStatus() {
    return callAs<PlanStatusDto>("GET", "billing/status");
}

PlanStatusDto ApiClient::cancelPlan() {
    return callAs<PlanStatusDto>("POST", "billing/cancel");
}

PlanStatusDto ApiClient::resumePlan() {
    return callAs<PlanStatusDto>("POST", "billing/resume");
}

PlanStatusDto ApiClient::switchPlan(const std::string& tier) {
    return callAs<PlanStatusDto>("POST", "billing/switch", Json{{"tier", tier}});
}

FileUploadAuthorization ApiClient::requestFileUpload(const FileUploadRequest& body) {
    return callAs<FileUploadAuthorization>("POST", "v1/files/upload-request", Json(body));
}

FileStatusDto ApiClient::completeFileUpload(const std::string& objectId) {
    // The server insists on an empty JSON object body.
    return callAs<FileStatusDto>("POST", path("v1/files/", objectId, "/complete"), Json::object());
}

FileStatusDto ApiClient::releaseFile(const std::string& objectId) {
    // Like /complete: an empty JSON object body.
    return callAs<FileStatusDto>("POST", path("v1/files/", objectId, "/release"), Json::object());
}

void ApiClient::deleteFile(const std::string& objectId) {
    call("DELETE", path("v1/files/", objectId));
}

FileDownloadDto ApiClient::fileDownload(const std::string& objectId) {
    return callAs<FileDownloadDto>("GET", path("v1/files/", objectId, "/download"));
}

// --- Blocking ---

std::vector<BlockedUserDto> ApiClient::listBlocked() {
    return callAs<std::vector<BlockedUserDto>>("GET", "blocks");
}

void ApiClient::block(const BlockRequest& body) {
    call("POST", "blocks", Json(body));
}

void ApiClient::unblock(const std::string& userId) {
    call("DELETE", path("blocks/", userId));
}

// --- Recovery key ---

void ApiClient::setRecoveryKey(const SetRecoveryRequest& body) {
    call("PUT", "auth/recovery", Json(body));
}

RecoveryStartResult ApiClient::recoveryStart(const RecoveryStartRequest& body) {
    return callAs<RecoveryStartResult>("POST", "auth/recovery/start", Json(body));
}

LoginResponse ApiClient::recoveryComplete(const RecoveryCompleteRequest& body) {
    return callAs<LoginResponse>("POST", "auth/recovery/complete", Json(body));
}

// --- Groups ---

CreatedConversation ApiClient::createConversation(const CreateConversationRequest& body) {
    return callAs<CreatedConversation>("POST", "conversations", Json(body));
}

void ApiClient::addParticipant(const std::string& conversationId, const WrappedKeyForUser& body) {
    call("POST", path("conversations/", conversationId, "/participants"), Json(body));
}

void ApiClient::publishKeyNotice(const std::string& conversationId, const KeyNoticeDto& notice) {
    call("PUT", path("conversations/", conversationId, "/key-notice"), Json(notice));
}

int ApiClient::rotateGroupKey(const std::string& conversationId, const RotateKeyRequest& body) {
    const Json result = call("POST", path("conversations/", conversationId, "/keys"), Json(body));
    return result.at("keyEpoch").get<int>();
}

void ApiClient::registerSigningKey(const std::string& signingPublicKey) {
    call("PUT", "auth/signing-key", Json{{"signingPublicKey", signingPublicKey}});
}

void ApiClient::removeParticipant(const std::string& conversationId, const std::string& userId) {
    call("DELETE", path("conversations/", conversationId, "/participants/" + percentEncode(userId)));
}

void ApiClient::setParticipantRole(const std::string& conversationId, const std::string& userId,
                                   const RoleRequest& body) {
    call("PATCH", path("conversations/", conversationId, "/participants/" + percentEncode(userId) + "/role"),
         Json(body));
}

void ApiClient::renameConversation(const std::string& conversationId, const RenameRequest& body) {
    call("PATCH", path("conversations/", conversationId, "/name"), Json(body));
}

}  // namespace redvault::api
