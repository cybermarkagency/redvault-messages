#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/vault_repository.hpp"

class QWindow;

namespace redvault::app {

class ChatSession;
class WindowsHello;

/**
 * The password manager (Passwords page). Items are decrypted only while the
 * vault is unlocked: locking (Lock now, 5 minutes without using the app, or
 * the app's own lock) drops every decrypted item from memory, and unlocking
 * takes Windows Hello or the account password. Passwords are masked by
 * default; copied ones leave the clipboard after 30 seconds. The security
 * check (reused, weak) runs on this computer and sends nothing anywhere; the
 * breach check ("exposed") sends only the first 5 characters of each
 * password's SHA-1 hash, through our server, to Have I Been Pwned.
 */
class VaultSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY itemsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool helloAvailable READ helloAvailable NOTIFY helloAvailableChanged)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)
    /// Items left out because they aren't what this computer last saw, or are in another item's place.
    Q_PROPERTY(int refused READ refused NOTIFY itemsChanged)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY itemsChanged)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY itemsChanged)
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY itemsChanged)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(QString totpCode READ totpCode NOTIFY totpChanged)
    Q_PROPERTY(int totpSecondsLeft READ totpSecondsLeft NOTIFY totpChanged)
    Q_PROPERTY(int totpPeriod READ totpPeriod NOTIFY totpChanged)
    Q_PROPERTY(int lockAfterMinutes READ lockAfterMinutes CONSTANT)
    // Generator
    Q_PROPERTY(QString generated READ generated NOTIFY generatorChanged)
    Q_PROPERTY(int genLength READ genLength WRITE setGenLength NOTIFY generatorChanged)
    Q_PROPERTY(bool genUpper READ genUpper WRITE setGenUpper NOTIFY generatorChanged)
    Q_PROPERTY(bool genDigits READ genDigits WRITE setGenDigits NOTIFY generatorChanged)
    Q_PROPERTY(bool genSymbols READ genSymbols WRITE setGenSymbols NOTIFY generatorChanged)
    Q_PROPERTY(bool genWords READ genWords WRITE setGenWords NOTIFY generatorChanged)
    Q_PROPERTY(QVariantMap generatedStrength READ generatedStrength NOTIFY generatorChanged)

public:
    VaultSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                 QObject* parent = nullptr);
    ~VaultSession() override;

    bool locked() const { return locked_; }
    bool loaded() const { return loaded_; }
    bool busy() const { return busy_; }
    bool helloAvailable() const { return helloAvailable_; }
    QString error() const { return error_; }
    QString notice() const { return notice_; }
    int refused() const { return refused_; }
    QString category() const { return category_; }
    void setCategory(const QString& category);
    QString search() const { return search_; }
    void setSearch(const QString& search);
    QVariantList items() const;
    QVariantMap counts() const;
    QString selectedId() const { return selectedId_; }
    QVariantMap selected() const;
    QString totpCode() const { return totpCode_; }
    int totpSecondsLeft() const { return totpSecondsLeft_; }
    int totpPeriod() const { return totpPeriod_; }
    int lockAfterMinutes() const { return 5; }

    QString generated() const { return generated_; }
    int genLength() const { return genLength_; }
    void setGenLength(int length);
    bool genUpper() const { return genUpper_; }
    void setGenUpper(bool on);
    bool genDigits() const { return genDigits_; }
    void setGenDigits(bool on);
    bool genSymbols() const { return genSymbols_; }
    void setGenSymbols(bool on);
    bool genWords() const { return genWords_; }
    void setGenWords(bool on);
    QVariantMap generatedStrength() const;

    Q_INVOKABLE void lockNow();
    Q_INVOKABLE void unlockWithHello(QWindow* window);
    Q_INVOKABLE void unlockWithPassword(const QString& password);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void select(const QString& id);
    /// A blank item of this type ("login", "passkey", "card", "note", "wifi") for the editor.
    Q_INVOKABLE QVariantMap blankItem(const QString& type) const;
    /// The selected item's editable fields (with the real password), for the editor.
    Q_INVOKABLE QVariantMap editableItem(const QString& id) const;
    /// Creates (no "id") or saves an item from the editor's fields.
    Q_INVOKABLE void saveItem(const QVariantMap& fields);
    Q_INVOKABLE void deleteItem(const QString& id);
    /// The selected item's password, for "Show password" (never kept in QML state longer than shown).
    Q_INVOKABLE QString revealPassword(const QString& id) const;
    /// Copies; `secret` ones are cleared from the clipboard after 30 seconds.
    Q_INVOKABLE void copy(const QString& text, bool secret);
    Q_INVOKABLE void copyField(const QString& id, const QString& field);
    Q_INVOKABLE void regenerate();
    /// Sets the generated password on this item (and notes when it changed).
    Q_INVOKABLE void useGenerated(const QString& id);
    Q_INVOKABLE void share(const QString& id, const QString& userId);
    Q_INVOKABLE void unshare(const QString& id, const QString& userId);
    /// Resets the idle timer (any use of the app counts).
    Q_INVOKABLE void touch();
    Q_INVOKABLE void clearMessages();
    /// Called when the app's own lock engages.
    void lockForAppLock() { lockNow(); }
    /// Strength of any password, for the editor: {label, fraction, tone}.
    Q_INVOKABLE QVariantMap strengthOf(const QString& password) const;

signals:
    void lockedChanged();
    void itemsChanged();
    void selectedChanged();
    void busyChanged();
    void helloAvailableChanged();
    void messagesChanged();
    void totpChanged();
    void generatorChanged();
    void saved(const QString& id);
    void sessionExpired();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Row {
        data::VaultEntry entry;
        QString status;  // "exposed", "reused", "weak" or ""
    };
    void load();
    void unlocked();
    void recompute();
    /// Looks up passwords not checked yet (k-anonymity: 5 hash characters per lookup).
    void checkBreaches();
    /// "Checking privately…", "Found in 3 breaches", ... for the selected item.
    QString breachText(const std::string& secret) const;
    void updateTotp();
    void setBusy(bool busy);
    void setMessages(const QString& error, const QString& notice);
    const Row* find(const QString& id) const;
    QString handleFor(const std::string& userId) const;

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    std::shared_ptr<data::VaultRepository> repository_;
    int refused_ = 0;
    QPointer<ChatSession> chats_;
    WindowsHello* hello_ = nullptr;
    QThreadPool pool_;
    QTimer idleTimer_;
    QTimer totpTimer_;
    QTimer clipboardTimer_;
    QString clipboardText_;

    bool locked_ = false;
    bool loaded_ = false;
    bool busy_ = false;
    bool helloAvailable_ = false;
    QString error_;
    QString notice_;
    QString category_ = QStringLiteral("all");
    QString search_;
    std::vector<Row> rows_;
    // SHA-1 (hex) of each password -> times seen in breaches; kBreachChecking / kBreachFailed while unknown.
    // Dropped on lock, like the items.
    std::map<std::string, std::int64_t> breaches_;
    bool breachCheckOff_ = false;
    int breachGeneration_ = 0;
    QString selectedId_;
    QString totpCode_;
    int totpSecondsLeft_ = 0;
    int totpPeriod_ = 30;

    QString generated_;
    int genLength_ = 24;
    bool genUpper_ = true;
    bool genDigits_ = true;
    bool genSymbols_ = true;
    bool genWords_ = false;
};

}  // namespace redvault::app
