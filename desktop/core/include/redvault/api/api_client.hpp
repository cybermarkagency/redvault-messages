// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <istream>
#include <memory>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "redvault/api/api_error.hpp"
#include "redvault/api/dtos.hpp"
#include "redvault/net/cookie_jar.hpp"
#include "redvault/net/http.hpp"

namespace redvault::api {

/**
 * The backend's REST API, one blocking method per endpoint: the desktop
 * counterpart of android/.../data/api/ApiService.kt plus apiCall(). Every
 * failure surfaces as ApiError, exactly as Android's ApiException:
 *
 *   - non-2xx: the server's {"error", "message"} (code "unknown_error" if absent)
 *   - no HTTP response at all: status 0, code "network_error"
 *   - a 2xx body that isn't the expected shape: status 0, code "malformed_response"
 *
 * Calls block, so run them off the UI thread. Thread-safe as long as the
 * transport is (CurlTransport is); the cookie jar has its own lock.
 *
 * Push-token endpoints are left out: the desktop app doesn't register for
 * push, it polls while running.
 */
class ApiClient {
public:
    /// `baseUrl` is the API root, e.g. "https://example.com/api/" (a trailing slash is added if missing).
    ApiClient(std::string baseUrl, std::shared_ptr<net::HttpTransport> transport,
              std::shared_ptr<net::SessionCookieJar> cookies);

    const std::string& baseUrl() const noexcept { return baseUrl_; }
    net::SessionCookieJar& cookies() noexcept { return *cookies_; }
    /// For raw transfers (encrypted file bodies) that aren't JSON API calls.
    net::HttpTransport& transport() noexcept { return *transport_; }

    /// Resolves a path relative to the API base (a leading slash is tolerated).
    std::string resolve(std::string_view relativePath) const;

    // --- Auth ---
    AuthResult registerAccount(const RegisterRequest& body);
    PreLoginResult preLogin(const PreLoginRequest& body);
    LoginResponse login(const LoginRequest& body);
    /// Completes a login that answered mfaRequired. Uses the half-session cookie login() set.
    LoginResponse verifyMfa(const MfaCodeRequest& body);
    void logout();
    MeResult me();
    UserResult getUser(const std::string& userId);

    // --- Conversations and messages ---
    /**
     * Your chats. Communities are listed only with `includeCommunities`
     * (`?include=communities`): only an app that holds their epoch keys
     * (data/community_keys.hpp) can read them.
     */
    std::vector<ConversationDto> listConversations(bool includeCommunities = false);
    /// 1:1 chats someone who isn't your friend started with you, waiting for accept or ignore.
    std::vector<ConversationDto> listConversationRequests();
    /// Accepting moves the chat into the normal list; ignoring hides it (the sender isn't told).
    void respondToConversationRequest(const std::string& conversationId, bool accept);
    std::vector<ParticipantDto> listParticipants(const std::string& conversationId);
    /**
     * Newest first. Without arguments the server returns the most recent 50;
     * `limit` (1-200) asks for more, and `before` (an ISO time) pages back
     * to messages created strictly before it.
     */
    std::vector<MessageDto> listMessages(const std::string& conversationId, std::optional<int> limit = std::nullopt,
                                         const std::optional<std::string>& before = std::nullopt);
    MessageDto sendMessage(const std::string& conversationId, const SendMessageRequest& body);
    std::vector<ReceiptDto> listReceipts(const std::string& conversationId);
    void markRead(const std::string& conversationId, const MarkReadRequest& body);
    /// Sends {"seconds": n}, or an explicit {"seconds": null} to turn disappearing messages off.
    void setDisappearing(const std::string& conversationId, std::optional<int> seconds);
    /// Deletes your own message for real: the server clears its ciphertext.
    void deleteMessage(const std::string& messageId);
    VerificationDto getMessageVerification(const std::string& messageId);

    // --- Friends ---
    UserLookupResult lookupUser(const std::string& username);
    FriendRequestDto sendFriendRequest(const UsernameRequest& body);
    std::vector<FriendRequestDto> incomingFriendRequests();
    std::vector<FriendRequestDto> outgoingFriendRequests();
    /// The caller seals a fresh conversation key for both sides; the server creates the friendship and chat together.
    CreatedConversation acceptFriendRequest(const std::string& requestId, const AcceptFriendRequest& body);
    void denyFriendRequest(const std::string& requestId);
    void cancelFriendRequest(const std::string& requestId);
    std::vector<FriendDto> listFriends();

    // --- Account ---
    void updateSettings(const SettingsRequest& body);
    /// Sends the new password with the private key re-wrapped under it, atomically. Revokes other sessions.
    void changePassword(const ChangePasswordRequest& body);
    void changeUsername(const UsernameRequest& body);
    MfaSetupResult setupMfa();
    void confirmMfa(const MfaCodeRequest& body);
    void disableMfa(const PasswordRequest& body);
    void deleteAccount(const PasswordRequest& body);
    /// `image` is base64 of a small square JPEG.
    AvatarResult uploadAvatar(const AvatarUploadRequest& body);
    void removeAvatar();
    void submitReport(const ReportRequest& body);

    // --- Public ---
    std::vector<TransparencyEventDto> transparencyFeed(int limit);

    // --- Files ---
    FileConfigDto fileConfig();
    FileStorageUsageDto fileStorageUsage();
    FileUploadAuthorization requestFileUpload(const FileUploadRequest& body);
    FileStatusDto completeFileUpload(const std::string& objectId);
    /// Lets a finished upload be downloaded without a server message pointing at it (a protocol-3 file). Uploader only.
    FileStatusDto releaseFile(const std::string& objectId);
    void deleteFile(const std::string& objectId);
    FileDownloadDto fileDownload(const std::string& objectId);

    // --- Billing (Stripe; the server's webhook, not the app, is what upgrades an account) ---
    BillingConfigDto billingConfig();
    PlanStatusDto planStatus();
    /// Switch to Free: the card subscription stops renewing; Pro lasts until the paid period ends.
    PlanStatusDto cancelPlan();
    /// Undoes cancelPlan() while the period lasts.
    PlanStatusDto resumePlan();
    /// Moves a card subscription between "pro" and "premium"; Stripe charges or credits the difference.
    PlanStatusDto switchPlan(const std::string& tier);

    // --- Blocking (server-enforced) ---
    std::vector<BlockedUserDto> listBlocked();
    void block(const BlockRequest& body);
    void unblock(const std::string& userId);

    // --- Recovery key ---
    void setRecoveryKey(const SetRecoveryRequest& body);
    RecoveryStartResult recoveryStart(const RecoveryStartRequest& body);
    LoginResponse recoveryComplete(const RecoveryCompleteRequest& body);

    // --- Groups (server-enforced: friends only; owner/admin manage members) ---
    CreatedConversation createConversation(const CreateConversationRequest& body);
    void addParticipant(const std::string& conversationId, const WrappedKeyForUser& body);
    /// Moves a group to its next key after someone left or was removed (db/migrations/030); returns the new epoch.
    int rotateGroupKey(const std::string& conversationId, const RotateKeyRequest& body);
    /// A signed notice for a group's key. ApiError "stale_key_notice" (409): a newer one is already there.
    void publishKeyNotice(const std::string& conversationId, const KeyNoticeDto& notice);
    /// Registers this account's message-signing public key (base64; set once, the same key again is fine).
    void registerSigningKey(const std::string& signingPublicKey);
    /// Remove a member (owner/admin), or leave (your own id).
    void removeParticipant(const std::string& conversationId, const std::string& userId);
    void setParticipantRole(const std::string& conversationId, const std::string& userId, const RoleRequest& body);
    void renameConversation(const std::string& conversationId, const RenameRequest& body);

    /**
     * One exchange against the API origin, with the session cookie, mapping
     * failures to ApiError like every endpoint above. `body`, when present,
     * is sent as JSON. Returns the parsed 2xx body (null when empty).
     */
    Json call(std::string_view method, std::string_view relativePath, const std::optional<Json>& body = std::nullopt,
              const net::Headers& headers = {});

    /**
     * Like call(), with the body streamed from `source` (exactly `size`
     * bytes, sent with that Content-Length) instead of JSON: uploads that
     * can be far too big to hold in memory.
     */
    Json callUpload(std::string_view method, std::string_view relativePath, std::istream& source, std::uint64_t size,
                    std::string_view contentType);

    /**
     * Like call(), but WITHOUT the session cookie, and a cookie the answer
     * sets is ignored: for public reads (channels) that the server mustn't
     * be able to tie to an account, as the web app sends them with
     * credentials "omit". `headers` (lowercase names) go along, e.g. a
     * private channel's access token.
     */
    Json publicCall(std::string_view method, std::string_view relativePath, const std::optional<Json>& body = std::nullopt,
                    const net::Headers& headers = {});

    /// A GET like publicCall() (no cookie) whose answer isn't JSON: returns the raw body (a channel's photo).
    std::string publicFetch(std::string_view relativePath, const net::Headers& headers = {});

    /// publicFetch() streamed into `sink` instead of memory (a channel's file, which can be gigabytes).
    void publicDownload(std::string_view relativePath, std::ostream& sink, const net::Headers& headers = {});

    /**
     * Fetches a profile photo by the `avatarUrl` the server hands out
     * ("/api/users/<id>/avatar?v=..."), with the session cookie, like
     * Android's avatar loader. Returns the raw image bytes. Anything that
     * isn't such a path is refused rather than fetched.
     */
    std::string fetchAvatar(std::string_view avatarUrl);

    /**
     * Fetches a community's picture or banner, or a creator's picture, with
     * the session cookie (the server serves them only to someone who may see
     * the community, like the web app's same-origin <img>). Only the paths
     * api/community_api.hpp's imagePath() and creatorAvatarPath() make are
     * fetched: "/api/communities/<id>/avatar" or ".../banner", optionally
     * with "?invite=<token>", and "/api/communities/creators/<id>/avatar".
     * Anything else is refused rather than fetched. Returns the raw bytes.
     */
    std::string fetchCommunityImage(std::string_view path);

    /** `image` is base64 of a small square JPEG. Returns the new avatar URL. */
    std::optional<std::string> uploadAvatarImage(const std::string& base64Jpeg);

    /// Like call(), for an endpoint that answers with plain text (the password manager's breach check).
    std::string callText(std::string_view method, std::string_view relativePath);

    /**
     * Like call(), with the session cookie, for raw bytes both ways (story
     * media): `body` is sent as `contentType` (nothing when empty), and the
     * 2xx answer's body comes back as is.
     */
    std::string callRaw(std::string_view method, std::string_view relativePath, std::string body = {},
                        std::string_view contentType = {});

private:
    /// One exchange with cookies and the ApiError mapping; returns the 2xx response.
    net::HttpResponse exchange(std::string_view method, std::string_view relativePath, const std::optional<Json>& body,
                               bool withSession = true, const net::Headers& extraHeaders = {}, const net::StreamOptions& streams = {});
    /// Sends a built request: cookies saved, non-2xx mapped to ApiError.
    net::HttpResponse perform(const net::HttpRequest& request, const net::Url& url, bool withSession,
                              const net::StreamOptions& streams = {});

    template <typename T>
    T callAs(std::string_view method, std::string_view relativePath, const std::optional<Json>& body = std::nullopt);

    std::string baseUrl_;
    std::shared_ptr<net::HttpTransport> transport_;
    std::shared_ptr<net::SessionCookieJar> cookies_;
};

}  // namespace redvault::api
