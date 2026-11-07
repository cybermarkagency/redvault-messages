#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class QWindow;

namespace redvault::app {

/**
 * Windows Hello (face, fingerprint or the device PIN) as a presence check
 * for unlocking the password vault. It confirms the person at the computer
 * is the account's owner on this PC; the vault's key itself stays derived
 * from the signed-in account's private key and never touches Hello.
 * Not available on Linux (unlock with the account password there).
 */
class WindowsHello : public QObject {
    Q_OBJECT
public:
    explicit WindowsHello(QObject* parent = nullptr);
    ~WindowsHello() override;

    /// Checks (once, asynchronously) whether Hello is set up on this PC.
    void checkAvailability(std::function<void(bool)> done);
    /// Shows Windows' own Hello prompt over `window`; `done(true)` only when verified.
    void verify(QWindow* window, const QString& message, std::function<void(bool verified, QString error)> done);

private:
    struct Pending;
    Pending* pending_ = nullptr;
    QTimer poll_;
};

}  // namespace redvault::app
