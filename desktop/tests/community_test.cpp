// Communities through the real repositories and crypto, against an in-memory
// server that keeps the same key-epoch rules as server/src/services/
// communityService.ts: it holds only sealed copies and ciphertext, refuses a
// send under an old epoch or while a rekey is due, and takes a rotation only
// from the current epoch and for exactly the enrolled members.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_chat_server.hpp"
#include "fake_transport.hpp"
#include "redvault/api/community_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/community_crypto.hpp"
#include "redvault/data/community_keys.hpp"
#include "redvault/data/conversation_repository.hpp"

using namespace redvault;
using redvault::test::makeUser;
using redvault::test::TestUser;
using Json = nlohmann::json;

namespace {

const std::string kBase = "https://example.com/api/";
const std::string kCommunityId = "6f9619ff-8b86-4011-b42d-00c04fc964ff";
const std::string kConversationId = "11111111-2222-4333-8444-555555555555";

class FakeCommunityServer {
public:
    struct Member {
        const TestUser* user = nullptr;
        std::string role = "member";
        bool canPost = false;
        std::string status = "active";  // or "pending_enrollment", "removed"
        std::map<std::int64_t, std::string> sealed;  // epoch -> wrapped key
    };

    std::string kind = "channel";
    std::int64_t keyEpoch = 0;
    bool rekeyRequired = false;
    std::map<std::string, Member> members;  // by user id
    std::vector<Json> messages;             // oldest first
    std::vector<std::pair<std::string, Json>> received;  // (user: method path, body)
    int rotations = 0;

    /// The creator's app made epoch 0 for this community id; the server stores its copy.
    void create(const TestUser& owner, const std::string& wrappedKey) {
        Member m{&owner, "owner", true, "active", {}};
        m.sealed[0] = wrappedKey;
        members.emplace(owner.id, std::move(m));
    }

    /// Paid or let in: waiting for any enrolled member's app to deliver the key.
    void addPending(const TestUser& user) { members.emplace(user.id, Member{&user, "member", false, "pending_enrollment", {}}); }

    /// Removed for cause: out of the conversation, every copy deleted, and nothing sent until a new epoch exists.
    void remove(const std::string& userId) {
        auto& m = members.at(userId);
        m.status = "removed";
        m.sealed.clear();
        rekeyRequired = true;
    }

    std::shared_ptr<api::ApiClient> clientFor(const TestUser& user) {
        auto transport = std::make_shared<test::FakeTransport>();
        const std::string userId = user.id;
        transport->handler = [this, userId](const net::HttpRequest& request) { return handle(userId, request); };
        return std::make_shared<api::ApiClient>(kBase, transport, std::make_shared<net::SessionCookieJar>());
    }

private:
    static net::HttpResponse ok(const Json& body, int status = 200) { return {status, {}, body.dump()}; }
    static net::HttpResponse noContent() { return {204, {}, ""}; }
    static net::HttpResponse error(int status, const std::string& code) {
        return {status, {}, Json{{"error", code}, {"message", code}}.dump()};
    }

    bool active(const std::string& userId) const {
        const auto it = members.find(userId);
        return it != members.end() && it->second.status == "active";
    }

    Json conversationFor(const Member& m) const {
        return Json{{"id", kConversationId},
                    {"isGroup", true},
                    {"role", m.role == "owner" ? "owner" : "member"},
                    {"nameCiphertext", nullptr},
                    {"nameNonce", nullptr},
                    {"wrappedKey", m.sealed.empty() ? "" : m.sealed.rbegin()->second},
                    {"disappearingSeconds", nullptr},
                    {"createdBy", "owner"},
                    {"createdAt", "2026-10-19T00:00:00.000Z"},
                    {"community",
                     {{"id", kCommunityId},
                      {"name", "Night Market Notes"},
                      {"kind", kind},
                      {"status", "published"},
                      {"hasAvatar", false},
                      {"role", m.role},
                      {"canPost", m.canPost},
                      {"keyEpoch", keyEpoch},
                      {"rekeyRequired", rekeyRequired}}}};
    }

    std::vector<std::string> enrolledIds() const {
        std::vector<std::string> out;
        for (const auto& [id, m] : members) {
            if (m.status == "active") out.push_back(id);
        }
        return out;
    }

    net::HttpResponse handle(const std::string& userId, const net::HttpRequest& request) {
        const std::string path = request.url.substr(kBase.size());
        const Json body = request.body.empty() ? Json() : Json::parse(request.body);
        if (request.method != "GET") received.emplace_back(userId + ": " + request.method + " " + path, body);
        const std::string community = "communities/" + kCommunityId;
        const std::string conversation = "conversations/" + kConversationId;

        if (request.method == "GET" && path == "conversations") return ok(Json::array());  // an older app never sees it
        if (request.method == "GET" && path == "conversations?include=communities") {
            Json list = Json::array();
            if (active(userId)) list.push_back(conversationFor(members.at(userId)));
            return ok(list);
        }
        if (path.rfind("users/", 0) == 0) {
            const auto it = members.find(path.substr(6));
            if (it == members.end()) return error(404, "not_found");
            const TestUser& u = *it->second.user;
            return ok(Json{{"userId", u.id}, {"username", u.username}, {"publicKey", u.publicKeyB64()}, {"createdAt", "2026-01-01T00:00:00.000Z"}});
        }
        if (request.method == "GET" && path == "communities/key-tasks") {
            Json list = Json::array();
            if (active(userId)) {
                int pending = 0;
                for (const auto& [id, m] : members) pending += m.status == "pending_enrollment" ? 1 : 0;
                list.push_back(Json{{"communityId", kCommunityId}, {"conversationId", kConversationId}, {"keyEpoch", keyEpoch},
                                    {"rekeyRequired", rekeyRequired}, {"pendingEnrollments", pending}, {"canPost", members.at(userId).canPost}});
            }
            return ok(list);
        }
        if (path == community + "/keys") {
            if (!active(userId)) return error(404, "not_found");
            Json keys = Json::array();
            for (const auto& [epoch, wrapped] : members.at(userId).sealed) keys.push_back(Json{{"epoch", epoch}, {"wrappedKey", wrapped}});
            return ok(Json{{"conversationId", kConversationId}, {"keyEpoch", keyEpoch}, {"rekeyRequired", rekeyRequired}, {"keys", keys}});
        }
        if (path == community + "/enrollments") {
            if (!active(userId)) return error(404, "not_found");
            if (request.method == "GET") {
                Json list = Json::array();
                for (const auto& [id, m] : members) {
                    if (m.status == "pending_enrollment") list.push_back(Json{{"userId", id}, {"publicKey", m.user->publicKeyB64()}});
                }
                return ok(list);
            }
            auto it = members.find(body.at("userId").get<std::string>());
            if (it == members.end() || it->second.status != "pending_enrollment") return error(409, "not_pending");
            if (rekeyRequired) return error(409, "rekey_required");
            if (body.at("epoch").get<std::int64_t>() != keyEpoch) return error(409, "stale_key_epoch");
            it->second.sealed[keyEpoch] = body.at("wrappedKey").get<std::string>();
            it->second.status = "active";
            return noContent();
        }
        if (path == community + "/roster") {
            if (!active(userId)) return error(404, "not_found");
            if (!members.at(userId).canPost) return error(403, "forbidden");
            Json list = Json::array();
            for (const auto& id : enrolledIds()) list.push_back(Json{{"userId", id}, {"publicKey", members.at(id).user->publicKeyB64()}});
            return ok(Json{{"keyEpoch", keyEpoch}, {"rekeyRequired", rekeyRequired}, {"members", list}});
        }
        if (path == community + "/rotate") {
            if (!active(userId)) return error(404, "not_found");
            if (!members.at(userId).canPost) return error(403, "forbidden");
            if (body.at("fromEpoch").get<std::int64_t>() != keyEpoch) return error(409, "stale_key_epoch");
            std::set<std::string> covered;
            for (const auto& k : body.at("keys")) covered.insert(k.at("userId").get<std::string>());
            const auto enrolled = enrolledIds();
            if (covered != std::set<std::string>(enrolled.begin(), enrolled.end()) || covered.size() != body.at("keys").size()) {
                return error(409, "member_set_changed");
            }
            ++keyEpoch;
            rekeyRequired = false;
            ++rotations;
            for (const auto& k : body.at("keys")) members.at(k.at("userId").get<std::string>()).sealed[keyEpoch] = k.at("wrappedKey").get<std::string>();
            return ok(Json{{"keyEpoch", keyEpoch}});
        }
        if (path.rfind(conversation + "/messages", 0) == 0) {
            if (!active(userId)) return error(404, "not_found");
            if (request.method == "GET") {
                Json page = Json::array();
                for (auto it = messages.rbegin(); it != messages.rend(); ++it) page.push_back(*it);  // newest first
                return ok(page);
            }
            const Member& sender = members.at(userId);
            if (!sender.canPost) return error(403, "cannot_post");
            if (rekeyRequired) return error(409, "rekey_required");
            if (!body.contains("keyEpoch") || body.at("keyEpoch").get<std::int64_t>() != keyEpoch) return error(409, "stale_key_epoch");
            char createdAt[40];
            std::snprintf(createdAt, sizeof createdAt, "2026-10-19T00:%02d:%02d.000Z", static_cast<int>(messages.size() / 60),
                          static_cast<int>(messages.size() % 60));
            Json message{{"id", "m" + std::to_string(messages.size() + 1)},
                         {"conversationId", kConversationId},
                         {"senderId", userId},
                         {"ciphertext", body.at("ciphertext")},
                         {"nonce", body.at("nonce")},
                         {"keyEpoch", keyEpoch},
                         {"createdAt", createdAt},
                         {"attachments", Json::array()}};
            messages.push_back(message);
            return ok(message, 201);
        }
        if (path == conversation + "/receipts") return ok(Json::array());
        if (path.rfind("communities/", 0) == 0) return ok(Json::object());  // display calls in the API tests below
        return error(404, "not_found");
    }
};

/// One member's app: its own API client, epoch keys and repository.
struct App {
    std::shared_ptr<api::ApiClient> api;
    std::shared_ptr<const data::SelfIdentity> identity;
    std::shared_ptr<data::CommunityKeyStore> keys;
    std::shared_ptr<data::ConversationRepository> chats;

    App(FakeCommunityServer& server, const TestUser& user)
        : api(server.clientFor(user)),
          identity(std::make_shared<const data::SelfIdentity>(user.identity())),
          keys(std::make_shared<data::CommunityKeyStore>(api, identity)),
          chats(std::make_shared<data::ConversationRepository>(api, data::ConversationRepository::HiddenIds{}, nullptr, keys)) {}

    /// The community chat as the chat list builds it.
    std::shared_ptr<const data::ConversationMeta> meta() {
        const auto list = chats->listConversations();
        REQUIRE(list.size() == 1);
        auto hydrated = chats->hydrateMeta(list[0], *identity);
        REQUIRE(hydrated.has_value());
        return std::make_shared<const data::ConversationMeta>(std::move(*hydrated));
    }

    std::vector<std::string> texts() {
        const auto m = meta();
        std::vector<std::string> out;
        for (const auto& message : chats->fetchMessages(*m, identity->userId).messages) {
            out.push_back(message.kind == data::MessageKind::Undecryptable ? "<unreadable>" : message.text);
        }
        return out;
    }
};

struct Fixture {
    TestUser creator = makeUser("owner", "nightmarket");
    TestUser fan = makeUser("fan", "fan_one");
    TestUser editor = makeUser("editor", "editor_two");
    FakeCommunityServer server;

    Fixture() {
        const auto epoch0 = crypto::community::newCommunityKey(kCommunityId, creator.keys.publicKey);
        server.create(creator, epoch0.wrappedKey);
    }

    /// The fan pays; the creator's background upkeep delivers the key.
    void subscribeFan(App& creatorApp) {
        server.addPending(fan);
        const auto upkeep = creatorApp.keys->runMaintenance();
        REQUIRE(upkeep.enrolled == 1);
    }
};

}  // namespace

TEST_CASE("an app without community keys never asks for communities", "[community]") {
    Fixture f;
    auto api = f.server.clientFor(f.creator);
    data::ConversationRepository legacy(api);
    CHECK(legacy.listConversations().empty());
    App app(f.server, f.creator);
    CHECK(app.chats->listConversations().size() == 1);
}

TEST_CASE("a community chat is named and keyed from the community, and read by epoch", "[community]") {
    Fixture f;
    App creator(f.server, f.creator);
    const auto meta = creator.meta();
    REQUIRE(meta->community.has_value());
    CHECK(meta->community->id == kCommunityId);
    CHECK(meta->community->kind == "channel");
    CHECK(meta->community->canPost);
    CHECK(meta->handle == "Night Market Notes");
    CHECK(meta->isGroup);
    REQUIRE(meta->members.size() == 1);
    CHECK(meta->members[0].isSelf);

    creator.chats->send(*meta, "first post, epoch 0");
    CHECK(f.server.received.back().second.at("keyEpoch") == 0);
    CHECK(creator.texts() == std::vector<std::string>{"first post, epoch 0"});
}

TEST_CASE("subscribe, receive the key, read; removed, a new key keeps them out", "[community]") {
    Fixture f;
    App creator(f.server, f.creator);
    creator.chats->send(*creator.meta(), "before the fan joined");

    App fan(f.server, f.fan);
    CHECK(fan.chats->listConversations().empty());  // waiting for the key: not in the chat yet
    f.subscribeFan(creator);
    creator.chats->send(*creator.meta(), "welcome, epoch 0");

    // The fan reads with their own copy; the creator's name shows on the posts.
    const auto fanMeta = fan.meta();
    CHECK_FALSE(fanMeta->community->canPost);
    const auto fetched = fan.chats->fetchMessages(*fanMeta, f.fan.id);
    REQUIRE(fetched.messages.size() == 2);
    CHECK(fetched.messages[1].text == "welcome, epoch 0");
    auto named = data::ConversationMeta{};
    named.community = fanMeta->community;
    named.members = fanMeta->members;
    fan.chats->addCommunitySenders(named, fetched.messages);
    CHECK(named.handleOf(f.creator.id) == "nightmarket");

    // A subscriber can't post in a channel; the server says so and nothing is retried.
    try {
        fan.chats->send(*fanMeta, "hello?");
        FAIL("a subscriber posted");
    } catch (const api::ApiError& e) {
        CHECK(e.code() == "cannot_post");
    }

    // Removed: the fan's copies are gone, and the creator's next post rotates first.
    const auto fanRing = fan.keys->ring(kConversationId);
    REQUIRE(fanRing);
    f.server.remove(f.fan.id);
    creator.chats->send(*creator.meta(), "after removal");
    CHECK(f.server.rotations == 1);
    CHECK(f.server.keyEpoch == 1);
    CHECK(f.server.messages.back().at("keyEpoch") == 1);
    CHECK(creator.texts() == std::vector<std::string>{"before the fan joined", "welcome, epoch 0", "after removal"});

    // The fan's app is shut out, and even with the ciphertext their saved keys open nothing new.
    CHECK(fan.chats->listConversations().empty());
    CHECK_THROWS_AS(fan.keys->load(kCommunityId, kConversationId), api::ApiError);
    const Json& newest = f.server.messages.back();
    CHECK_FALSE(crypto::community::decryptForCommunity(*fanRing, 1, crypto::base64Decode(newest.at("ciphertext").get<std::string>()),
                                                       crypto::base64Decode(newest.at("nonce").get<std::string>()))
                    .has_value());
    for (const auto& [epoch, key] : fanRing->keys) {
        CHECK_THROWS(crypto::decryptWithConversationKey(crypto::base64Decode(newest.at("ciphertext").get<std::string>()),
                                                        crypto::base64Decode(newest.at("nonce").get<std::string>()), key));
    }
    // The rotation covered exactly who remained.
    const auto& rotate = std::find_if(f.server.received.begin(), f.server.received.end(),
                                      [](const auto& r) { return r.first.find("/rotate") != std::string::npos; });
    REQUIRE(rotate != f.server.received.end());
    CHECK(rotate->second.at("fromEpoch") == 0);
    REQUIRE(rotate->second.at("keys").size() == 1);
    CHECK(rotate->second.at("keys")[0].at("userId") == f.creator.id);
}

TEST_CASE("a member whose keys are a step behind reloads and sends under the new epoch", "[community]") {
    Fixture f;
    f.server.kind = "group";
    App creator(f.server, f.creator);
    f.server.addPending(f.editor);
    REQUIRE(creator.keys->runMaintenance().enrolled == 1);
    f.server.members.at(f.editor.id).canPost = true;

    App editor(f.server, f.editor);
    const auto editorMeta = editor.meta();  // holds epoch 0
    editor.chats->send(*editorMeta, "epoch 0 from the editor");

    // Someone else leaves and the creator's upkeep rotates; the editor's ring still says epoch 0.
    f.server.addPending(f.fan);
    REQUIRE(creator.keys->runMaintenance().enrolled == 1);
    f.server.remove(f.fan.id);
    const auto upkeep = creator.keys->runMaintenance();
    CHECK(upkeep.rotated == 1);
    CHECK(f.server.keyEpoch == 1);

    editor.chats->send(*editorMeta, "epoch 1 from the editor");  // stale_key_epoch, reload, send
    CHECK(f.server.messages.back().at("keyEpoch") == 1);
    CHECK(f.server.rotations == 1);  // no second rotation: the editor only reloaded
    CHECK(creator.texts() == std::vector<std::string>{"epoch 0 from the editor", "epoch 1 from the editor"});
    CHECK(editor.texts() == std::vector<std::string>{"epoch 0 from the editor", "epoch 1 from the editor"});
}

TEST_CASE("upkeep: only members who can post rotate, and keys go only to who is waiting", "[community]") {
    Fixture f;
    App creator(f.server, f.creator);
    f.subscribeFan(creator);
    App fan(f.server, f.fan);
    f.server.addPending(f.editor);
    f.server.remove(f.editor.id);  // paid, then removed before anyone delivered the key: a rekey is due

    // The fan (read-only) sees the rekey but leaves it to someone who can post.
    const auto fanUpkeep = fan.keys->runMaintenance();
    CHECK(fanUpkeep.rotated == 0);
    CHECK(f.server.rotations == 0);

    const auto creatorUpkeep = creator.keys->runMaintenance();
    CHECK(creatorUpkeep.rotated == 1);
    CHECK(f.server.keyEpoch == 1);
    // The fan got the new epoch in that rotation and reads what follows.
    creator.chats->send(*creator.meta(), "epoch 1 post");
    CHECK(fan.texts() == std::vector<std::string>{"epoch 1 post"});
    // Nothing left to do: another round is quiet.
    const auto again = creator.keys->runMaintenance();
    CHECK(again.rotated == 0);
    CHECK(again.enrolled == 0);
}

TEST_CASE("community API calls go out as the web app sends them", "[community]") {
    auto transport = std::make_shared<test::FakeTransport>();
    api::ApiClient api(kBase, transport, std::make_shared<net::SessionCookieJar>());
    namespace c = api::community;

    transport->enqueue(200, R"({"subscriptionId":"s1","checkoutUrl":"https://checkout.stripe.com/c/pay/x","reused":false})");
    const auto checkout = c::checkout(api, kCommunityId, true, true);
    CHECK(transport->last().url == kBase + "communities/" + kCommunityId + "/checkout");
    // Strict: the platform and the two boxes, never an amount or a commission.
    CHECK(Json::parse(transport->last().body) == Json{{"platform", "desktop"}, {"termsAccepted", true}, {"ageConfirmed", true}});
    CHECK(checkout.checkoutUrl == "https://checkout.stripe.com/c/pay/x");

    transport->enqueue(201, R"({"id":"r1","platformReview":false})");
    c::report(api, kCommunityId, {"content", std::string("u1"), "spam", "", std::string("m1"), std::nullopt});
    // Not shared: no evidence leaves the app, even though the message id is known.
    CHECK(Json::parse(transport->last().body) == Json{{"targetType", "content"}, {"targetUserId", "u1"}, {"category", "spam"}, {"details", ""}});

    transport->enqueue(201, R"({"id":"r2","platformReview":true})");
    c::report(api, kCommunityId, {"content", std::string("u1"), "scam", "fake giveaway", std::string("m1"), std::string("send me $5")});
    const Json shared = Json::parse(transport->last().body);
    CHECK(shared.at("evidenceMessageId") == "m1");
    CHECK(shared.at("evidencePlaintext") == "send me $5");
    CHECK(shared.at("disclosureAcknowledged") == true);

    transport->enqueue(200, "[]");
    c::discover(api, "night market & co", "education", 30);
    CHECK(transport->last().url == kBase + "communities/discover?q=night%20market%20%26%20co&category=education&offset=30");

    transport->enqueue(200, R"({"url":"https://connect.stripe.com/setup/x"})");
    CHECK(c::startOnboarding(api, "us") == "https://connect.stripe.com/setup/x");

    transport->enqueue(201, "{}");
    c::CreateInput input;
    input.id = kCommunityId;
    input.kind = "channel";
    input.name = "Notes";
    input.category = "education";
    input.visibility = "discoverable";
    input.accessType = "paid";
    input.priceMinor = 1000;
    input.wrappedKey = "c2VhbGVk";
    c::create(api, input);
    const Json created = Json::parse(transport->last().body);
    CHECK(created.at("id") == kCommunityId);
    CHECK(created.at("priceMinor") == 1000);
    CHECK(created.at("memberLimit").is_null());
    CHECK(created.at("wrappedKey") == "c2VhbGVk");
    CHECK_FALSE(created.contains("commissionBps"));
}
