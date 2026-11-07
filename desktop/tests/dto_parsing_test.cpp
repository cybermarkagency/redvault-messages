// Parses response bodies in the exact shapes the live server returns (the
// same samples as Android's DtoParsingTest), so a mismatch fails here
// instead of as "Something went wrong." in the app.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "redvault/api/api_error.hpp"
#include "redvault/api/dtos.hpp"

using namespace redvault::api;

namespace {

template <typename T>
T parse(const char* text) {
    return Json::parse(text).get<T>();
}

}  // namespace

TEST_CASE("receipts have no timestamp", "[dto]") {
    const auto receipts = parse<std::vector<ReceiptDto>>(R"([{"messageId":"m1","userId":"u1","status":"read"}])");
    REQUIRE(receipts.size() == 1);
    CHECK(receipts[0].status == "read");
}

TEST_CASE("listed message has no deleteAt or verification", "[dto]") {
    const auto message = parse<MessageDto>(R"({"id":"m1","conversationId":"c1","senderId":"u1","ciphertext":"AA==","nonce":"AA==",
        "createdAt":"2026-09-24T09:24:19.676Z","editedAt":null,"deletedAt":null,
        "attachments":[{"objectId":"o1","status":"available","encryptedSize":17}],"verification":null})");
    CHECK_FALSE(message.deleteAt);
    CHECK_FALSE(message.verification);
    REQUIRE(message.attachments.size() == 1);
    CHECK(message.attachments[0].objectId == "o1");
    CHECK(message.attachments[0].encryptedSize == 17);
}

TEST_CASE("deleted message has no ciphertext", "[dto]") {
    const auto message = parse<MessageDto>(R"({"id":"m1","conversationId":"c1","senderId":"u1","ciphertext":null,"nonce":null,
        "createdAt":"2026-09-24T09:24:19.676Z","deletedAt":"2026-09-24T10:00:00.000Z"})");
    CHECK_FALSE(message.ciphertext);
    CHECK(message.attachments.empty());
    CHECK(message.deletedAt == "2026-09-24T10:00:00.000Z");
}

TEST_CASE("conversation, participant and user", "[dto]") {
    const auto conversation = parse<ConversationDto>(R"({"id":"c1","isGroup":false,"role":"owner","nameCiphertext":null,
        "nameNonce":null,"wrappedKey":"AA==","disappearingSeconds":null,"createdBy":"u1","createdAt":"2026-09-24T09:24:19.120Z"})");
    CHECK_FALSE(conversation.disappearingSeconds);
    CHECK(parse<ParticipantDto>(R"({"userId":"u1","role":"owner","joinedAt":"2026-09-24T09:24:19.120Z"})").role == "owner");
    const auto user = parse<UserResult>(R"({"userId":"u1","username":"a","publicKey":"AA==","avatarUrl":null,
        "createdAt":"2026-09-24T09:24:19.120Z","deleted":false})");
    CHECK_FALSE(user.deleted);
}

TEST_CASE("friend request, lookup and me", "[dto]") {
    const auto request = parse<FriendRequestDto>(R"({"id":"r1","requesterId":"u1","recipientId":"u2","status":"pending",
        "createdAt":"2026-09-24T09:24:19.120Z","respondedAt":null})");
    CHECK(request.status == "pending");
    CHECK(parse<UserLookupResult>(R"({"userId":"u2","publicKey":"AA==","avatarUrl":null})").userId == "u2");
    const auto me = parse<MeResult>(R"({"userId":"u1","username":"a","publicKey":"AA==","acceptMessageRequests":true,
        "transparencyMode":false,"avatarUrl":"/api/users/u1/avatar?v=1","role":"user","subscriptionTier":"free","mfaEnabled":false})");
    CHECK(me.acceptMessageRequests);
    CHECK_FALSE(me.hasRecoveryKey);
    CHECK(me.avatarUrl == "/api/users/u1/avatar?v=1");
}

TEST_CASE("unknown keys are ignored and missing required keys throw", "[dto]") {
    CHECK(parse<ReceiptDto>(R"({"messageId":"m","userId":"u","status":"read","extra":{"x":1}})").messageId == "m");
    CHECK_THROWS_AS(parse<ReceiptDto>(R"({"messageId":"m","userId":"u"})"), Json::exception);
}

TEST_CASE("login response: MFA challenge, then a full result", "[dto]") {
    const auto challenge = parse<LoginResponse>(R"({"mfaRequired":true})");
    CHECK(challenge.mfaRequired);
    CHECK_THROWS_AS(toLoginResult(challenge), ApiError);

    const auto full = parse<LoginResponse>(R"({"userId":"u1","acceptMessageRequests":true,"transparencyMode":false,
        "avatarUrl":null,"expiresAt":"2026-10-25T00:00:00.000Z","role":"user","subscriptionTier":"pro",
        "publicKey":"pk","encryptedPrivateKey":"epk","privateKeyNonce":"n","privateKeyKdfSalt":"s"})");
    CHECK_FALSE(full.mfaRequired);
    const auto result = toLoginResult(full);
    CHECK(result.subscriptionTier == "pro");
    CHECK_FALSE(result.avatarUrl);
}

TEST_CASE("file transfer targets keep their headers", "[dto]") {
    const auto auth = parse<FileUploadAuthorization>(R"({"objectId":"o1","maxEncryptedSize":100,
        "upload":{"mode":"direct","url":"https://bucket/x","method":"PUT","headers":{"content-type":"application/octet-stream"},
        "expiresAt":"2026-09-24T10:00:00.000Z"}})");
    CHECK(auth.upload.mode == "direct");
    CHECK(auth.upload.headers.at("content-type") == "application/octet-stream");
    CHECK(parse<FileConfigDto>(R"({"maxFileSize":1,"maxEncryptedSize":2,"transferMode":"proxy"})").maxAttachmentsPerMessage == 1);
}

TEST_CASE("billing config: a null payment link means billing is off", "[dto]") {
    CHECK_FALSE(parse<BillingConfigDto>(R"({"paymentLink":null})").paymentLink);
    CHECK(parse<BillingConfigDto>(R"({"paymentLink":"https://buy.stripe.com/x"})").paymentLink == "https://buy.stripe.com/x");
}

TEST_CASE("plan status, as the server sends it", "[dto]") {
    const auto cancelling = parse<PlanStatusDto>(
        R"({"tier":"pro","source":"stripe","renews":false,"endsOn":"2026-10-26","canCancel":false,"canResume":true})");
    CHECK(cancelling.endsOn == "2026-10-26");
    CHECK(cancelling.canResume);
    CHECK_FALSE(cancelling.canCancel);
    const auto free = parse<PlanStatusDto>(
        R"({"tier":"free","source":null,"renews":false,"endsOn":null,"canCancel":false,"canResume":false})");
    CHECK(free.tier == "free");
    CHECK_FALSE(free.source);
    CHECK_FALSE(free.endsOn);
}

TEST_CASE("requests leave out unset optionals", "[dto]") {
    CHECK(Json(SettingsRequest{.transparencyMode = true}).dump() == R"({"transparencyMode":true})");
    CHECK(Json(SendMessageRequest{.ciphertext = "c", .nonce = "n"}).dump() == R"({"ciphertext":"c","nonce":"n"})");
    CHECK(Json(SendMessageRequest{.ciphertext = "c", .nonce = "n", .attachments = std::vector<std::string>{"o1"}}).dump() ==
          R"({"attachments":["o1"],"ciphertext":"c","nonce":"n"})");

    RegisterRequest registration{.username = "a", .password = "p", .publicKey = "k", .encryptedPrivateKey = "e",
                                 .privateKeyNonce = "n", .privateKeyKdfSalt = "s"};
    CHECK_FALSE(Json(registration).contains("recovery"));
    registration.recovery = RecoveryDto{"auth", "enc", "nonce"};
    CHECK(Json(registration)["recovery"]["recoveryAuth"] == "auth");
    // The sign-up boxes go by the server's names, unticked unless set.
    CHECK(Json(registration)["terms_accepted"] == false);
    registration.termsAccepted = registration.privacyAcknowledged = registration.age18Confirmed = true;
    CHECK(Json(registration)["terms_accepted"] == true);
    CHECK(Json(registration)["privacy_acknowledged"] == true);
    CHECK(Json(registration)["age_18_confirmed"] == true);

    const Json group = CreateConversationRequest{.isGroup = true, .participants = {{"u1", "w1"}}};
    CHECK_FALSE(group.contains("nameCiphertext"));
    CHECK(group["participants"][0]["wrappedKey"] == "w1");
}
