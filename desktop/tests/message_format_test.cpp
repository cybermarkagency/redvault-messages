// Ports of Android's RichMessageTest and FileMessageTest (themselves pinned
// to ui/src/test/richMessage.test.ts and fileMessage.ts), so a reply,
// reaction or file sent from any client reads the same on the others.
// Invisible characters are written as \x byte escapes on purpose.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "redvault/model/file_message.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using namespace redvault::model;

namespace {

const std::string kMsg = "3f2504e0-4f89-11d3-9a0c-0305e82c3301";
const std::string kUser = "9b2e6a3c-1d4f-4e7a-8b5c-2a1f0e9d8c7b";
const std::string kThumbsUp = "\xF0\x9F\x91\x8D";
const std::string kHeart = "\xE2\x9D\xA4\xEF\xB8\x8F";
const std::string kRlo = "\xE2\x80\xAE";  // U+202E RIGHT-TO-LEFT OVERRIDE
const std::string kZwsp = "\xE2\x80\x8B";  // U+200B ZERO WIDTH SPACE

const std::string kWebDescriptor =
    file_message::kPrefix +
    R"({"type":"file","version":1,"objectId":"3F2504E0-4F89-11D3-9A0C-0305E82C3301",)"
    R"("fileKey":"Y2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTw=","filename":"report.pdf","mimeType":"application/pdf",)"
    R"("originalSize":70000,"encryptedSize":70034,)"
    R"("ciphertextSha256":"b22cc5359bf22587e30e474b37a248c166664dc7b251eec50d0dfedeefbc0e19",)"
    R"("encryption":{"algorithm":"xchacha20poly1305-secretstream","version":1,"header":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYX","chunkSize":65536}})";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

}  // namespace

// --- RichMessage ---

TEST_CASE("rich messages match the web wire format exactly", "[format][rich]") {
    // The same expected strings as ui/src/test/richMessage.test.ts.
    CHECK(rich_message::serializeReaction(kMsg, kThumbsUp) ==
          std::string("\0rv-reaction:", 13) + R"({"type":"reaction","version":1,"messageId":")" + kMsg +
              R"(","emoji":")" + kThumbsUp + R"("})");
    CHECK(rich_message::serializeReply("hi", {kMsg, kUser, "yo"}) ==
          std::string("\0rv-msg:", 8) + R"({"type":"text","version":1,"text":"hi","replyTo":{"messageId":")" + kMsg +
              R"(","senderId":")" + kUser + R"(","preview":"yo"}})");
}

TEST_CASE("rich messages round-trip replies and reactions", "[format][rich]") {
    const auto reply = rich_message::serializeReply("sounds good", {kMsg, kUser, "lunch at 1?"});
    CHECK(rich_message::isStructured(reply));
    CHECK(rich_message::parse(reply) == ParsedBody{parsed::Text{"sounds good", ReplyRef{kMsg, kUser, "lunch at 1?"}}});
    CHECK(rich_message::parse(rich_message::serializeReaction(kMsg, kHeart)) == ParsedBody{parsed::Reaction{kMsg, kHeart}});
    // Ids come back lowercase whatever case was sent.
    const auto upper = rich_message::serializeReaction("3F2504E0-4F89-11D3-9A0C-0305E82C3301", kThumbsUp);
    CHECK(std::get<parsed::Reaction>(rich_message::parse(upper)).messageId == kMsg);
}

TEST_CASE("rich messages reject anything but one emoji, and malformed bodies", "[format][rich]") {
    const auto invalid = ParsedBody{parsed::Invalid{}};
    // A reaction is exactly one emoji (see the "any single emoji" case below).
    CHECK(rich_message::parse(replaced(rich_message::serializeReaction(kMsg, kThumbsUp), kThumbsUp, "xx")) == invalid);
    CHECK(rich_message::parse(replaced(rich_message::serializeReaction(kMsg, kThumbsUp), kThumbsUp,
                                       "\xF0\x9F\x91\x8D\xF0\x9F\x91\x8D")) == invalid);
    CHECK(rich_message::parse(std::string("\0rv-msg:not json", 16)) == invalid);
    CHECK(rich_message::parse(std::string("\0rv-msg:", 8) + R"({"type":"text","version":2,"text":"x"})") == invalid);
    CHECK(rich_message::parse(std::string("\0rv-msg:", 8) +
                              R"({"type":"text","version":1,"text":"x","replyTo":{"messageId":"../x"}})") == invalid);
    CHECK_THROWS_AS(rich_message::serializeReaction(kMsg, "x"), std::invalid_argument);
    CHECK_FALSE(rich_message::isStructured("plain text"));
}

TEST_CASE("edits and forwards are text messages older apps can still read", "[format][rich]") {
    const auto edit = rich_message::serializeEdit("fixed typo", "3F2504E0-4F89-11D3-9A0C-0305E82C3301");
    CHECK(edit == std::string("\0rv-msg:", 8) + R"({"type":"text","version":1,"text":"fixed typo","edits":")" +
                      std::string("3F2504E0-4F89-11D3-9A0C-0305E82C3301") + R"("})");
    const auto parsedEdit = std::get<parsed::Text>(rich_message::parse(edit));
    CHECK(parsedEdit.text == "fixed typo");
    CHECK(parsedEdit.edits == kMsg);
    CHECK_FALSE(parsedEdit.forwarded);

    const auto forwarded = rich_message::serializeForwarded("look at this");
    CHECK(forwarded == std::string("\0rv-msg:", 8) + R"({"type":"text","version":1,"text":"look at this","forwarded":true})");
    CHECK(rich_message::parse(forwarded) == ParsedBody{parsed::Text{"look at this", std::nullopt, std::nullopt, true}});

    // Malformed additions are rejected rather than half-applied.
    const auto invalid = ParsedBody{parsed::Invalid{}};
    CHECK(rich_message::parse(std::string("\0rv-msg:", 8) + R"({"type":"text","version":1,"text":"x","edits":"nope"})") == invalid);
    CHECK(rich_message::parse(std::string("\0rv-msg:", 8) + R"({"type":"text","version":1,"text":"x","forwarded":"yes"})") == invalid);
}

TEST_CASE("reply previews are sanitized and capped", "[format][rich]") {
    CHECK(rich_message::makePreview("a" + kRlo + "b" + kZwsp + "c\n\nd") == "abc d");
    const auto capped = rich_message::makePreview(std::string(300, 'x'));
    CHECK(util::utf16Length(util::decodeUtf8(capped)) == 200);
    CHECK(capped.ends_with("\xE2\x80\xA6"));  // …
}

// --- FileMessage ---

TEST_CASE("file descriptors parse the web format", "[format][file]") {
    const auto d = file_message::parse(kWebDescriptor);
    REQUIRE(d);
    CHECK(d->objectId == "3f2504e0-4f89-11d3-9a0c-0305e82c3301");
    CHECK(d->filename == "report.pdf");
    CHECK(d->encryptedSize == 70'034);
    CHECK(d->encryption.chunkSize == 65'536);
}

TEST_CASE("file descriptors round-trip in the web's field order", "[format][file]") {
    const auto d = file_message::parse(kWebDescriptor);
    REQUIRE(d);
    CHECK(file_message::parse(file_message::serialize(*d)) == d);
    CHECK(file_message::serialize(*d).starts_with(file_message::kPrefix + R"({"type":"file","version":1,"objectId")"));
}

TEST_CASE("malformed file descriptors are rejected", "[format][file]") {
    CHECK_FALSE(file_message::parse("hello"));
    CHECK_FALSE(file_message::parse(file_message::kPrefix + "not json"));
    CHECK_FALSE(file_message::parse(replaced(kWebDescriptor, R"("chunkSize":65536)", R"("chunkSize":1024)")));
    CHECK_FALSE(file_message::parse(replaced(kWebDescriptor, "3F2504E0", "../../etc")));
    CHECK_FALSE(file_message::parse(replaced(kWebDescriptor, R"("originalSize":70000)", R"("originalSize":-1)")));
    CHECK_FALSE(file_message::parse(replaced(kWebDescriptor, R"("version":1,"objectId")", R"("version":"1","objectId")")));
}

TEST_CASE("unknown MIME types fall back to opaque bytes", "[format][file]") {
    const auto d = file_message::parse(replaced(kWebDescriptor, "application/pdf", "text/html<script>"));
    REQUIRE(d);
    CHECK(d->mimeType == "application/octet-stream");
}

TEST_CASE("filenames are sanitized like Android", "[format][file]") {
    CHECK(file_message::sanitizeFilename("../../etc/passwd") == "passwd");
    CHECK(file_message::sanitizeFilename("..\\..\\Windows\\win.ini") == "win.ini");
    CHECK(file_message::sanitizeFilename("invoice" + kRlo + "gpj.exe") == "invoicegpj.exe");
    CHECK(file_message::sanitizeFilename(".bashrc") == "bashrc");
    CHECK(file_message::sanitizeFilename("CON.txt") == "_CON.txt");
    CHECK(file_message::sanitizeFilename("a<b>.txt") == "a_b_.txt");
    CHECK(file_message::sanitizeFilename("...") == "file");
    CHECK(file_message::sanitizeFilename("trailing. . ") == "trailing");
    const auto longName = file_message::sanitizeFilename(std::string(300, 'x') + ".pdf");
    CHECK(longName.size() == 150);
    CHECK(longName.ends_with(".pdf"));
}

TEST_CASE("sanitized names never end in a space or dot, even after cutting", "[format][file]") {
    // Found by fuzzing: cutting to 150 left a trailing space, which Windows drops when it creates the file.
    const auto cut = file_message::sanitizeFilename(std::string(149, 'x') + " " + std::string(50, 'y'));
    CHECK(cut == std::string(149, 'x'));
    const auto dots = file_message::sanitizeFilename(std::string(149, 'x') + "." + std::string(50, 'y'));
    CHECK_FALSE(dots.ends_with('.'));
}

TEST_CASE("reserved Windows names, without a regex", "[format][file]") {
    CHECK(file_message::sanitizeFilename("con") == "_con");
    CHECK(file_message::sanitizeFilename("Lpt9.log") == "_Lpt9.log");
    CHECK(file_message::sanitizeFilename("CONOUT$.txt") == "_CONOUT$.txt");
    CHECK(file_message::sanitizeFilename("COM\xC2\xB9.txt") == "_COM\xC2\xB9.txt");
    CHECK(file_message::sanitizeFilename("NUL .txt") == "_NUL .txt");
    CHECK(file_message::sanitizeFilename("console.txt") == "console.txt");
    CHECK(file_message::sanitizeFilename("com10.txt") == "com10.txt");
    // The old pattern's `(\..*)?` recursed once per character: a long reserved name overflowed the stack.
    const auto huge = file_message::sanitizeFilename("con." + std::string(500'000, 'a'));
    CHECK(huge.starts_with("_con."));
}

TEST_CASE("a huge file key from a peer is checked without a regex", "[format][file]") {
    const std::string key = "Y2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTw=";
    CHECK(file_message::parse(replaced(kWebDescriptor, key, std::string(2'000'000, 'A'))));
    CHECK_FALSE(file_message::parse(replaced(kWebDescriptor, key, std::string(2'000'000, 'A') + "!")));
    CHECK_FALSE(file_message::parse(replaced(kWebDescriptor, key, "AAAA===")));
    CHECK_FALSE(file_message::parse(replaced(kWebDescriptor, key, "==")));
}

TEST_CASE("only inert types open, judged on the name actually written", "[format][file]") {
    CHECK(file_message::opensInertly("photo.jpg"));
    for (const std::string name : {"invoice.exe", "invoice.exe.", "invoice.exe ", "invoice.exe. . .", "setup.msi.", "run.ps1 ",
                                   "invoice.exe\xE2\x80\xAE", "invoice.exe\xE2\x80\x8B", "page.html", "x.svg", "noextension"}) {
        CHECK_FALSE(file_message::opensInertly(name));
    }
}

TEST_CASE("byte sizes format like Android", "[format][file]") {
    CHECK(file_message::formatBytes(512) == "512 B");
    CHECK(file_message::formatBytes(39'731) == "39 KB");
    CHECK(file_message::formatBytes(5'000) == "4.9 KB");
    CHECK(file_message::formatBytes(1'258'291) == "1.2 MB");
    CHECK(file_message::formatBytes(50LL * 1024 * 1024) == "50 MB");
}

// --- util ---

TEST_CASE("ISO timestamps parse to Unix milliseconds", "[util]") {
    CHECK(util::parseIsoMillis("1970-01-01T00:00:00.000Z") == 0);
    CHECK(util::parseIsoMillis("2026-09-24T09:24:19.676Z") == 1'790'241'859'676LL);
    CHECK(util::parseIsoMillis("2026-09-24T09:24:19Z") == 1'790'241'859'000LL);
    CHECK(util::parseIsoMillis("2026-09-24T11:24:19.676+02:00") == 1'790'241'859'676LL);
    CHECK_FALSE(util::parseIsoMillis("2026-02-30T00:00:00Z"));
    CHECK_FALSE(util::parseIsoMillis("yesterday"));
}

TEST_CASE("UTF-8 helpers count like JavaScript and survive bad input", "[util]") {
    CHECK(util::utf16Length(util::decodeUtf8(kThumbsUp)) == 2);
    CHECK(util::utf16Length(util::decodeUtf8("abc")) == 3);
    CHECK(util::decodeUtf8("\xC0\xAF") == std::u32string(U"��"));  // overlong '/' is rejected
    CHECK(util::encodeUtf8(util::decodeUtf8("h\xC3\xA9llo " + kThumbsUp)) == "h\xC3\xA9llo " + kThumbsUp);
    CHECK(util::randomUuid().size() == 36);
}

// The same cases as ui/src/test/richFeatures.test.ts and android RichFeaturesTest.kt.
TEST_CASE("any single emoji is a reaction, nothing else is", "[format][rich]") {
    const std::string zwj = "‍";
    const std::vector<std::string> one = {
        "👍", "❤️", "💩", "👍🏽", "👨" + zwj + "👩" + zwj + "👧", "🏳️" + zwj + "🌈", "🇳🇱", "1️⃣", "#⃣",
        "🏴\U000E0067\U000E0062\U000E0073\U000E0063\U000E0074\U000E007F", "👩🏽" + zwj + "❤️" + zwj + "💋" + zwj + "👨🏻", "☺️", "©️",
    };
    for (const auto& emoji : one) {
        INFO(emoji);
        CHECK(rich_message::isReactionEmoji(emoji));
        CHECK(rich_message::parse(rich_message::serializeReaction(kMsg, emoji)) == ParsedBody{parsed::Reaction{kMsg, emoji}});
    }
    // At most 16 code points.
    std::string fifteen;
    for (int i = 0; i < 7; ++i) fifteen += "👍" + zwj;
    fifteen += "👍";
    CHECK(rich_message::isReactionEmoji(fifteen));
    const std::vector<std::string> notOne = {
        "", "x", "👍👍", "👍 ", " 👍", "🇳", "🏽", zwj + "👍", "👍" + zwj, "1", "11⃣", "👍\n", "🇳🇱🇳🇱",
        fifteen + zwj + "👍", "\xF0\x9F\x91", "\xED\xA0\xBD",
    };
    for (const auto& text : notOne) {
        INFO(text);
        CHECK_FALSE(rich_message::isReactionEmoji(text));
    }
    CHECK_THROWS_AS(rich_message::serializeReaction(kMsg, "👍👍"), std::invalid_argument);
}
