// Resumable Files-tab transfers over Tor (docs/tor-routing-plan.md): uploads
// in parts that survive an interruption or a restart, downloads that continue
// with HTTP Range, cancellation, and integrity checks on every piece.
#include <catch2/catch_test_macros.hpp>
#include <sodium.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <string>

#include "fake_transport.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/drive_repository.hpp"
#include "redvault/net/cookie_jar.hpp"
#include "redvault/util/text.hpp"

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace redvault;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("redvault-drive-test-" + util::randomUuid());
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::string hexSha(const std::string& bytes) {
    unsigned char d[32];
    crypto_hash_sha256(d, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    char hex[65];
    sodium_bin2hex(hex, sizeof hex, d, 32);
    return hex;
}

/// The server's /api/backup parts protocol (server/src/services/backupService.ts), in memory.
class FakeBackupServer final : public net::HttpTransport {
public:
    struct Upload {
        std::uint64_t size = 0;
        std::uint64_t partSize = 0;
        int partCount = 0;
        std::string manifest;
        std::map<int, std::string> parts;
        bool complete = false;
        bool deleted = false;
    };
    std::map<std::string, Upload> uploads;
    std::vector<std::string> log;  // "METHOD path"
    int failPartPutsAfter = -1;    // >= 0: part PUTs fail (network) once this many went through
    int partPuts = 0;
    std::uint64_t cutDownloadAt = 0;  // > 0: the first download breaks off after this many bytes
    bool downloadCut = false;

    net::HttpResponse send(const net::HttpRequest& request) override { return handle(request, nullptr); }

    net::HttpResponse sendStreaming(const net::HttpRequest& request, const net::StreamOptions& streams) override {
        net::HttpRequest buffered = request;
        if (streams.upload != nullptr) {
            buffered.body.assign(static_cast<std::size_t>(streams.uploadSize), '\0');
            streams.upload->read(buffered.body.data(), static_cast<std::streamsize>(buffered.body.size()));
            if (static_cast<std::uint64_t>(streams.upload->gcount()) != streams.uploadSize) throw net::NetworkError("Upload body ended early");
        }
        return handle(buffered, streams.download);
    }

private:
    static std::string header(const net::HttpRequest& r, const std::string& name) { return test::FakeTransport::headerOf(r, name); }
    static net::HttpResponse json(int status, const Json& body) { return {status, {{"content-type", "application/json"}}, body.dump()}; }

    net::HttpResponse handle(const net::HttpRequest& r, std::ostream* download) {
        const std::string path = net::parseUrl(r.url)->path;
        log.push_back(r.method + " " + path);
        std::smatch m;
        if (r.method == "GET" && path == "/api/backup/usage") return json(200, {{"used", 0}, {"limit", 1LL << 40}, {"remaining", 1LL << 40}, {"tier", "pro"}});
        if (r.method == "POST" && path == "/api/backup/upload-request") {
            const Json body = Json::parse(r.body);
            if (!body.contains("partSize")) return json(400, {{"error", "test_expects_parts"}});
            Upload u;
            u.size = body.at("encryptedSize").get<std::uint64_t>();
            u.partSize = body.at("partSize").get<std::uint64_t>();
            u.partCount = static_cast<int>((u.size + u.partSize - 1) / u.partSize);
            u.manifest = body.at("partsSha256").get<std::string>();
            const std::string id = util::randomUuid();
            uploads[id] = u;
            return json(201, {{"objectId", id},
                              {"upload", {{"mode", "parts"}, {"url", "/backup/" + id + "/parts"}, {"method", "PUT"}, {"headers", Json::object()},
                                          {"expiresAt", "2099-01-01T00:00:00.000Z"}, {"partSize", u.partSize}, {"partCount", u.partCount}}}});
        }
        if (std::regex_match(path, m, std::regex("/api/backup/([^/]+)/parts/(\\d+)")) && r.method == "PUT") {
            auto& u = uploads.at(m[1]);
            if (failPartPutsAfter >= 0 && partPuts >= failPartPutsAfter) throw net::NetworkError("Tor circuit closed");
            ++partPuts;
            const int n = std::stoi(m[2]);
            const std::uint64_t expected = n < u.partCount ? u.partSize : u.size - u.partSize * (u.partCount - 1);
            if (r.body.size() != expected || hexSha(r.body) != header(r, "x-content-sha256")) return json(400, {{"error", "bad_part"}});
            u.parts[n] = r.body;
            return json(200, {{"partNumber", n}});
        }
        if (std::regex_match(path, m, std::regex("/api/backup/([^/]+)/parts")) && r.method == "GET") {
            Json received = Json::array();
            for (const auto& [n, bytes] : uploads.at(m[1]).parts) received.push_back({{"partNumber", n}, {"size", bytes.size()}, {"sha256", hexSha(bytes)}});
            return json(200, {{"received", received}});
        }
        if (std::regex_match(path, m, std::regex("/api/backup/([^/]+)/complete"))) {
            auto& u = uploads.at(m[1]);
            if (static_cast<int>(u.parts.size()) != u.partCount) return json(409, {{"error", "upload_incomplete"}});
            std::string digests;
            for (const auto& [n, bytes] : u.parts) {
                unsigned char d[32];
                crypto_hash_sha256(d, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
                digests.append(reinterpret_cast<const char*>(d), 32);
            }
            if (hexSha(digests) != u.manifest) return json(400, {{"error", "manifest"}});
            u.complete = true;
            return json(200, Json::object());
        }
        if (std::regex_match(path, m, std::regex("/api/backup/([^/]+)/download"))) {
            return json(200, {{"download", {{"mode", "proxy"}, {"url", "/backup/" + std::string(m[1]) + "/content"}, {"method", "GET"},
                                            {"headers", Json::object()}, {"expiresAt", "2099-01-01T00:00:00.000Z"}}}});
        }
        if (std::regex_match(path, m, std::regex("/api/backup/([^/]+)/content")) && r.method == "GET") {
            std::string whole;
            for (const auto& [n, bytes] : uploads.at(m[1]).parts) whole += bytes;
            std::uint64_t start = 0;
            const std::string range = header(r, "range");
            if (!range.empty()) start = std::stoull(range.substr(6, range.find('-') - 6));
            std::string body = whole.substr(start);
            if (cutDownloadAt > 0 && !downloadCut) {
                downloadCut = true;
                if (download) download->write(body.data(), static_cast<std::streamsize>(cutDownloadAt));
                throw net::NetworkError("Tor circuit closed mid-download");
            }
            if (download) download->write(body.data(), static_cast<std::streamsize>(body.size()));
            return {range.empty() ? 200 : 206, {}, download ? std::string() : body};
        }
        if (std::regex_match(path, m, std::regex("/api/backup/([^/]+)")) && r.method == "DELETE") {
            if (uploads.count(m[1])) uploads.at(m[1]).deleted = true;
            return {204, {}, {}};
        }
        return json(404, {{"error", "not_found"}});
    }
};

struct Fixture {
    TempDir temp;
    std::shared_ptr<FakeBackupServer> server = std::make_shared<FakeBackupServer>();
    std::shared_ptr<api::ApiClient> api =
        std::make_shared<api::ApiClient>("http://test.invalid/api/", server, std::make_shared<net::SessionCookieJar>());
    std::shared_ptr<data::SelfIdentity> identity = [] {
        auto id = std::make_shared<data::SelfIdentity>();
        auto pair = crypto::generateKeyPair();
        id->userId = "user-1";
        id->publicKey = pair.publicKey;
        id->privateKey = std::move(pair.privateKey);
        return id;
    }();

    std::unique_ptr<data::DriveRepository> repository() {
        auto repo = std::make_unique<data::DriveRepository>(api, identity, temp.path / "cache", [](std::chrono::milliseconds) {});
        repo->enableResumable(temp.path / "journal", data::DriveRepository::deriveJournalKey(identity->privateKey));
        return repo;
    }

    fs::path file(std::size_t size) {
        std::string bytes(size, '\0');
        randombytes_buf(bytes.data(), bytes.size());
        const fs::path p = temp.path / "holiday-video.mp4";
        std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return p;
    }

    std::string read(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

constexpr std::size_t kPart = data::DriveRepository::kPartSize;

}  // namespace

TEST_CASE("drive: an upload in parts resumes after an interruption, sending only what's missing", "[drive][tor]") {
    Fixture f;
    const auto source = f.file(2 * kPart + 12345);  // 3 parts
    f.server->failPartPutsAfter = 1;
    auto repo = f.repository();
    CHECK_THROWS_AS(repo->upload(source, "", std::nullopt), api::TransferInterrupted);
    REQUIRE(repo->pendingUploads().size() == 1);
    const auto pending = repo->pendingUploads().front();
    CHECK(pending.name == "holiday-video.mp4");
    CHECK(pending.partCount == 3);
    CHECK(f.server->uploads.at(pending.objectId).parts.size() == 1);

    f.server->failPartPutsAfter = -1;
    const int putsBefore = f.server->partPuts;
    const auto entry = repo->resumeUpload(pending.objectId);
    CHECK(f.server->partPuts - putsBefore == 2);  // parts 2 and 3 only
    CHECK(f.server->uploads.at(pending.objectId).complete);
    CHECK(entry.id == pending.objectId);
    CHECK(entry.meta.contains("key"));
    CHECK(repo->pendingUploads().empty());
    CHECK(fs::is_empty(f.temp.path / "journal" / "downloads"));

    // And it downloads back byte for byte.
    const fs::path out = f.temp.path / "back.mp4";
    repo->download(entry, out);
    CHECK(f.read(out) == f.read(source));
}

TEST_CASE("drive: a pending upload survives a restart (sealed journal) and finishes once", "[drive][tor]") {
    Fixture f;
    const auto source = f.file(kPart + 1);
    f.server->failPartPutsAfter = 0;
    CHECK_THROWS(f.repository()->upload(source, "", std::nullopt));

    // Nothing readable on disk: not the file's name or its key.
    std::string disk;
    for (const auto& e : fs::recursive_directory_iterator(f.temp.path / "journal")) {
        if (e.is_regular_file() && e.path().extension() == ".upload") disk += f.read(e.path());
    }
    CHECK(disk.find("holiday-video") == std::string::npos);

    f.server->failPartPutsAfter = -1;
    auto restarted = f.repository();  // a new session, same account
    const auto pending = restarted->pendingUploads();
    REQUIRE(pending.size() == 1);
    restarted->resumeUpload(pending.front().objectId);
    CHECK(f.server->uploads.at(pending.front().objectId).complete);
    CHECK(restarted->pendingUploads().empty());

    // Another account's key can't open the journal.
    Fixture other;
    CHECK(other.repository()->pendingUploads().empty());
}

TEST_CASE("drive: cancelling an upload stops it and drops it on the server and here", "[drive][tor]") {
    Fixture f;
    const auto source = f.file(3 * kPart);
    auto control = std::make_shared<data::TransferControl>();
    std::string cancelledId;
    auto repo = f.repository();
    CHECK_THROWS_AS(repo->upload(source, "", std::nullopt,
                                 [&](data::DriveStage stage, std::uint64_t done, std::uint64_t) {
                                     if (stage == data::DriveStage::Uploading && done > kPart) control->cancelled = true;
                                 },
                                 nlohmann::json::object(), nullptr, control),
                    data::TransferCancelled);
    CHECK(repo->pendingUploads().empty());
    bool deleted = false;
    for (const auto& [id, u] : f.server->uploads) deleted = deleted || u.deleted;
    CHECK(deleted);
}

TEST_CASE("drive: a broken download continues from where it stopped (Range) and decrypts intact", "[drive][tor]") {
    Fixture f;
    const auto source = f.file(kPart + 500000);
    auto repo = f.repository();
    const auto entry = repo->upload(source, "", std::nullopt);
    f.server->cutDownloadAt = 300000;
    f.server->log.clear();
    const fs::path out = f.temp.path / "copy.mp4";
    repo->download(entry, out);
    CHECK(f.read(out) == f.read(source));
    // The retry asked only for the rest.
    int contentGets = 0;
    for (const auto& line : f.server->log) contentGets += line.find("/content") != std::string::npos ? 1 : 0;
    CHECK(contentGets == 2);
}

TEST_CASE("drive: a part the server got wrong is sent again; a damaged local copy is refused", "[drive][tor]") {
    Fixture f;
    const auto source = f.file(2 * kPart);
    f.server->failPartPutsAfter = 1;
    auto repo = f.repository();
    CHECK_THROWS(repo->upload(source, "", std::nullopt));
    const auto pending = repo->pendingUploads().front();
    const auto id = pending.objectId;
    // The server's copy of part 1 is corrupt (a storage fault): resume must resend it, and the rest.
    f.server->uploads.at(id).parts[1][10] ^= 0x01;
    f.server->failPartPutsAfter = -1;
    const int before = f.server->partPuts;
    repo->resumeUpload(id);
    CHECK(f.server->partPuts - before == pending.partCount);
    CHECK(f.server->uploads.at(id).complete);

    // A local encrypted copy changed on disk is never uploaded as the part it claims to be.
    Fixture g;
    const auto other = g.file(2 * kPart);
    g.server->failPartPutsAfter = 0;
    auto repo2 = g.repository();
    CHECK_THROWS(repo2->upload(other, "", std::nullopt));
    const auto pendingId = repo2->pendingUploads().front().objectId;
    const fs::path copy = g.temp.path / "journal" / (pendingId + ".ciphertext");
    std::string bytes = g.read(copy);
    bytes[100] ^= 0x01;
    std::ofstream(copy, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    g.server->failPartPutsAfter = -1;
    CHECK_THROWS_AS(repo2->resumeUpload(pendingId), api::UserFacingError);
    CHECK(g.server->uploads.at(pendingId).parts.empty());
}
