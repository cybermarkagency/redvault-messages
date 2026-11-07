// TEMPORARY test probe (spliced into app/src/main.cpp for one run, then removed).
// Drives the real desktop app classes (TorController, AppController,
// ChatSession with its Tor outbox, FilesSession's resumable uploads) the way QML does, against the scratch API
// behind a test onion service, and writes one JSON line per step to
// $RV_PROBE_OUT. Peer "bob" uses the core repositories over the same gateway.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUrl>
#include <curl/curl.h>
#include <sodium.h>

#include <fstream>
#include <random>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/file_repository.hpp"
#include "redvault/data/friend_repository.hpp"
#include "redvault/net/cookie_jar.hpp"

namespace torprobe {

using namespace redvault;
namespace fs = std::filesystem;

static QFile* out = nullptr;
static int failures = 0;

static void line(const QString& step, bool ok, const QJsonObject& detail = {}) {
    QJsonObject o{{"step", step}, {"ok", ok}, {"detail", detail}};
    out->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
    out->flush();
    if (!ok) ++failures;
}

static bool waitFor(const std::function<bool()>& condition, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    return true;
}

static std::size_t collect(char* d, std::size_t s, std::size_t n, void* o) {
    static_cast<std::string*>(o)->append(d, s * n);
    return s * n;
}

static std::string control(const std::string& url, const std::string& body = {}) {
    CURL* h = curl_easy_init();
    std::string result;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &collect);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &result);
    if (!body.empty()) curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_perform(h);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return result;
}

static int lastLogBytes = 0;
static QJsonObject serverLog(const std::string& controlBase) {
    const std::string raw = control(controlBase + "/log");
    lastLogBytes = static_cast<int>(raw.size());
    return QJsonDocument::fromJson(QByteArray::fromStdString(raw)).object();
}

static std::string sha256(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    char buf[65536];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) crypto_hash_sha256_update(&st, reinterpret_cast<unsigned char*>(buf), in.gcount());
    unsigned char d[32];
    crypto_hash_sha256_final(&st, d);
    char hex[65];
    sodium_bin2hex(hex, sizeof hex, d, 32);
    return hex;
}

static fs::path makeFile(const fs::path& dir, const std::string& name, std::size_t size) {
    fs::create_directories(dir);
    std::string bytes(size, '\0');
    randombytes_buf(bytes.data(), bytes.size());
    std::ofstream(dir / name, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return dir / name;
}

/// Everything bob sees in his chat with alice, decrypted.
static std::vector<data::Message> bobView(const std::shared_ptr<api::ApiClient>& bobApi, const data::SelfIdentity& bob, const std::string& aliceId) {
    data::ConversationRepository repo(bobApi);
    for (const auto& dto : repo.listConversations()) {
        auto meta = repo.hydrateMeta(dto, bob);
        if (meta && !meta->isGroup && meta->otherUserId == aliceId) return repo.fetchMessages(*meta, bob.userId, 200).messages;
    }
    return {};
}

static int count(const std::vector<data::Message>& messages, const std::string& text) {
    int n = 0;
    for (const auto& m : messages) n += m.text == text ? 1 : 0;
    return n;
}

/// Part uploads the server accepted (resumable Files uploads), all or for one part number.
static int partPuts(const QJsonObject& log, int partNumber = 0) {
    int n = 0;
    for (const auto& e : log.value("log").toArray()) {
        const auto o = e.toObject();
        const QString path = o.value("path").toString();
        if (o.value("method").toString() != "PUT" || !path.contains("/parts/") || o.value("status").toInt() != 200) continue;
        if (partNumber == 0 || path.endsWith(QStringLiteral("/parts/") + QString::number(partNumber))) ++n;
    }
    return n;
}

/// The Files tab's row for an upload in progress (empty when there's none).
static QVariantMap uploadRow(app::FilesSession* files, const QString& name, const QString& kind = QStringLiteral("upload")) {
    if (files == nullptr) return {};
    for (const auto& r : files->rows()) {
        const auto m = r.toMap();
        if (m.value("name").toString() == name && m.value("kind").toString() == kind) return m;
    }
    return {};
}

static bool onlyOnion(const QJsonObject& log, int* onion, int* direct) {
    *onion = 0;
    *direct = 0;
    for (const auto& e : log.value("log").toArray()) (e.toObject().value("listener").toString() == "onion" ? *onion : *direct)++;
    return *direct == 0;
}

int run(const std::string& apiBase) {
    out = new QFile(qEnvironmentVariable("RV_PROBE_OUT"));
    out->open(QIODevice::WriteOnly | QIODevice::Truncate);
    sodium_init();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const std::string controlBase = qEnvironmentVariable("RV_PROBE_CONTROL", "http://127.0.0.1:18082").toStdString();
    const auto server = net::parseUrl(apiBase);
    const QString host = QString::fromStdString(server->host);
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    std::random_device rd;
    const std::string suffix = std::to_string(100000 + rd() % 900000);
    const QString aliceHandle = QStringLiteral("deskalice") + QString::fromStdString(suffix);
    const std::string bobHandle = "deskbob" + suffix;
    const QString password = QStringLiteral("correct horse battery staple ") + QString::fromStdString(suffix);
    const fs::path files = fs::path(dataDir.toStdWString()) / "probe-files";
    control(controlBase + "/log/clear", "{}");

    // ---- Phase A: Tor up, real app sends ----
    auto* tor = new app::TorController(host, dataDir + "/tor");
    auto* app = new app::AppController(apiBase, tor);
    QStringList transitions;
    QObject::connect(tor, &app::TorController::changed, [&] { transitions << tor->state() + " " + QString::number(tor->progress()); });
    line("tor_connects", waitFor([&] { return tor->connected(); }, 240000), {{"transitions", QJsonArray::fromStringList(transitions)}});

    net::CurlTransport::Options bobOptions;
    bobOptions.allowedHost = host.toStdString();
    bobOptions.torGate = tor->gate();
    bobOptions.connectTimeout = std::chrono::seconds(90);
    bobOptions.stallTimeout = std::chrono::seconds(60);
    auto bobCookies = std::make_shared<net::SessionCookieJar>();
    auto bobApi = std::make_shared<api::ApiClient>(apiBase, std::make_shared<net::CurlTransport>(bobOptions), bobCookies);
    std::optional<data::SelfIdentity> bob;
    try {
        bob.emplace(std::move(data::AuthRepository(bobApi).registerAccount(bobHandle, password.toStdString(), {true, true, true}).identity));
        line("peer_registered", true);
    } catch (const std::exception& e) {
        line("peer_registered", false, {{"error", e.what()}});
        return 1;
    }

    app->registerAccount(aliceHandle, password, password, true, true, true);
    const bool shown = waitFor([&] { return app->state() == app::AppController::State::ShowRecoveryKey || !app->error().isEmpty(); }, 180000);
    line("app_register_shows_recovery_key", shown && app->state() == app::AppController::State::ShowRecoveryKey, {{"error", app->error()}});
    app->acknowledgeRecoveryKey();
    line("app_signed_in", waitFor([&] { return app->state() == app::AppController::State::SignedIn; }, 60000));
    const std::string aliceId = app->userId().toStdString();
    control(controlBase + "/tier", QJsonDocument(QJsonObject{{"userId", app->userId()}, {"tier", "pro"}}).toJson().toStdString());
    control(controlBase + "/tier", QJsonDocument(QJsonObject{{"userId", QString::fromStdString(bob->userId)}, {"tier", "pro"}}).toJson().toStdString());

    data::FriendRepository(bobApi).sendRequest(aliceHandle.toStdString());
    app->people()->refresh();
    const bool incoming = waitFor([&] { return !app->people()->incoming().isEmpty(); }, 120000);
    if (incoming) app->people()->accept(app->people()->incoming().first().toMap().value("id").toString());
    const bool chat = waitFor([&] { app->chats()->refreshNow(); return app->chats()->chatList()->rowCount() > 0; }, 180000);
    line("friend_and_chat", incoming && chat);
    if (!chat) return 1;
    const std::string chatId = app->chats()->chatList()->chats().front().meta->id;
    app->chats()->openChat(QString::fromStdString(chatId));

    const std::string textA = "Hello from the desktop app over Tor " + suffix;
    app->chats()->sendMessage(QString::fromStdString(textA));
    const bool queuedFirst = app->chats()->queuedCount() == 1;
    const bool drained = waitFor([&] { return app->chats()->queuedCount() == 0; }, 120000);
    std::vector<data::Message> seen;
    waitFor([&] { seen = bobView(bobApi, *bob, aliceId); return count(seen, textA) == 1; }, 60000);
    line("text_via_outbox_and_tor", queuedFirst && drained && count(seen, textA) == 1,
         {{"queuedBeforeSend", queuedFirst}, {"drained", drained}, {"bobSees", count(seen, textA)}});

    const fs::path photo = makeFile(files, "desk-photo.jpg", 300 * 1024);
    app->chats()->attachFile(QUrl::fromLocalFile(QString::fromStdWString(photo.wstring())));
    const bool fileDrained = waitFor([&] { return app->chats()->fileActivity().isEmpty() && app->chats()->queuedCount() == 0; }, 240000);
    bool fileOk = false;
    waitFor([&] {
        for (const auto& m : bobView(bobApi, *bob, aliceId)) {
            if (m.kind == data::MessageKind::File && m.file && m.text == "desk-photo.jpg") {
                data::ConversationRepository repo(bobApi);
                auto convs = std::make_shared<data::ConversationRepository>(bobApi);
                data::FileRepository fr(bobApi, convs, files / "bob-cache");
                fileOk = sha256(fr.download(*m.file)) == sha256(photo);
                return true;
            }
        }
        return false;
    }, 120000);
    line("photo_via_outbox_and_tor", fileDrained && fileOk);

    int onion = 0, direct = 0;
    const bool aOnly = onlyOnion(serverLog(controlBase), &onion, &direct);
    line("phase_a_server_saw_only_onion", aOnly && onion > 0, {{"onion", onion}, {"direct", direct}, {"logBytes", lastLogBytes}});

    // ---- Files tab: a resumable upload, cut off by Tor stopping and finished after the restart ----
    const QString backupName = QStringLiteral("desk-backup.bin");
    const fs::path backupFile = makeFile(files, backupName.toStdString(), 20 * 1024 * 1024);
    const std::string backupHash = sha256(backupFile);
    const bool filesReady = waitFor([&] { return app->files() != nullptr && app->files()->loaded(); }, 120000);
    if (filesReady) app->files()->upload({QUrl::fromLocalFile(QString::fromStdWString(backupFile.wstring()))});
    const bool firstPart = filesReady && waitFor([&] { return partPuts(serverLog(controlBase)) >= 1; }, 300000);
    line("files_upload_goes_in_parts", firstPart, {{"partsReceived", partPuts(serverLog(controlBase))}});

    // ---- Phase B: Tor stops; nothing may go out, everything waits sealed ----
    tor->stop();
    line("tor_stopped_reports_unavailable", tor->state() == "unavailable");
    const bool uploadPaused = waitFor([&] { return uploadRow(app->files(), backupName).value("status").toString() == "paused"; }, 120000);
    const int partsAtStop = partPuts(serverLog(controlBase));
    line("files_upload_pauses_when_tor_stops", uploadPaused && partsAtStop >= 1 && partsAtStop < 3,
         {{"status", uploadRow(app->files(), backupName).value("status").toString()},
          {"error", uploadRow(app->files(), backupName).value("error").toString()},
          {"partsReceived", partsAtStop}});
    const int before = serverLog(controlBase).value("log").toArray().size();
    const std::string textB = "Written while Tor was down " + suffix;
    app->chats()->sendMessage(QString::fromStdString(textB));
    const fs::path doc = makeFile(files, "desk-report.pdf", 200 * 1024);
    app->chats()->attachFile(QUrl::fromLocalFile(QString::fromStdWString(doc.wstring())));
    waitFor([&] { return app->chats()->fileActivity().isEmpty(); }, 30000);
    waitFor([] { return false; }, 5000);  // give any (wrong) send a chance to show up
    const int after = serverLog(controlBase).value("log").toArray().size();
    bool shownQueued = false;
    for (const auto& m : app->chats()->messages()->all()) shownQueued = shownQueued || (m.queued && m.text == textB);
    line("tor_down_queues_and_sends_nothing",
         app->chats()->queuedCount() == 2 && after == before && shownQueued && tor->pausedUploads() == 1,
         {{"queued", app->chats()->queuedCount()}, {"serverRequestsWhileDown", after - before}, {"shownAsQueued", shownQueued},
          {"pausedUploads", tor->pausedUploads()}});

    // The outbox on disk holds nothing readable.
    std::string disk;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(dataDir.toStdWString()) / "outbox")) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        disk.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::ifstream docIn(doc, std::ios::binary);
    std::string docBytes((std::istreambuf_iterator<char>(docIn)), std::istreambuf_iterator<char>());
    line("outbox_sealed_on_disk", disk.find(textB) == std::string::npos && disk.find(docBytes.substr(1000, 64)) == std::string::npos,
         {{"bytesOnDisk", static_cast<int>(disk.size())}});

    // Quit (the app's own shutdown, with Tor down: the sign-out can't go anywhere either).
    delete app;
    delete tor;
    line("phase_b_no_direct_traffic", onlyOnion(serverLog(controlBase), &onion, &direct), {{"onion", onion}, {"direct", direct}});

    // ---- Phase C: restart; the queue survived and drains exactly once ----
    auto* tor2 = new app::TorController(host, dataDir + "/tor");
    auto* app2 = new app::AppController(apiBase, tor2);
    QStringList transitions2;
    QObject::connect(tor2, &app::TorController::changed, [&] { transitions2 << tor2->state() + " " + QString::number(tor2->progress()) + " " + tor2->detail().left(60); });
    const bool tor2Up = waitFor([&] { return tor2->connected(); }, 240000);
    line("restart_tor_connects", tor2Up, {{"state", tor2->state()}, {"detail", tor2->detail()}, {"transitions", QJsonArray::fromStringList(transitions2)}});
    // Bob keeps his session, now through the new gateway.
    bobOptions.torGate = tor2->gate();
    bobApi = std::make_shared<api::ApiClient>(apiBase, std::make_shared<net::CurlTransport>(bobOptions), bobCookies);
    app2->signIn(aliceHandle, password);
    const bool signedIn = waitFor([&] { return app2->state() == app::AppController::State::SignedIn || !app2->error().isEmpty(); }, 240000);
    line("restart_sign_in", signedIn && app2->state() == app::AppController::State::SignedIn,
         {{"error", app2->error()}, {"busy", app2->busy()}, {"state", static_cast<int>(app2->state())}, {"tor", tor2->state()}});
    const QVariantMap restored = uploadRow(app2->files(), backupName);
    line("files_upload_restored_from_journal", !restored.isEmpty(), {{"status", restored.value("status").toString()}});
    line("queue_survived_restart", waitFor([&] { return app2->chats() && app2->chats()->queuedCount() == 2; }, 10000),
         {{"queued", app2->chats() ? app2->chats()->queuedCount() : -1}});
    const bool drained2 = waitFor([&] { app2->chats()->refreshNow(); return app2->chats()->queuedCount() == 0; }, 300000);
    std::vector<data::Message> final;
    int docCount = 0;
    waitFor([&] {
        try {
            final = bobView(bobApi, *bob, aliceId);
        } catch (const std::exception&) {
            return false;
        }
        docCount = 0;
        for (const auto& m : final) docCount += m.text == "desk-report.pdf" ? 1 : 0;
        return count(final, textB) == 1 && docCount == 1;
    }, 120000);
    line("queue_drained_once_after_restart", drained2 && count(final, textB) == 1 && docCount == 1,
         {{"drained", drained2}, {"textCopies", count(final, textB)}, {"fileCopies", docCount}});
    const bool uploaded = waitFor([&] { return !uploadRow(app2->files(), backupName, QStringLiteral("file")).isEmpty(); }, 600000);
    const QJsonObject afterUpload = serverLog(controlBase);
    line("files_upload_resumed_without_resending", uploaded && partPuts(afterUpload) == 3 && partPuts(afterUpload, 1) == 1,
         {{"partsAccepted", partPuts(afterUpload)}, {"part1Accepted", partPuts(afterUpload, 1)},
          {"pending", uploadRow(app2->files(), backupName).value("status").toString()}});
    bool downloadedOk = false;
    if (uploaded) {
        const fs::path saved = files / "desk-backup-restored.bin";
        app2->files()->download(uploadRow(app2->files(), backupName, QStringLiteral("file")).value("id").toString(),
                                QUrl::fromLocalFile(QString::fromStdWString(saved.wstring())));
        waitFor([&] { return app2->files()->notice().startsWith("Saved to") || !app2->files()->error().isEmpty(); }, 600000);
        std::error_code ec;
        downloadedOk = fs::exists(saved, ec) && sha256(saved) == backupHash;
    }
    line("files_download_decrypts_to_original", downloadedOk, {{"error", app2->files()->error()}});
    line("all_server_traffic_via_onion", onlyOnion(serverLog(controlBase), &onion, &direct), {{"onion", onion}, {"direct", direct}});

    delete app2;
    delete tor2;
    out->write(QByteArray("{\"summary\":\"") + (failures == 0 ? "all steps passed" : QByteArray::number(failures) + " step(s) failed") + "\"}\n");
    out->close();
    return failures == 0 ? 0 : 1;
}

}  // namespace torprobe
