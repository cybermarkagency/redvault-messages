#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace redvault::app {

/**
 * The app-lock PIN, like Android's AppLockStore and AppLock: stored only as
 * an Argon2id hash with a random salt (the same KDF and cost as the
 * password), never the PIN itself. The app locks after it has been in the
 * background (another window active, or hidden in the tray) longer than the
 * chosen timeout, or on demand. Ten wrong PINs in a row end the session,
 * which wipes the keys from memory; getting back in then takes the account
 * password.
 *
 * The PIN and the count of wrong tries are held in memory for the session, so
 * editing the settings file while the app runs changes neither. A lock whose
 * stored PIN has gone missing or changed ends the session rather than opening.
 * The count needs no file: quitting ends the session anyway.
 *
 * What this protects against: someone at the keyboard of an unattended, running
 * app. It is a screen lock. It doesn't protect against a process running as the
 * same user, or a compromised OS: those can read this app's memory (the keys
 * included), inject input, or edit the settings before the next sign-in, and the
 * hash file is only as private as the user's own account. Only the operating
 * system's account and disk protection can address that.
 */
class AppLock : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by App")
    Q_PROPERTY(bool enabled READ enabled NOTIFY settingsChanged)
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
    Q_PROPERTY(int pinLength READ pinLength NOTIFY settingsChanged)
    Q_PROPERTY(int timeoutIndex READ timeoutIndex WRITE setTimeoutIndex NOTIFY settingsChanged)
    Q_PROPERTY(QStringList timeoutLabels READ timeoutLabels CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    static constexpr int kMinPinLength = 4;
    static constexpr int kMaxPinLength = 12;
    /// After this many wrong PINs in a row the session ends.
    static constexpr int kMaxAttempts = 10;

    explicit AppLock(QObject* parent = nullptr);
    ~AppLock() override;

    bool enabled() const;
    bool locked() const { return locked_; }
    int pinLength() const;
    int timeoutIndex() const;
    void setTimeoutIndex(int index);
    QStringList timeoutLabels() const;
    bool busy() const { return busy_; }
    QString error() const { return error_; }

    /// Only a signed-in app locks; signing in or out always leaves it unlocked.
    void setSignedIn(bool signedIn);

    Q_INVOKABLE void setPin(const QString& pin, const QString& confirm);
    Q_INVOKABLE void disable();
    Q_INVOKABLE void lockNow();
    Q_INVOKABLE void unlock(const QString& pin);
    /**
     * Call just before opening a window outside the app for the app itself
     * (a file dialog, a received file in its viewer): coming back from it
     * within 10 minutes doesn't lock, so what the user was doing isn't lost.
     */
    Q_INVOKABLE void expectExternalReturn();
    Q_INVOKABLE void clearError();

signals:
    void settingsChanged();
    void lockedChanged();
    void busyChanged();
    void errorChanged();
    /// A new PIN is set (the settings form closes on this).
    void pinSet();
    /// The session must end (too many wrong PINs, or the stored PIN was tampered with). Stays locked until it does.
    void lockedOut(const QString& reason);

private:
    void onApplicationStateChanged(Qt::ApplicationState state);
    void setLocked(bool locked);
    void setBusy(bool busy);
    void setError(const QString& error);
    void clearPin();
    void loadPin();
    void forceSignOut(const QString& reason);

    QThreadPool pool_;
    /// The PIN as of sign-in or the last change; the settings file can't change these behind the app's back.
    QByteArray pinHash_;
    QByteArray pinSalt_;
    int pinLength_ = kMinPinLength;
    int timeoutIndex_ = 0;
    int failed_ = 0;
    bool signedIn_ = false;
    bool locked_ = false;
    bool busy_ = false;
    QString error_;
    QElapsedTimer backgroundFor_;
    QElapsedTimer externalFor_;
};

}  // namespace redvault::app
