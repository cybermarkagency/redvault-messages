// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "app_lock.hpp"

#include <QGuiApplication>
#include <QPointer>
#include <QSettings>

#include <array>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::app {

namespace {

struct Timeout {
    qint64 millis;
    const char* label;
};

constexpr std::array<Timeout, 5> kTimeouts = {{
    {0, "Immediately"},
    {30'000, "After 30 seconds"},
    {60'000, "After 1 minute"},
    {5 * 60'000, "After 5 minutes"},
    {15 * 60'000, "After 15 minutes"},
}};
constexpr int kDefaultTimeout = 2;  // one minute, like Android
constexpr qint64 kExternalGraceMs = 10 * 60 * 1000;

const QString kHash = QStringLiteral("appLock/pinHash");
const QString kSalt = QStringLiteral("appLock/pinSalt");
const QString kLength = QStringLiteral("appLock/pinLength");
const QString kTimeout = QStringLiteral("appLock/timeout");
// Older versions kept the count of wrong tries here. Only ever removed now.
const QString kOldFailed = QStringLiteral("appLock/failedAttempts");

bool isValidPin(const QString& pin) {
    if (pin.size() < AppLock::kMinPinLength || pin.size() > AppLock::kMaxPinLength) return false;
    for (const QChar c : pin) {
        if (c < u'0' || c > u'9') return false;
    }
    return true;
}

/// Argon2id with the password's parameters: slow on purpose, so a copied settings file can't be brute-forced quickly.
QByteArray hashPin(const QString& pin, const crypto::Bytes& salt) {
    const auto key = crypto::deriveWrapKey(pin.toStdString(), salt);
    return QByteArray(reinterpret_cast<const char*>(key.data()), static_cast<qsizetype>(key.size()));
}

}  // namespace

AppLock::AppLock(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, &AppLock::onApplicationStateChanged);
    loadPin();
}

AppLock::~AppLock() {
    pool_.waitForDone();
}

void AppLock::loadPin() {
    QSettings settings;
    pinHash_ = QByteArray::fromBase64(settings.value(kHash).toByteArray());
    pinSalt_ = QByteArray::fromBase64(settings.value(kSalt).toByteArray());
    pinLength_ = settings.value(kLength, kMinPinLength).toInt();
    const int index = settings.value(kTimeout, kDefaultTimeout).toInt();
    timeoutIndex_ = index >= 0 && index < static_cast<int>(kTimeouts.size()) ? index : kDefaultTimeout;
    settings.remove(kOldFailed);
    failed_ = 0;
}

bool AppLock::enabled() const {
    return !pinHash_.isEmpty();
}

int AppLock::pinLength() const {
    return pinLength_;
}

int AppLock::timeoutIndex() const {
    return timeoutIndex_;
}

void AppLock::setTimeoutIndex(int index) {
    if (index < 0 || index >= static_cast<int>(kTimeouts.size()) || index == timeoutIndex()) return;
    QSettings().setValue(kTimeout, index);
    timeoutIndex_ = index;
    emit settingsChanged();
}

QStringList AppLock::timeoutLabels() const {
    QStringList out;
    for (const auto& t : kTimeouts) out << QString::fromLatin1(t.label);
    return out;
}

void AppLock::setSignedIn(bool signedIn) {
    signedIn_ = signedIn;
    backgroundFor_.invalidate();
    externalFor_.invalidate();
    // The password was just proved (or the session is over), so this is the point to read the PIN again.
    loadPin();
    emit settingsChanged();
    setLocked(false);
}

void AppLock::onApplicationStateChanged(Qt::ApplicationState state) {
    if (state != Qt::ApplicationActive) {
        if (!backgroundFor_.isValid()) backgroundFor_.start();
        return;
    }
    if (!backgroundFor_.isValid()) return;
    const qint64 away = backgroundFor_.elapsed();
    backgroundFor_.invalidate();
    const bool external = externalFor_.isValid() && externalFor_.elapsed() < kExternalGraceMs;
    externalFor_.invalidate();
    if (external) return;
    if (signedIn_ && enabled() && away >= kTimeouts[static_cast<std::size_t>(timeoutIndex())].millis) setLocked(true);
}

void AppLock::expectExternalReturn() {
    externalFor_.start();
}

void AppLock::lockNow() {
    if (signedIn_ && enabled()) setLocked(true);
}

void AppLock::setPin(const QString& pin, const QString& confirm) {
    if (busy_ || locked_) return;
    if (!isValidPin(pin)) {
        setError(QStringLiteral("Use %1 to %2 digits.").arg(kMinPinLength).arg(kMaxPinLength));
        return;
    }
    if (pin != confirm) {
        setError(QStringLiteral("The PINs don't match."));
        return;
    }
    setError({});
    setBusy(true);
    QPointer<AppLock> self(this);
    pool_.start([self, pin] {
        crypto::Bytes salt = crypto::randomBytes(crypto::kPwhashSaltBytes);
        const QByteArray hash = hashPin(pin, salt);
        const QByteArray saltBytes(reinterpret_cast<const char*>(salt.data()), static_cast<qsizetype>(salt.size()));
        QMetaObject::invokeMethod(
            qApp,
            [self, hash, saltBytes, length = pin.size()] {
                if (!self) return;
                QSettings settings;
                settings.setValue(kHash, hash.toBase64());
                settings.setValue(kSalt, saltBytes.toBase64());
                settings.setValue(kLength, length);
                self->pinHash_ = hash;
                self->pinSalt_ = saltBytes;
                self->pinLength_ = length;
                self->failed_ = 0;
                self->setBusy(false);
                emit self->settingsChanged();
                emit self->pinSet();
            },
            Qt::QueuedConnection);
    });
}

void AppLock::disable() {
    if (locked_) return;
    clearPin();
}

void AppLock::unlock(const QString& pin) {
    if (busy_ || !locked_) return;
    // The lock is held against what was set at sign-in. If the file disagrees, something has been at it,
    // and a lock with no PIN to check must not open: the password decides from here.
    QSettings settings;
    const QByteArray onDisk = QByteArray::fromBase64(settings.value(kHash).toByteArray());
    const QByteArray saltOnDisk = QByteArray::fromBase64(settings.value(kSalt).toByteArray());
    if (pinHash_.isEmpty() || pinSalt_.isEmpty() || onDisk != pinHash_ || saltOnDisk != pinSalt_) {
        forceSignOut(QStringLiteral("The app lock's PIN was changed or removed. Sign in with your password."));
        return;
    }
    const QByteArray stored = pinHash_;
    const QByteArray salt = pinSalt_;
    setError({});
    setBusy(true);
    QPointer<AppLock> self(this);
    pool_.start([self, pin, stored, salt] {
        const crypto::Bytes saltBytes(salt.begin(), salt.end());
        const QByteArray candidate = hashPin(pin, saltBytes);
        const bool ok = crypto::constantTimeEquals(
            crypto::ByteView(reinterpret_cast<const std::uint8_t*>(candidate.data()), static_cast<std::size_t>(candidate.size())),
            crypto::ByteView(reinterpret_cast<const std::uint8_t*>(stored.data()), static_cast<std::size_t>(stored.size())));
        QMetaObject::invokeMethod(
            qApp,
            [self, ok] {
                if (!self) return;
                self->setBusy(false);
                if (ok) {
                    self->failed_ = 0;
                    self->setLocked(false);
                    return;
                }
                if (++self->failed_ >= kMaxAttempts) {
                    self->forceSignOut(QStringLiteral("Too many wrong PINs. Sign in with your password."));
                    return;
                }
                const int left = kMaxAttempts - self->failed_;
                self->setError(left == 1 ? QStringLiteral("Wrong PIN. 1 try left before you're signed out.")
                                         : QStringLiteral("Wrong PIN. %1 tries left.").arg(left));
            },
            Qt::QueuedConnection);
    });
}

void AppLock::clearError() {
    setError({});
}

void AppLock::clearPin() {
    QSettings settings;
    settings.remove(kHash);
    settings.remove(kSalt);
    settings.remove(kLength);
    settings.remove(kOldFailed);
    pinHash_.clear();
    pinSalt_.clear();
    pinLength_ = kMinPinLength;
    failed_ = 0;
    emit settingsChanged();
}

void AppLock::forceSignOut(const QString& reason) {
    clearPin();
    // Still locked: signing out is what unlocks.
    emit lockedOut(reason);
}

void AppLock::setLocked(bool locked) {
    if (locked_ == locked) return;
    locked_ = locked;
    if (!locked) setError({});
    emit lockedChanged();
}

void AppLock::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void AppLock::setError(const QString& error) {
    if (error_ == error) return;
    error_ = error;
    emit errorChanged();
}

}  // namespace redvault::app
