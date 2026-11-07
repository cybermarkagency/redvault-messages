#include <catch2/catch_test_macros.hpp>

#include "redvault/model/group_call_link.hpp"

using namespace redvault::model::group_calls;

namespace {
const std::string kCall = "3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b";
const std::string kGroup = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";
const std::string kBase = "https://www.redvaultsystems.com/redvaultmessages/";
}  // namespace

TEST_CASE("reads the call id out of a link or of the chat message carrying it", "[calls][group]") {
    // The same shapes the web app posts and parses (ui/src/lib/groupCallLink.ts).
    CHECK(parseCallLink(kBase + "?group-call=" + kCall) == kCall);
    CHECK(parseCallLink("  " + kBase + "?group-call=" + kCall + "\n") == kCall);
    CHECK(parseCallLink("Group video call started. To join, paste this link:\n" + kBase + "?group-call=" + kCall) == kCall);
    CHECK(parseCallLink("/redvaultmessages/?x=1&group-call=" + kCall + "#top") == kCall);

    std::string upper = kCall;
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(parseCallLink(kBase + "?group-call=" + upper) == kCall);
}

TEST_CASE("rejects anything that isn't a group call link", "[calls][group]") {
    CHECK_FALSE(parseCallLink(""));
    CHECK_FALSE(parseCallLink("hello there"));
    CHECK_FALSE(parseCallLink(kBase + "?group-call=not-an-id"));
    CHECK_FALSE(parseCallLink(kBase + "?group-call=" + kCall + "0"));
    CHECK_FALSE(parseCallLink(kBase + "?group-call=" + kCall.substr(1)));
    // A one-to-one call link, and the start address, are different things.
    CHECK_FALSE(parseCallLink(kBase + "?join=abcdefghijkl#" + std::string(43, 'A')));
    CHECK_FALSE(parseCallLink(kBase + "?start-group-call=" + kGroup + "&kind=video"));
    CHECK_FALSE(parseCallLink("group-call=" + kCall));
}

TEST_CASE("builds the addresses the web app opens", "[calls][group]") {
    CHECK(joinUrl(kBase, kCall) == kBase + "?group-call=" + kCall);
    CHECK(parseCallLink(joinUrl(kBase, kCall)) == kCall);

    CHECK(startUrl(kBase, kGroup, "video") == kBase + "?start-group-call=" + kGroup + "&kind=video");
    CHECK(startUrl(kBase, kGroup, "voice") == kBase + "?start-group-call=" + kGroup + "&kind=voice");
    CHECK(startUrl(kBase, kGroup, "anything else") == kBase + "?start-group-call=" + kGroup + "&kind=voice");
    CHECK(startUrl(kBase, "not-a-uuid", "video").empty());
}
