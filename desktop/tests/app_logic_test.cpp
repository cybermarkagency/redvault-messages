// The app's logic with no window: the real AppLock with real QSettings in a throwaway folder, and the real
// MiniChat.qml loaded against stand-ins for the app's types (tests/qml_stubs). Nothing here is a person clicking.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <memory>

#include "app_lock.hpp"
#include "app_test_support.hpp"
#include "local_store.hpp"
#include "call_notice.hpp"
#include "prefs.hpp"

using redvault::app::AppLock;
using redvault::test::Application;
using redvault::test::qt;
using redvault::test::waitFor;

namespace {

/// An AppLock signed in, with this PIN set and its settings otherwise empty.
struct Lock {
    Lock() {
        qt();
        QSettings().clear();
        QObject::connect(&lock, &AppLock::pinSet, [this] { set = true; });
        QObject::connect(&lock, &AppLock::lockedOut, [this](const QString& why) {
            ++signedOut;
            reason = why;
        });
        lock.setSignedIn(true);
        lock.setPin(QStringLiteral("1234"), QStringLiteral("1234"));
        REQUIRE(waitFor([this] { return set; }));
    }
    ~Lock() { QSettings().clear(); }

    /// Types a PIN and waits for the answer.
    void enter(const QString& pin) {
        lock.unlock(pin);
        REQUIRE(waitFor([this] { return !lock.busy(); }));
    }

    AppLock lock;
    bool set = false;
    int signedOut = 0;
    QString reason;
};

}  // namespace

TEST_CASE("control: the right PIN unlocks, a wrong one doesn't", "[app][lock]") {
    Lock t;
    CHECK(t.lock.enabled());
    t.lock.lockNow();
    REQUIRE(t.lock.locked());
    t.enter(QStringLiteral("9999"));
    CHECK(t.lock.locked());
    CHECK_FALSE(t.lock.error().isEmpty());
    t.enter(QStringLiteral("1234"));
    CHECK_FALSE(t.lock.locked());
    CHECK(t.signedOut == 0);
}

TEST_CASE("a locked app whose stored PIN is missing signs out instead of opening", "[app][lock]") {
    const QString key = GENERATE(QStringLiteral("appLock/pinHash"), QStringLiteral("appLock/pinSalt"));
    Lock t;
    t.lock.lockNow();
    REQUIRE(t.lock.locked());
    QSettings().remove(key);
    QSettings().sync();

    t.enter(QStringLiteral("0000"));
    CHECK(t.signedOut == 1);
    CHECK(t.reason.contains(QStringLiteral("Sign in")));
    // It never opened on its own: only the sign-out (setSignedIn(false)) lets go of it.
    CHECK(t.lock.locked());
    t.lock.setSignedIn(false);
    CHECK_FALSE(t.lock.locked());
}

TEST_CASE("the right PIN doesn't rescue a lock whose stored PIN has gone", "[app][lock]") {
    Lock t;
    t.lock.lockNow();
    QSettings().remove(QStringLiteral("appLock/pinHash"));
    t.enter(QStringLiteral("1234"));
    CHECK(t.signedOut == 1);
    CHECK(t.lock.locked());
}

TEST_CASE("a PIN swapped in the settings file while locked doesn't open it", "[app][lock]") {
    Lock t;
    // The attacker's own PIN, hashed the app's own way, by a second copy that writes the same settings.
    QSettings settings;
    const QVariant realHash = settings.value(QStringLiteral("appLock/pinHash"));
    const QVariant realSalt = settings.value(QStringLiteral("appLock/pinSalt"));
    {
        AppLock attacker;
        bool done = false;
        QObject::connect(&attacker, &AppLock::pinSet, [&] { done = true; });
        attacker.setPin(QStringLiteral("0000"), QStringLiteral("0000"));
        REQUIRE(waitFor([&] { return done; }));
    }
    REQUIRE(settings.value(QStringLiteral("appLock/pinHash")) != realHash);

    t.lock.lockNow();
    REQUIRE(t.lock.locked());
    t.enter(QStringLiteral("0000"));
    CHECK(t.lock.locked());
    CHECK(t.signedOut == 1);
}

TEST_CASE("deleting the PIN from the settings file doesn't stop the app locking", "[app][lock]") {
    Lock t;
    QSettings().remove(QStringLiteral("appLock/pinHash"));
    QSettings().remove(QStringLiteral("appLock/pinSalt"));
    CHECK(t.lock.enabled());
    t.lock.lockNow();
    CHECK(t.lock.locked());
}

TEST_CASE("clearing the attempts in the settings file doesn't reset the count", "[app][lock]") {
    Lock t;
    t.lock.lockNow();
    for (int i = 0; i < AppLock::kMaxAttempts - 1; ++i) {
        t.enter(QStringLiteral("9999"));
        REQUIRE(t.signedOut == 0);
        // Whatever an attacker writes between tries.
        QSettings().remove(QStringLiteral("appLock/failedAttempts"));
        QSettings().setValue(QStringLiteral("appLock/failedAttempts"), 0);
    }
    CHECK(t.lock.error().contains(QStringLiteral("1 try left")));
    t.enter(QStringLiteral("9999"));
    CHECK(t.signedOut == 1);
    CHECK(t.lock.locked());
    CHECK(t.reason.contains(QStringLiteral("Too many")));
    // And the PIN is gone.
    CHECK_FALSE(QSettings().contains(QStringLiteral("appLock/pinHash")));
}

TEST_CASE("a right PIN resets the count", "[app][lock]") {
    Lock t;
    t.lock.lockNow();
    for (int i = 0; i < 2; ++i) t.enter(QStringLiteral("9999"));
    t.enter(QStringLiteral("1234"));
    REQUIRE_FALSE(t.lock.locked());
    t.lock.lockNow();
    t.enter(QStringLiteral("9999"));
    CHECK(t.lock.error().contains(QStringLiteral("9 tries left")));
}

TEST_CASE("the PIN can't be turned off or replaced while locked", "[app][lock]") {
    Lock t;
    t.lock.lockNow();
    t.lock.disable();
    CHECK(t.lock.locked());
    CHECK(t.lock.enabled());
    CHECK(QSettings().contains(QStringLiteral("appLock/pinHash")));
    const QVariant before = QSettings().value(QStringLiteral("appLock/pinHash"));
    t.lock.setPin(QStringLiteral("0000"), QStringLiteral("0000"));
    QCoreApplication::processEvents();
    CHECK_FALSE(t.lock.busy());
    CHECK(QSettings().value(QStringLiteral("appLock/pinHash")) == before);
}

TEST_CASE("control: unlocked, the PIN can be turned off", "[app][lock]") {
    Lock t;
    t.lock.disable();
    CHECK_FALSE(t.lock.enabled());
    CHECK_FALSE(QSettings().contains(QStringLiteral("appLock/pinHash")));
    t.lock.lockNow();
    CHECK_FALSE(t.lock.locked());
}

TEST_CASE("signing in reads the PIN again", "[app][lock]") {
    Lock t;
    t.lock.setSignedIn(false);
    t.lock.disable();
    AppLock next;
    CHECK_FALSE(next.enabled());
}

// --- Call notifications ---

TEST_CASE("a call notification names nobody unless that's allowed", "[app][notify]") {
    qt();
    using redvault::app::callNotice;
    const auto hidden = callNotice(QStringLiteral("marcelo"), QStringLiteral("video"), false);
    CHECK_FALSE(hidden.text.contains(QStringLiteral("marcelo")));
    CHECK_FALSE(hidden.title.contains(QStringLiteral("marcelo")));
    CHECK(hidden.title == QStringLiteral("Video call"));

    const auto shown = callNotice(QStringLiteral("marcelo"), QStringLiteral("voice"), true);
    CHECK(shown.text == QStringLiteral("marcelo is calling"));
    CHECK(shown.title == QStringLiteral("Voice call"));
}

TEST_CASE("device-key notifications respect the notifications setting", "[app][notify]") {
    qt();
    using redvault::app::deviceKeyNotice;
    CHECK_FALSE(deviceKeyNotice(3, false));
    CHECK_FALSE(deviceKeyNotice(0, true));
    // Control, and it still says only how many.
    REQUIRE(deviceKeyNotice(1, true));
    CHECK(deviceKeyNotice(1, true)->text == QStringLiteral("1 new message"));
    CHECK(deviceKeyNotice(3, true)->text == QStringLiteral("3 new messages"));
}

// --- Pins in local settings ---

TEST_CASE("a signing key pin in settings has one spelling, and keeps working where it was stored", "[app][pins]") {
    qt();
    QSettings().clear();
    const std::string self = "abcdef11-1111-4111-8111-111111111111";
    const std::string them = "bcdefa22-2222-4222-8222-222222222222";
    const auto upper = [](const std::string& text) { return QString::fromStdString(text).toUpper().toStdString(); };
    CHECK(redvault::app::prefs::pinSigningKey(self, them, "genuine") == "genuine");
    // Capitals can't earn a second pin, and a later answer doesn't replace the first.
    CHECK(redvault::app::prefs::pinSigningKey(upper(self), upper(them), "attacker") == "genuine");
    CHECK(redvault::app::prefs::pinSigningKey(self, them, "attacker") == "genuine");
    // Pins from before this change live under the same name, so nobody goes back to first use.
    CHECK(QSettings().contains(QStringLiteral("prefs/") + QString::fromStdString(self) + QStringLiteral("/signingKeys/") + QString::fromStdString(them)));
    QSettings().clear();
}

TEST_CASE("every pin in settings has one spelling", "[app][pins]") {
    qt();
    QSettings().clear();
    namespace prefs = redvault::app::prefs;
    const std::string self = "abcdef11-1111-4111-8111-111111111111";
    const std::string them = "bcdefa22-2222-4222-8222-222222222222";
    const auto upper = [](const std::string& text) { return QString::fromStdString(text).toUpper().toStdString(); };
    const std::string first = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    const std::string other = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBA=";

    // The signing key lookup reads what the pin wrote, whichever way either is spelled.
    CHECK_FALSE(prefs::pinnedSigningKey(self, them));
    prefs::pinSigningKey(upper(self), upper(them), "genuine");
    CHECK(prefs::pinnedSigningKey(self, them) == std::optional<std::string>("genuine"));
    CHECK(prefs::pinnedSigningKey(upper(self), upper(them)) == std::optional<std::string>("genuine"));

    // Contact keys, through the store the core is given.
    const auto contacts = prefs::contactKeyPinStore();
    CHECK_FALSE(contacts->keyChanged(self, them, first));
    CHECK(contacts->keyChanged(upper(self), upper(them), other));
    CHECK_FALSE(contacts->keyChanged(upper(self), them, first));
    // No key to compare isn't a change.
    CHECK_FALSE(contacts->keyChanged(self, them, ""));
    // Accepting the new number is what lets sealing go on.
    prefs::acceptContactKey(self, upper(them), other);
    CHECK_FALSE(contacts->keyChanged(self, them, other));
    CHECK(contacts->keyChanged(self, them, first));

    // Vault revisions, per account.
    const auto revs = prefs::vaultRevStore();
    revs->record(self, "item", 5);
    CHECK(prefs::vaultRevStore()->seen(upper(self), "ITEM") == 5);
    CHECK(prefs::vaultRevStore()->seen(them, "item") == 0);
    CHECK(prefs::vaultRevStore()->seen(self, "shared:item") == 0);

    // A contact marked verified stays so for either spelling of this account.
    redvault::app::local_store::setVerified(self, first, true);
    CHECK(redvault::app::local_store::isVerified(upper(self), first));
    QSettings().clear();
}

TEST_CASE("a conversation key pin survives a new session in settings", "[app][pins]") {
    qt();
    QSettings().clear();
    const auto first = redvault::app::prefs::keyPinStore();
    first->save("self", "chat", redvault::data::KeyPin{2, 3, "abc=", {"self"}});
    const auto pin = redvault::app::prefs::keyPinStore()->load("SELF", "CHAT");
    REQUIRE(pin);
    CHECK(pin->seq == 3);
    QSettings().clear();
}

// --- The in-call chat panel, in its real QML ---

namespace {

struct Panel {
    Panel() {
        engine.addImportPath(QStringLiteral(REDVAULT_QML_STUBS));
        QQmlComponent stub(&engine);
        stub.setData("import Redvault\nChatSession {}", QUrl());
        INFO(stub.errorString().toStdString());
        session.reset(stub.create());
        REQUIRE(session);
        // The real file, against the stand-ins.
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(REDVAULT_QML_DIR "/MiniChat.qml")));
        INFO(component.errorString().toStdString());
        root.reset(component.createWithInitialProperties({{QStringLiteral("session"), QVariant::fromValue(session.get())}}));
        REQUIRE(root);
        input = root->findChild<QObject*>(QStringLiteral("miniChatInput"));
        REQUIRE(input);
        app = engine.singletonInstance<QObject*>("Redvault", "App");
        REQUIRE(app);
    }

    void setPinned(bool pinned) {
        QObject* keys = app->property("deviceKeys").value<QObject*>();
        REQUIRE(keys);
        keys->setProperty("pinnedChat", pinned ? QStringLiteral("chat-1") : QString());
    }

    void send(const QString& text) {
        input->setProperty("text", text);
        QMetaObject::invokeMethod(root.get(), "send");
    }

    QString sent() const { return session->property("lastSent").toString(); }

    Application& application = qt();  // before the engine
    QQmlEngine engine;
    std::unique_ptr<QObject> session;
    std::unique_ptr<QObject> root;
    QObject* input = nullptr;
    QObject* app = nullptr;
};

}  // namespace

TEST_CASE("control: a chat that isn't on device keys sends from the in-call panel", "[app][minichat]") {
    Panel p;
    p.send(QStringLiteral("hello"));
    CHECK(p.sent() == QStringLiteral("hello"));
    CHECK(p.root->property("deviceKeyChat").toBool() == false);
}

TEST_CASE("a chat pinned to device keys can't send from the in-call panel", "[app][minichat]") {
    Panel p;
    p.setPinned(true);
    // chatPinned is a function, so the binding needs something it reads to change, as switching chats does.
    p.session->setProperty("currentChatId", QStringLiteral("chat-2"));
    p.session->setProperty("currentChatId", QStringLiteral("chat-1"));
    CHECK(p.root->property("deviceKeyChat").toBool());
    p.send(QStringLiteral("this must not go out on protocol 1"));
    CHECK(p.sent().isEmpty());
    CHECK_FALSE(p.input->property("enabled").toBool());
}

TEST_CASE("a chat switched to device keys but not pinned yet can't send either", "[app][minichat]") {
    Panel p;
    p.app->property("deviceKeys").value<QObject*>()->setProperty("chatActive", true);
    p.send(QStringLiteral("nor this"));
    CHECK(p.sent().isEmpty());
}
