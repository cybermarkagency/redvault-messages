#include "vault_session.hpp"

#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeData>
#include <QUrl>

#include <algorithm>
#include <set>

#include "async.hpp"
#include "chat_session.hpp"
#include "prefs.hpp"
#include "redvault/api/drive_api.hpp"
#include "redvault/crypto/vault_crypto.hpp"
#include "redvault/data/file_key_repository.hpp"
#include "windows_hello.hpp"

namespace redvault::app {

namespace vc = crypto::vault;
using Json = nlohmann::json;

namespace {

constexpr int kIdleLockMs = 5 * 60 * 1000;
// Breach lookups per unlock at most, like the web app; and the two "not known yet" states.
constexpr std::size_t kMaxBreachLookups = 300;
constexpr std::int64_t kBreachChecking = -2;
constexpr std::int64_t kBreachFailed = -1;
constexpr int kClipboardClearMs = 30 * 1000;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

std::string str(const Json& data, const char* key) {
    const auto it = data.find(key);
    return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string nested(const Json& data, const char* object, const char* key) {
    const auto it = data.find(object);
    return it != data.end() && it->is_object() ? str(*it, key) : std::string();
}

std::string firstUrl(const Json& data) {
    const auto it = data.find("urls");
    if (it == data.end() || !it->is_array() || it->empty() || !(*it)[0].is_string()) return {};
    return (*it)[0].get<std::string>();
}

QString hostOf(const std::string& url) {
    if (url.empty()) return {};
    QUrl parsed = QUrl::fromUserInput(qs(url));
    return parsed.host().isEmpty() ? qs(url) : parsed.host();
}

/// The password an item's security check is about (logins and Wi-Fi).
std::string secretOf(const Json& data) {
    const std::string type = str(data, "type");
    if (type == "wifi") return nested(data, "wifi", "password");
    return str(data, "password");
}

QVariantMap strengthMap(const std::string& password) {
    if (password.empty()) return {};
    const auto estimate = vc::estimateStrength(password);
    const double fraction = estimate.strength == vc::Strength::Strong ? 1.0 : estimate.strength == vc::Strength::Fair ? 0.6 : 0.28;
    const QString tone = estimate.strength == vc::Strength::Strong ? QStringLiteral("good")
                         : estimate.strength == vc::Strength::Fair ? QStringLiteral("warn")
                                                                   : QStringLiteral("bad");
    return {{"label", qs(estimate.label)}, {"fraction", fraction}, {"tone", tone}, {"length", static_cast<int>(QString::fromStdString(password).size())}};
}

QString dayText(std::int64_t ms) {
    if (ms <= 0) return QStringLiteral("—");
    const QDate day = QDateTime::fromMSecsSinceEpoch(ms).date();
    if (day.year() == QDate::currentDate().year()) return QLocale().toString(day, QStringLiteral("MMM d"));
    return QString::number(day.year());
}

}  // namespace

VaultSession::VaultSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                           QObject* parent)
    : QObject(parent),
      api_(std::move(api)),
      identity_(std::move(identity)),
      repository_(std::make_shared<data::VaultRepository>(api_, identity_, prefs::vaultRevStore(), prefs::contactKeyPinStore())),
      chats_(chats) {
    pool_.setMaxThreadCount(2);
    hello_ = new WindowsHello(this);
    hello_->checkAvailability([this](bool available) {
        helloAvailable_ = available;
        emit helloAvailableChanged();
    });

    idleTimer_.setSingleShot(true);
    idleTimer_.setInterval(kIdleLockMs);
    connect(&idleTimer_, &QTimer::timeout, this, &VaultSession::lockNow);
    idleTimer_.start();
    qApp->installEventFilter(this);

    totpTimer_.setInterval(1000);
    connect(&totpTimer_, &QTimer::timeout, this, &VaultSession::updateTotp);

    clipboardTimer_.setSingleShot(true);
    clipboardTimer_.setInterval(kClipboardClearMs);
    connect(&clipboardTimer_, &QTimer::timeout, this, [this] {
        // Only if it's still what we put there: never wipe something the user copied since.
        if (QGuiApplication::clipboard()->text() == clipboardText_) QGuiApplication::clipboard()->clear();
        clipboardText_.clear();
    });

    regenerate();
    load();
}

VaultSession::~VaultSession() {
    qApp->removeEventFilter(this);
    if (!clipboardText_.isEmpty() && QGuiApplication::clipboard()->text() == clipboardText_) QGuiApplication::clipboard()->clear();
    pool_.clear();
    pool_.waitForDone();
}

bool VaultSession::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::KeyPress:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
            if (!locked_) idleTimer_.start();
            break;
        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}

void VaultSession::touch() {
    if (!locked_) idleTimer_.start();
}

// --- Lock ---

void VaultSession::lockNow() {
    if (locked_) return;
    // Nothing readable stays in memory: items and the one-time code.
    rows_.clear();
    breaches_.clear();
    ++breachGeneration_;
    selectedId_.clear();
    totpCode_.clear();
    totpTimer_.stop();
    idleTimer_.stop();
    loaded_ = false;
    locked_ = true;
    emit lockedChanged();
    emit itemsChanged();
    emit selectedChanged();
    emit totpChanged();
}

void VaultSession::unlockWithHello(QWindow* window) {
    if (!locked_) return;
    setMessages({}, {});
    hello_->verify(window, QStringLiteral("Unlock your password vault"), [this](bool verified, const QString& error) {
        if (verified) unlocked();
        else if (!error.isEmpty()) setMessages(error, {});
    });
}

void VaultSession::unlockWithPassword(const QString& password) {
    if (!locked_ || busy_) return;
    if (password.isEmpty()) return setMessages(QStringLiteral("Enter your password."), {});
    setBusy(true);
    setMessages({}, {});
    auto identity = identity_;
    const std::string pw = password.toStdString();
    runInBackground(
        pool_, this, [identity, pw] { return identity->passwordMatches(pw); },
        [this](bool matches) {
            setBusy(false);
            if (matches) unlocked();
            else setMessages(QStringLiteral("That password isn't right."), {});
        },
        [this](const Failure&) {
            setBusy(false);
            setMessages(QStringLiteral("That password isn't right."), {});
        });
}

void VaultSession::unlocked() {
    locked_ = false;
    idleTimer_.start();
    emit lockedChanged();
    load();
}

// --- Loading ---

void VaultSession::refresh() {
    if (!locked_) load();
}

void VaultSession::load() {
    auto repository = repository_;
    runInBackground(
        pool_, this, [repository] { return repository->list(); },
        [this](std::vector<data::VaultEntry> entries) {
            if (locked_) return;  // locked while loading: don't keep anything
            refused_ = repository_->refused();
            rows_.clear();
            for (auto& entry : entries) {
                // File keys live in the vault too, but belong to Files, not to this page.
                if (data::isFileKeyItem(entry.data)) continue;
                rows_.push_back(Row{std::move(entry), {}});
            }
            loaded_ = true;
            recompute();
            checkBreaches();
            if (selectedId_.isEmpty() || !find(selectedId_)) {
                const auto first = items();
                selectedId_ = first.isEmpty() ? QString() : first.front().toMap().value("id").toString();
            }
            emit selectedChanged();
            updateTotp();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) {
                emit sessionExpired();
                return;
            }
            setMessages(failure.message, {});
        });
}

void VaultSession::recompute() {
    // Reused: the same password on more than one of your items.
    std::map<std::string, int> uses;
    for (const auto& row : rows_) {
        if (row.entry.incoming) continue;
        const auto secret = secretOf(row.entry.data);
        if (!secret.empty()) ++uses[secret];
    }
    for (auto& row : rows_) {
        const auto secret = secretOf(row.entry.data);
        row.status.clear();
        if (secret.empty() || row.entry.incoming) continue;
        // Exposed in a breach is the most urgent, so it wins over reused and weak.
        if (const auto it = breaches_.find(vc::breachHash(secret)); it != breaches_.end() && it->second > 0) {
            row.status = QStringLiteral("exposed");
        } else if (uses[secret] > 1) row.status = QStringLiteral("reused");
        else if (vc::estimateStrength(secret).strength == vc::Strength::Weak) row.status = QStringLiteral("weak");
    }
    emit itemsChanged();
    emit selectedChanged();
}

void VaultSession::checkBreaches() {
    if (breachCheckOff_) return;
    // Hashes are made here; only their first 5 characters go out, grouped so one lookup answers them all.
    std::map<std::string, std::vector<std::string>> byPrefix;
    std::size_t queued = 0;
    for (const auto& row : rows_) {
        const auto secret = secretOf(row.entry.data);
        if (secret.empty()) continue;
        const std::string hash = vc::breachHash(secret);
        if (breaches_.contains(hash) || queued >= kMaxBreachLookups) continue;
        breaches_[hash] = kBreachChecking;
        byPrefix[hash.substr(0, 5)].push_back(hash);
        ++queued;
    }
    if (byPrefix.empty()) return;
    emit selectedChanged();
    auto api = api_;
    const int generation = breachGeneration_;
    struct Outcome {
        std::map<std::string, std::int64_t> counts;
        bool off = false;
    };
    runInBackground(
        pool_, this,
        [api, byPrefix] {
            Outcome outcome;
            for (const auto& [prefix, hashes] : byPrefix) {
                try {
                    const std::string range = api::vault::breachRange(*api, prefix);
                    for (const auto& hash : hashes) outcome.counts[hash] = vc::breachCount(hash, range);
                } catch (const api::ApiError& e) {
                    if (e.status() == 401) throw;
                    for (const auto& hash : hashes) outcome.counts[hash] = kBreachFailed;
                    if (e.code() == "breach_check_disabled") {
                        outcome.off = true;
                        break;
                    }
                }
            }
            return outcome;
        },
        [this, generation](Outcome outcome) {
            if (generation != breachGeneration_ || locked_) return;  // locked meanwhile: keep nothing
            for (const auto& [hash, count] : outcome.counts) breaches_[hash] = count;
            if (outcome.off) {
                breachCheckOff_ = true;
                for (auto& [hash, count] : breaches_) {
                    if (count == kBreachChecking) count = kBreachFailed;
                }
            }
            recompute();
        },
        [this](const Failure& failure) {
            if (failure.status == 401) emit sessionExpired();
        });
}

QString VaultSession::breachText(const std::string& secret) const {
    if (secret.empty()) return QStringLiteral("No password");
    if (breachCheckOff_) return QStringLiteral("Off on this server");
    const auto it = breaches_.find(vc::breachHash(secret));
    if (it == breaches_.end() || it->second == kBreachChecking) return QStringLiteral("Checking privately…");
    if (it->second == kBreachFailed) return QStringLiteral("Couldn't check");
    if (it->second > 0) {
        // Have I Been Pwned counts sightings across breach dumps, not breaches.
        return it->second == 1 ? QStringLiteral("Seen once in known breaches")
                               : QStringLiteral("Seen %1 times in known breaches").arg(QLocale().toString(qlonglong(it->second)));
    }
    return QStringLiteral("Checked privately: not found in known breaches");
}

// --- Views ---

const VaultSession::Row* VaultSession::find(const QString& id) const {
    const std::string key = id.toStdString();
    for (const auto& row : rows_) {
        if (row.entry.id == key) return &row;
    }
    return nullptr;
}

QString VaultSession::handleFor(const std::string& userId) const {
    if (chats_) {
        for (const auto& c : chats_->contacts()) {
            const auto m = c.toMap();
            if (m.value("userId").toString() == qs(userId)) return m.value("handle").toString();
        }
    }
    return QStringLiteral("Someone");
}

void VaultSession::setCategory(const QString& category) {
    if (category_ == category) return;
    category_ = category;
    emit itemsChanged();
}

void VaultSession::setSearch(const QString& search) {
    if (search_ == search) return;
    search_ = search;
    emit itemsChanged();
}

QVariantList VaultSession::items() const {
    QVariantList out;
    const QString q = search_.trimmed().toLower();
    std::vector<const Row*> sorted;
    for (const auto& row : rows_) sorted.push_back(&row);
    std::sort(sorted.begin(), sorted.end(), [](const Row* a, const Row* b) {
        return QString::fromStdString(str(a->entry.data, "title")).compare(QString::fromStdString(str(b->entry.data, "title")),
                                                                           Qt::CaseInsensitive) < 0;
    });
    for (const Row* row : sorted) {
        const Json& d = row->entry.data;
        const std::string type = str(d, "type");
        const bool hasPasskey = !nested(d, "passkey", "rpId").empty();
        const bool matchesCategory = category_ == "all" || (category_ == "shared" && row->entry.incoming) ||
                                     (!row->entry.incoming && category_ == qs(type)) ||
                                     (!row->entry.incoming && category_ == "passkey" && hasPasskey) ||
                                     (category_ == "exposed" || category_ == "reused" || category_ == "weak" ? row->status == category_ : false);
        if (!matchesCategory) continue;
        const QString title = qs(str(d, "title")).isEmpty() ? QStringLiteral("Untitled") : qs(str(d, "title"));
        QString subtitle;
        if (type == "card") {
            const std::string number = nested(d, "card", "number");
            const std::string last4 = number.size() >= 4 ? number.substr(number.size() - 4) : number;
            subtitle = QStringLiteral("Expires %1 · Card ending %2").arg(qs(nested(d, "card", "expiry")), qs(last4));
        } else if (type == "wifi") {
            subtitle = QStringLiteral("%1 · Network: %2").arg(qs(nested(d, "wifi", "security")).isEmpty() ? QStringLiteral("Wi-Fi") : qs(nested(d, "wifi", "security")),
                                                              qs(nested(d, "wifi", "ssid")));
        } else if (type == "note") {
            subtitle = qs(str(d, "notes")).section(u'\n', 0, 0).left(60);
        } else {
            subtitle = qs(str(d, "username"));
            const QString host = hostOf(firstUrl(d));
            if (!host.isEmpty()) subtitle += (subtitle.isEmpty() ? QString() : QStringLiteral(" · ")) + host;
        }
        if (!q.isEmpty() && !title.toLower().contains(q) && !subtitle.toLower().contains(q)) continue;
        out.append(QVariantMap{
            {"id", qs(row->entry.id)},
            {"title", title},
            {"subtitle", subtitle},
            {"initial", title.left(1).toUpper()},
            {"type", qs(type)},
            {"status", row->status},
            {"incoming", row->entry.incoming},
        });
    }
    return out;
}

QVariantMap VaultSession::counts() const {
    int all = 0, login = 0, passkey = 0, card = 0, note = 0, wifi = 0, shared = 0, exposed = 0, reused = 0, weak = 0;
    for (const auto& row : rows_) {
        ++all;
        if (row.entry.incoming) {
            ++shared;
            continue;
        }
        const std::string type = str(row.entry.data, "type");
        // A login with a passkey saved counts under Passkeys too.
        if (type == "passkey" || !nested(row.entry.data, "passkey", "rpId").empty()) ++passkey;
        if (type == "login") ++login;
        else if (type == "card") ++card;
        else if (type == "note") ++note;
        else if (type == "wifi") ++wifi;
        if (row.status == "exposed") ++exposed;
        else if (row.status == "reused") ++reused;
        else if (row.status == "weak") ++weak;
    }
    return {{"all", all},   {"login", login},   {"passkey", passkey}, {"card", card}, {"note", note},
            {"wifi", wifi}, {"shared", shared}, {"exposed", exposed}, {"reused", reused}, {"weak", weak}};
}

QVariantMap VaultSession::selected() const {
    const Row* row = find(selectedId_);
    if (!row) return {};
    const Json& d = row->entry.data;
    const std::string type = str(d, "type");
    const std::string secret = secretOf(d);
    QVariantList sharedWith;
    for (const auto& userId : row->entry.sharedWith) sharedWith.append(QVariantMap{{"userId", qs(userId)}, {"handle", handleFor(userId)}});
    int reusedWith = 0;
    if (row->status == "reused") {
        for (const auto& other : rows_) {
            if (&other != row && !other.entry.incoming && secretOf(other.entry.data) == secret) ++reusedWith;
        }
    }
    const std::string url = firstUrl(d);
    const auto passwordChanged = d.value("passwordChanged", d.value("updated", std::int64_t{0}));
    return {
        {"id", qs(row->entry.id)},
        {"type", qs(type)},
        {"title", qs(str(d, "title"))},
        {"host", hostOf(url)},
        {"website", qs(url)},
        {"username", qs(str(d, "username"))},
        {"hasPassword", !secret.empty()},
        {"passwordLength", static_cast<int>(QString::fromStdString(secret).size())},
        {"strength", strengthMap(secret)},
        {"status", row->status},
        {"breach", breachText(secret)},
        {"exposed", row->status == "exposed"},
        {"reusedWith", reusedWith},
        {"notes", qs(str(d, "notes"))},
        {"hasTotp", !str(d, "totp").empty()},
        {"passkeyRpId", qs(nested(d, "passkey", "rpId"))},
        {"passkeyUser", qs(nested(d, "passkey", "userName"))},
        {"cardHolder", qs(nested(d, "card", "holder"))},
        {"cardExpiry", qs(nested(d, "card", "expiry"))},
        {"cardLast4", qs([&] {
             const auto n = nested(d, "card", "number");
             return n.size() >= 4 ? n.substr(n.size() - 4) : n;
         }())},
        {"wifiSsid", qs(nested(d, "wifi", "ssid"))},
        {"wifiSecurity", qs(nested(d, "wifi", "security"))},
        {"sharedWith", sharedWith},
        {"incoming", row->entry.incoming},
        {"owner", row->entry.incoming ? handleFor(row->entry.ownerId) : QString()},
        {"lastChanged", dayText(passwordChanged)},
    };
}

void VaultSession::select(const QString& id) {
    if (selectedId_ == id) return;
    selectedId_ = id;
    if (!notice_.isEmpty()) setMessages(error_, {});
    emit selectedChanged();
    updateTotp();
}

void VaultSession::updateTotp() {
    const Row* row = find(selectedId_);
    const std::string secret = row ? str(row->entry.data, "totp") : std::string();
    const auto params = secret.empty() ? std::nullopt : vc::parseTotp(secret);
    if (!params) {
        totpTimer_.stop();
        if (!totpCode_.isEmpty()) {
            totpCode_.clear();
            emit totpChanged();
        }
        return;
    }
    const auto now = QDateTime::currentSecsSinceEpoch();
    const QString code = qs(vc::totpCode(*params, now));
    totpCode_ = code.size() == 6 ? code.left(3) + u' ' + code.mid(3) : code;
    totpSecondsLeft_ = vc::totpSecondsLeft(*params, now);
    totpPeriod_ = params->period;
    if (!totpTimer_.isActive()) totpTimer_.start();
    emit totpChanged();
}

// --- Editing ---

QVariantMap VaultSession::blankItem(const QString& type) const {
    QVariantMap out{{"type", type}, {"title", QString()}, {"username", QString()}, {"password", QString()}, {"url", QString()},
                    {"notes", QString()}, {"totp", QString()}};
    if (type == "login" || type == "wifi") out["password"] = generated_;
    return out;
}

QVariantMap VaultSession::editableItem(const QString& id) const {
    const Row* row = find(id);
    if (!row || row->entry.incoming) return {};
    const Json& d = row->entry.data;
    return {
        {"id", qs(row->entry.id)},
        {"type", qs(str(d, "type"))},
        {"title", qs(str(d, "title"))},
        {"username", qs(str(d, "username"))},
        {"password", qs(str(d, "password"))},
        {"url", qs(firstUrl(d))},
        {"notes", qs(str(d, "notes"))},
        {"totp", qs(str(d, "totp"))},
        {"cardHolder", qs(nested(d, "card", "holder"))},
        {"cardNumber", qs(nested(d, "card", "number"))},
        {"cardExpiry", qs(nested(d, "card", "expiry"))},
        {"cardCvv", qs(nested(d, "card", "cvv"))},
        {"wifiSsid", qs(nested(d, "wifi", "ssid"))},
        {"wifiSecurity", qs(nested(d, "wifi", "security"))},
        {"wifiPassword", qs(nested(d, "wifi", "password"))},
        {"passkeyRpId", qs(nested(d, "passkey", "rpId"))},
        {"passkeyUser", qs(nested(d, "passkey", "userName"))},
    };
}

void VaultSession::saveItem(const QVariantMap& fields) {
    if (locked_ || busy_) return;
    const QString type = fields.value("type").toString();
    if (fields.value("title").toString().trimmed().isEmpty()) return setMessages(QStringLiteral("Give it a name."), {});
    if (!fields.value("totp").toString().trimmed().isEmpty() && !vc::parseTotp(fields.value("totp").toString().toStdString())) {
        return setMessages(QStringLiteral("That one-time code setup key isn't valid. Paste the key or otpauth:// link from the site."), {});
    }

    data::VaultEntry entry;
    const QString id = fields.value("id").toString();
    if (const Row* existing = find(id)) entry = existing->entry;
    Json& d = entry.data;
    if (!d.is_object()) d = Json::object();
    const auto text = [&](const char* key) { return fields.value(key).toString().trimmed().toStdString(); };
    const std::string oldSecret = secretOf(d);
    d["type"] = type.toStdString();
    d["title"] = text("title");
    d["username"] = text("username");
    d["password"] = fields.value("password").toString().toStdString();
    d["notes"] = fields.value("notes").toString().toStdString();
    d["totp"] = text("totp");
    d["urls"] = text("url").empty() ? Json::array() : Json::array({text("url")});
    if (type == "card") {
        d["card"] = {{"holder", text("cardHolder")}, {"number", text("cardNumber")}, {"expiry", text("cardExpiry")}, {"cvv", text("cardCvv")}};
    }
    if (type == "wifi") {
        d["wifi"] = {{"ssid", text("wifiSsid")}, {"security", text("wifiSecurity")}, {"password", fields.value("wifiPassword").toString().toStdString()}};
    }
    // A passkey can be saved on its own or alongside a login's password.
    if (type == "passkey" || !text("passkeyRpId").empty()) {
        d["passkey"] = {{"rpId", text("passkeyRpId")}, {"userName", text("passkeyUser").empty() ? text("username") : text("passkeyUser")}};
    } else {
        d.erase("passkey");
    }
    if (secretOf(d) != oldSecret) d["passwordChanged"] = QDateTime::currentMSecsSinceEpoch();

    setBusy(true);
    setMessages({}, {});
    auto repository = repository_;
    runInBackground(
        pool_, this, [repository, entry] { return repository->save(entry); },
        [this](data::VaultEntry stored) {
            setBusy(false);
            if (locked_) return;
            bool replaced = false;
            for (auto& row : rows_) {
                if (row.entry.id == stored.id) {
                    row.entry = stored;
                    replaced = true;
                }
            }
            if (!replaced) rows_.push_back(Row{stored, {}});
            selectedId_ = qs(stored.id);
            recompute();
            checkBreaches();
            updateTotp();
            setMessages({}, QStringLiteral("Saved."));
            emit saved(qs(stored.id));
        },
        [this](const Failure& failure) {
            setBusy(false);
            if (failure.status == 401) return emit sessionExpired();
            setMessages(failure.message, {});
            refresh();
        });
}

void VaultSession::deleteItem(const QString& id) {
    const Row* row = find(id);
    if (!row || row->entry.incoming) return;
    auto repository = repository_;
    const auto entry = row->entry;
    runInBackground(
        pool_, this, [repository, entry] { repository->remove(entry); },
        [this, id] {
            rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&](const Row& r) { return qs(r.entry.id) == id; }), rows_.end());
            if (selectedId_ == id) selectedId_.clear();
            recompute();
            setMessages({}, QStringLiteral("Deleted."));
        },
        [this](const Failure& failure) { setMessages(failure.message, {}); });
}

QString VaultSession::revealPassword(const QString& id) const {
    const Row* row = find(id);
    return row ? qs(secretOf(row->entry.data)) : QString();
}

void VaultSession::copy(const QString& text, bool secret) {
    auto* mime = new QMimeData;
    mime->setText(text);
#ifdef _WIN32
    if (secret) {
        // Keep secrets out of Windows clipboard history and cloud clipboard sync.
        const QByteArray zero(4, '\0');
        mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""), zero);
        mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanIncludeInClipboardHistory\""), zero);
        mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanUploadToCloudClipboard\""), zero);
    }
#endif
    QGuiApplication::clipboard()->setMimeData(mime);
    if (secret) {
        clipboardText_ = text;
        clipboardTimer_.start();
    }
    setMessages({}, secret ? QStringLiteral("Copied. It clears from the clipboard in 30 seconds.") : QStringLiteral("Copied."));
}

void VaultSession::copyField(const QString& id, const QString& field) {
    const Row* row = find(id);
    if (!row) return;
    const Json& d = row->entry.data;
    if (field == "username") return copy(qs(str(d, "username")), false);
    if (field == "password") return copy(qs(secretOf(d)), true);
    if (field == "website") return copy(qs(firstUrl(d)), false);
    if (field == "cardNumber") return copy(qs(nested(d, "card", "number")), true);
    if (field == "cardCvv") return copy(qs(nested(d, "card", "cvv")), true);
    if (field == "totp") {
        if (const auto params = vc::parseTotp(str(d, "totp"))) copy(qs(vc::totpCode(*params, QDateTime::currentSecsSinceEpoch())), true);
    }
}

// --- Generator ---

void VaultSession::regenerate() {
    vc::GeneratorOptions options;
    options.length = genLength_;
    options.upper = genUpper_;
    options.digits = genDigits_;
    options.symbols = genSymbols_;
    options.words = genWords_;
    generated_ = qs(vc::generatePassword(options));
    emit generatorChanged();
}

void VaultSession::setGenLength(int length) {
    const int clamped = genWords_ ? std::clamp(length, 4, 12) : std::clamp(length, 8, 64);
    if (clamped == genLength_) return;
    genLength_ = clamped;
    regenerate();
}

void VaultSession::setGenUpper(bool on) {
    genUpper_ = on;
    regenerate();
}

void VaultSession::setGenDigits(bool on) {
    genDigits_ = on;
    regenerate();
}

void VaultSession::setGenSymbols(bool on) {
    genSymbols_ = on;
    regenerate();
}

void VaultSession::setGenWords(bool on) {
    if (genWords_ == on) return;
    genWords_ = on;
    genLength_ = on ? 6 : 24;
    regenerate();
}

QVariantMap VaultSession::generatedStrength() const {
    return strengthMap(generated_.toStdString());
}

QVariantMap VaultSession::strengthOf(const QString& password) const {
    return strengthMap(password.toStdString());
}

void VaultSession::useGenerated(const QString& id) {
    QVariantMap fields = editableItem(id);
    if (fields.isEmpty()) return;
    if (fields.value("type").toString() == "wifi") fields["wifiPassword"] = generated_;
    else fields["password"] = generated_;
    saveItem(fields);
    regenerate();
}

// --- Sharing ---

void VaultSession::share(const QString& id, const QString& userId) {
    const Row* row = find(id);
    if (!row || row->entry.incoming) return;
    auto repository = repository_;
    auto entry = row->entry;
    const std::string recipient = userId.toStdString();
    runInBackground(
        pool_, this, [repository, entry, recipient] { repository->share(entry, recipient); },
        [this, id, recipient, userId] {
            for (auto& row : rows_) {
                if (qs(row.entry.id) == id && std::find(row.entry.sharedWith.begin(), row.entry.sharedWith.end(), recipient) == row.entry.sharedWith.end()) {
                    row.entry.sharedWith.push_back(recipient);
                }
            }
            emit selectedChanged();
            setMessages({}, QStringLiteral("Shared with %1. Only they can open it.").arg(handleFor(recipient)));
        },
        [this](const Failure& failure) { setMessages(failure.message, {}); });
}

void VaultSession::unshare(const QString& id, const QString& userId) {
    const Row* row = find(id);
    if (!row) return;
    auto repository = repository_;
    auto entry = row->entry;
    const std::string recipient = userId.toStdString();
    runInBackground(
        pool_, this, [repository, entry, recipient] { repository->unshare(entry, recipient); },
        [this, id, recipient] {
            for (auto& row : rows_) {
                if (qs(row.entry.id) == id) {
                    auto& list = row.entry.sharedWith;
                    list.erase(std::remove(list.begin(), list.end(), recipient), list.end());
                }
            }
            emit selectedChanged();
        },
        [this](const Failure& failure) { setMessages(failure.message, {}); });
}

// --- Messages ---

void VaultSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void VaultSession::setMessages(const QString& error, const QString& notice) {
    error_ = error;
    notice_ = notice;
    emit messagesChanged();
}

void VaultSession::clearMessages() {
    setMessages({}, {});
}

}  // namespace redvault::app
