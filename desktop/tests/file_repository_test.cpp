#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

#include "fake_chat_server.hpp"
#include "redvault/data/file_repository.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace redvault;
using namespace redvault::data;
using redvault::test::FakeChatServer;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("redvault-test-" + util::randomUuid());
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const fs::path& path, const std::string& content) {
    std::ofstream(path, std::ios::binary) << content;
}

struct Fixture {
    test::TestUser alice = test::makeUser("a1", "alice");
    test::TestUser bob = test::makeUser("b1", "bob");
    FakeChatServer server;
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::shared_ptr<net::SessionCookieJar> cookies = std::make_shared<net::SessionCookieJar>();
    std::shared_ptr<api::ApiClient> api = std::make_shared<api::ApiClient>(FakeChatServer::kBase, transport, cookies);
    std::shared_ptr<ConversationRepository> conversations = std::make_shared<ConversationRepository>(api);
    TempDir temp;
    int sleeps = 0;
    FileRepository files{api, conversations, temp.path / "cache", [this](std::chrono::milliseconds) { ++sleeps; }};
    ConversationMeta meta;

    Fixture() {
        server.attach(*transport);
        server.addUser(alice);
        server.addUser(bob);
        server.addConversation("c1", alice, {&bob});
        cookies->saveFromResponse(*net::parseUrl(FakeChatServer::kBase + "auth/login"), {"sid=session; Path=/"});
        auto hydrated = conversations->hydrateMeta(api::Json(server.conversations[0]).get<api::ConversationDto>(),
                                                   alice.identity());
        REQUIRE(hydrated);
        meta = std::move(*hydrated);
    }

    fs::path source(const std::string& name, const std::string& content) {
        const auto path = temp.path / name;
        writeAll(path, content);
        return path;
    }
};

std::string sampleContent() {
    // Spans two secretstream chunks.
    std::string content;
    for (int i = 0; i < 70'000; ++i) content.push_back(static_cast<char>(i * 31 % 251));
    return content;
}

}  // namespace

TEST_CASE("a file round-trips through a proxy upload and download", "[files]") {
    Fixture f;
    std::vector<FileSendStage> stages;
    const auto sent = f.files.send(f.meta, f.source("holiday photo.jpg", sampleContent()),
                                   [&](FileSendStage stage) { stages.push_back(stage); });
    CHECK(stages == std::vector{FileSendStage::Encrypting, FileSendStage::Uploading, FileSendStage::Sending});
    CHECK(sent.kind == MessageKind::File);
    REQUIRE(sent.file);
    CHECK(sent.file->filename == "holiday photo.jpg");
    CHECK(sent.file->mimeType == "image/jpeg");
    CHECK(sent.file->originalSize == 70'000);

    // Only ciphertext reached the server, with the session cookie on the proxy transfer.
    const auto& blob = f.server.blobs.at(sent.file->objectId);
    CHECK(blob.size() == static_cast<std::size_t>(sent.file->encryptedSize));
    CHECK(blob.find(sampleContent().substr(0, 64)) == std::string::npos);
    CHECK(test::FakeTransport::headerOf(f.server.transfers.at(0), "cookie") == "sid=session");
    CHECK(test::FakeTransport::headerOf(f.server.transfers.at(0), "content-type") == "application/octet-stream");
    // The message lists the object as its attachment, and the descriptor is inside the ciphertext only.
    const auto body = f.server.bodiesFor("conversations/c1/messages").at(0);
    CHECK(body["attachments"] == api::Json::array({sent.file->objectId}));
    CHECK(body.dump().find("holiday") == std::string::npos);

    // The outgoing ciphertext file is cleaned up.
    CHECK(fs::is_empty(f.temp.path / "cache" / "outgoing"));

    const auto downloaded = f.files.download(*sent.file);
    CHECK(downloaded.filename() == "holiday photo.jpg");
    CHECK(readAll(downloaded) == sampleContent());
    CHECK_FALSE(fs::exists(downloaded.parent_path() / ".ciphertext"));

    f.files.clearCache();
    CHECK_FALSE(fs::exists(downloaded));
}

#ifndef _WIN32
TEST_CASE("decrypted files and their folders are this user's alone, whatever the umask", "[files][modes]") {
    const mode_t saved = ::umask(022);
    struct Restore {
        mode_t mode;
        ~Restore() { ::umask(mode); }
    } restore{saved};
    const auto modeOf = [](const fs::path& path) {
        struct stat info {};
        REQUIRE(::stat(path.c_str(), &info) == 0);
        return info.st_mode & 07777;
    };

    Fixture f;
    const auto sent = f.files.send(f.meta, f.source("holiday photo.jpg", sampleContent()));
    REQUIRE(sent.file);
    CHECK(modeOf(f.temp.path / "cache") == 0700);
    CHECK(modeOf(f.temp.path / "cache" / "outgoing") == 0700);

    const auto downloaded = f.files.download(*sent.file);
    CHECK(readAll(downloaded) == sampleContent());  // control: it's still the file
    CHECK(modeOf(downloaded) == 0600);
    CHECK(modeOf(downloaded.parent_path()) == 0700);
    CHECK(modeOf(downloaded.parent_path().parent_path()) == 0700);
    CHECK(modeOf(f.temp.path / "cache") == 0700);
}
#endif

TEST_CASE("direct transfers to the storage provider are refused, so it never sees the user's IP", "[files][privacy]") {
    Fixture f;
    f.server.transferMode = "direct";
    CHECK_THROWS_AS(f.files.send(f.meta, f.source("notes.txt", "secret notes")), api::UserFacingError);
    CHECK(f.server.transfers.empty());
    for (const auto& request : f.transport->requests) CHECK_FALSE(request.url.starts_with(FakeChatServer::kBucket));
}

TEST_CASE("transfers retry 5xx with backoff", "[files]") {
    Fixture f;
    f.server.transferFailuresToServe = 2;
    const auto sent = f.files.send(f.meta, f.source("a.bin", "abc"));
    CHECK(f.server.transfers.size() == 3);
    CHECK(f.sleeps == 2);

    f.server.transferFailuresToServe = 4;
    try {
        f.files.download(*sent.file);
        FAIL("expected UserFacingError");
    } catch (const api::UserFacingError& e) {
        CHECK(std::string(e.what()) == "Transfer failed (503). Try again.");
    }
    // Nothing partial is left behind.
    CHECK(fs::is_empty(f.temp.path / "cache" / "decrypted"));
}

TEST_CASE("tampered or expired downloads fail cleanly", "[files]") {
    Fixture f;
    const auto sent = f.files.send(f.meta, f.source("a.bin", sampleContent()));

    f.server.corruptDownloads = true;
    try {
        f.files.download(*sent.file);
        FAIL("expected UserFacingError");
    } catch (const api::UserFacingError& e) {
        CHECK(std::string(e.what()) == "Unable to decrypt this file. It may be corrupted or tampered with.");
    }
    CHECK(fs::is_empty(f.temp.path / "cache" / "decrypted"));

    f.server.corruptDownloads = false;
    f.server.expiresAt = "2000-01-01T00:00:00.000Z";
    CHECK_THROWS_WITH(f.files.download(*sent.file), "The transfer link expired. Try again.");
}

TEST_CASE("files over the plan's limit are refused before uploading", "[files]") {
    Fixture f;
    CHECK_THROWS_WITH(f.files.send(f.meta, f.source("big.bin", std::string(1024 * 1024 + 1, 'x'))),
                      "File too large. The limit is 1 MB.");
    CHECK(f.server.transfers.empty());
    CHECK_THROWS_WITH(f.files.send(f.meta, f.temp.path / "missing.bin"), "Couldn't read that file.");
}

TEST_CASE("a device-key upload is released and sends no message", "[files]") {
    Fixture f;
    const std::string content = sampleContent();
    const auto path = f.source("report.pdf", content);
    std::vector<FileSendStage> stages;
    const std::size_t messagesBefore = f.server.messages["c1"].size();

    // No conversation repository: a device-key chat has no server message to send.
    FileRepository files{f.api, nullptr, f.temp.path / "v3-cache", [](std::chrono::milliseconds) {}};
    const auto descriptor = files.uploadForDeviceKeys("c1", path, [&](FileSendStage stage) { stages.push_back(stage); });

    CHECK(stages == std::vector{FileSendStage::Encrypting, FileSendStage::Uploading, FileSendStage::Sending});
    CHECK(f.server.released == std::vector{descriptor.objectId});
    CHECK(f.server.messages["c1"].size() == messagesBefore);
    CHECK(descriptor.filename == "report.pdf");
    CHECK(descriptor.mimeType == "application/pdf");
    CHECK(model::file_message::parse(model::file_message::serialize(descriptor)).has_value());

    // The ciphertext is what went up; the descriptor alone opens it.
    CHECK(f.server.blobs.at(descriptor.objectId).find(content.substr(0, 64)) == std::string::npos);
    CHECK(readAll(files.download(descriptor)) == content);
}

TEST_CASE("a released file can be deleted from the server", "[files]") {
    Fixture f;
    FileRepository files{f.api, nullptr, f.temp.path / "v3-cache", [](std::chrono::milliseconds) {}};
    const auto descriptor = files.uploadForDeviceKeys("c1", f.source("a.txt", "hello"));
    REQUIRE(f.server.blobs.contains(descriptor.objectId));
    f.api->deleteFile(descriptor.objectId);
    CHECK_FALSE(f.server.blobs.contains(descriptor.objectId));
}

TEST_CASE("guesses MIME types from the extension", "[files]") {
    CHECK(FileRepository::guessMimeType("a/B.JPG") == "image/jpeg");
    CHECK(FileRepository::guessMimeType("voice.m4a") == "audio/mp4");
    CHECK(FileRepository::guessMimeType("archive.tar.gz") == "application/octet-stream");
    CHECK(FileRepository::guessMimeType("noextension") == "application/octet-stream");
}
