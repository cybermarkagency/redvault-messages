#pragma once

#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <deque>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/drive_repository.hpp"
#include "redvault/data/file_key_repository.hpp"

namespace redvault::app {

class ChatSession;
class TorController;
struct LockSpec;

/**
 * Encrypted cloud storage (the Files page). Files and their names are
 * encrypted on this computer before upload, each plan up to its own total
 * (Free 5 GB, Pro 50 GB, Premium 100 GB); a file that doesn't fit is
 * refused before it's even encrypted, with a notification. Search runs on
 * this computer over the decrypted index. Also sharing with people, share
 * links (expiry, password, download), versions, the trash, and "PC backup",
 * which keeps a chosen folder on this computer backed up.
 *
 * Extra locks (files_locks.cpp): a file's key can also be locked under one
 * of the person's own encryption keys (kept in the password vault) or a
 * password. Unlocking lasts for this session only: unlocked file keys and
 * opened encryption keys live in this object's memory and go with it.
 */
class FilesSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by App on sign-in")
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY messagesChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY messagesChanged)
    Q_PROPERTY(QString section READ section WRITE setSection NOTIFY changed)
    Q_PROPERTY(QString folderId READ folderId NOTIFY changed)
    Q_PROPERTY(QVariantList breadcrumb READ breadcrumb NOTIFY changed)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY changed)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap usage READ usage NOTIFY changed)
    Q_PROPERTY(QString pcBackupPath READ pcBackupPath NOTIFY pcBackupChanged)
    Q_PROPERTY(QString pcBackupStatus READ pcBackupStatus NOTIFY pcBackupChanged)
    // Share-link settings for the next link (the details panel's switches).
    Q_PROPERTY(bool linkExpires READ linkExpires WRITE setLinkExpires NOTIFY linkSettingsChanged)
    Q_PROPERTY(bool linkPassword READ linkPassword WRITE setLinkPassword NOTIFY linkSettingsChanged)
    Q_PROPERTY(bool linkAllowDownload READ linkAllowDownload WRITE setLinkAllowDownload NOTIFY linkSettingsChanged)
    // Extra locks and encryption keys.
    Q_PROPERTY(int lockedCount READ lockedCount NOTIFY changed)
    Q_PROPERTY(QVariantList keys READ keys NOTIFY keysChanged)
    Q_PROPERTY(bool keysLoaded READ keysLoaded NOTIFY keysChanged)
    Q_PROPERTY(QString selectedKeyId READ selectedKeyId NOTIFY keysChanged)
    Q_PROPERTY(QVariantMap selectedKey READ selectedKey NOTIFY keysChanged)
    /// The selected key as 64 hex characters in groups of 8, while revealed for backup; empty otherwise.
    Q_PROPERTY(QString exportedKey READ exportedKey NOTIFY keysChanged)
    /// A lock or key operation is running (Argon2id takes a moment): dialogs show it and wait.
    Q_PROPERTY(bool lockBusy READ lockBusy NOTIFY lockBusyChanged)

public:
    /// `tor`: when routing goes through Tor, uploads are resumable (parts + a sealed journal) and pause while Tor is down.
    FilesSession(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const data::SelfIdentity> identity, ChatSession* chats,
                 QString webAppBase, TorController* tor = nullptr, QObject* parent = nullptr);
    ~FilesSession() override;

    bool loaded() const { return loaded_; }
    QString error() const { return error_; }
    QString notice() const { return notice_; }
    QString section() const { return section_; }
    void setSection(const QString& section);
    QString folderId() const { return QString::fromStdString(folderId_); }
    QVariantList breadcrumb() const;
    QString search() const { return search_; }
    void setSearch(const QString& search);
    QVariantList folders() const;
    QVariantList rows() const;
    QString selectedId() const { return selectedId_; }
    QVariantMap selected() const;
    QVariantMap usage() const;
    QString pcBackupPath() const { return pcBackupPath_; }
    QString pcBackupStatus() const { return pcBackupStatus_; }
    bool linkExpires() const { return linkExpires_; }
    void setLinkExpires(bool on);
    bool linkPassword() const { return linkPassword_; }
    void setLinkPassword(bool on);
    bool linkAllowDownload() const { return linkAllowDownload_; }
    void setLinkAllowDownload(bool on);
    int lockedCount() const;
    QVariantList keys() const;
    bool keysLoaded() const { return keysLoaded_; }
    QString selectedKeyId() const { return selectedKeyId_; }
    QVariantMap selectedKey() const;
    QString exportedKey() const { return exportedKey_; }
    bool lockBusy() const { return lockBusy_; }
    /// What the last refresh decrypted (public share links read it to keep folder links in step).
    const data::DriveSnapshot& snapshot() const { return snapshot_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void openFolder(const QString& id);
    Q_INVOKABLE void select(const QString& id);
    /// Local files (file:// URLs) into the current folder.
    Q_INVOKABLE void upload(const QList<QUrl>& files);
    /// Stops an upload (running, waiting or paused); a resumable one is dropped on the server too.
    Q_INVOKABLE void cancelUpload(const QString& localId);
    /// Paused uploads (Tor or the connection was down) try again now.
    Q_INVOKABLE void resumePaused();
    /// A new version of an existing file.
    Q_INVOKABLE void uploadNewVersion(const QString& id, const QUrl& file);
    Q_INVOKABLE void newFolder(const QString& name);
    Q_INVOKABLE void rename(const QString& id, const QString& name);
    Q_INVOKABLE void move(const QString& id, const QString& folderId);
    Q_INVOKABLE void trash(const QString& id);
    Q_INVOKABLE void restore(const QString& id);
    Q_INVOKABLE void deleteForever(const QString& id);
    Q_INVOKABLE void emptyTrash();
    /// Decrypts to where the user chose.
    Q_INVOKABLE void download(const QString& id, const QUrl& destination);
    /// Decrypts into the session cache and opens it with its app (not for programs).
    Q_INVOKABLE void openFile(const QString& id);
    Q_INVOKABLE void shareWith(const QString& id, const QString& userId);
    Q_INVOKABLE void unshare(const QString& id, const QString& userId);
    /// A share link with the current switches (and this password if "Require a password" is on); copies it.
    Q_INVOKABLE void createLink(const QString& id, const QString& password);
    Q_INVOKABLE void copyLink(const QString& id, const QString& linkId);
    Q_INVOKABLE void deleteLink(const QString& id, const QString& linkId);
    /// Folders the selected entry can move to: [{id, name}], the top level first.
    Q_INVOKABLE QVariantList moveTargets(const QString& id) const;
    Q_INVOKABLE void setPcBackupFolder(const QUrl& folder);
    Q_INVOKABLE void stopPcBackup();
    Q_INVOKABLE void syncPcBackupNow();
    Q_INVOKABLE void clearMessages();

    // --- Extra locks (each reports back through lockDone) ---
    /// Name, size and type tile of local files, for the upload dialog.
    Q_INVOKABLE QVariantList describeFiles(const QList<QUrl>& files) const;
    /// Uploads with an extra lock: mode "none", "key" (keyId, and keyPassword if it has one) or "password".
    Q_INVOKABLE void uploadLocked(const QList<QUrl>& files, const QString& mode, const QString& keyId, const QString& keyPassword,
                                  const QString& filePassword);
    /// Whether using this key needs its password now (it has one and wasn't opened this session).
    Q_INVOKABLE bool keyNeedsPassword(const QString& keyId) const;
    /// Unlocks a file for this session: mode "key" or "password".
    Q_INVOKABLE void unlockFile(const QString& id, const QString& mode, const QString& keyId, const QString& keyPassword,
                                const QString& filePassword);
    /// Forgets a file's unlocked key: it's locked again.
    Q_INVOKABLE void lockNow(const QString& id);
    /// Adds, changes or (mode "none") removes a file's lock. The file must be unlocked first.
    Q_INVOKABLE void changeLock(const QString& id, const QString& mode, const QString& keyId, const QString& keyPassword,
                                const QString& filePassword);

    // --- Encryption keys ---
    Q_INVOKABLE void refreshKeys();
    Q_INVOKABLE void selectKey(const QString& id);
    Q_INVOKABLE void createKey(const QString& name, const QString& password);
    /// Checks a pasted key as it's typed: {state: "empty"|"valid"|"invalid"|"duplicate", format, fingerprint, message, name}.
    Q_INVOKABLE QVariantMap checkImport(const QString& text) const;
    Q_INVOKABLE void importKey(const QString& name, const QString& text, const QString& password);
    Q_INVOKABLE void renameKey(const QString& id, const QString& name);
    /// Sets, changes or (empty `next`) removes a key's password.
    Q_INVOKABLE void setKeyPassword(const QString& id, const QString& current, const QString& next);
    Q_INVOKABLE void revealKeyExport(const QString& id, const QString& password);
    Q_INVOKABLE void hideKeyExport();
    /// Copies the revealed export; it clears from the clipboard after 30 seconds.
    Q_INVOKABLE void copyKeyExport();
    Q_INVOKABLE void deleteKey(const QString& id);
    /// Files whose current version is locked with this key.
    Q_INVOKABLE int filesLockedWith(const QString& keyId) const;

    // --- Passwords for keys and files ---
    Q_INVOKABLE QString suggestPassword(bool words) const;
    /// Why this password can't protect files (empty: it can).
    Q_INVOKABLE QString passwordProblem(const QString& password) const;
    /// {label, fraction, tone, length} for StrengthBar.
    Q_INVOKABLE QVariantMap strengthOf(const QString& password) const;

signals:
    /// A refresh landed: Files may have changed (new files, renames, moves, the Trash).
    void snapshotApplied();
    void changed();
    void selectedChanged();
    void messagesChanged();
    void pcBackupChanged();
    void linkSettingsChanged();
    void linkCreated(const QString& url);
    void keysChanged();
    void lockBusyChanged();
    /// A lock or key operation finished: `action` names it ("upload", "unlock", "changeLock", "createKey",
    /// "importKey", "keyPassword", "export", "renameKey", "deleteKey"); `error` is empty on success.
    void lockDone(const QString& action, const QString& error);
    /// A file didn't fit the plan's storage (for the tray notification).
    void storageFull(const QString& message);
    void sessionExpired();

private:
    struct Upload {
        QString localId;
        std::filesystem::path path;
        std::string parent;
        std::optional<data::DriveEntry> replacing;
        nlohmann::json extraMeta = nlohmann::json::object();
        /// An extra lock for this upload (null: none).
        std::shared_ptr<LockSpec> lock;
        /// PC backup: the file's folder inside the chosen one ("Invoices/2024"), made under `parent` if missing.
        std::string subPath;
        QString name;
        std::int64_t size = 0;
        QString stage;  // "queued", "encrypting", "uploading", "paused", "failed"
        /// A resumable upload the server already knows (from the journal): continue it, don't start over.
        std::string resumeObjectId;
        /// Set to stop it (Cancel).
        std::shared_ptr<data::TransferControl> control = std::make_shared<data::TransferControl>();
        double progress = 0;
        QString error;
    };

    void apply(data::DriveSnapshot snapshot);
    void startNextUpload();
    TorController* tor_ = nullptr;
    QTimer pausedTimer_;
    void fail(const QString& message);
    void setMessages(const QString& error, const QString& notice);
    const data::DriveEntry* find(const QString& id) const;
    data::DriveEntry* findMutable(const QString& id);
    /// The current version of each file, keyed by fileId.
    std::map<std::string, const data::DriveEntry*> currentVersions() const;
    int versionsOf(const data::DriveEntry& entry) const;
    QVariantMap rowFor(const data::DriveEntry& entry) const;
    QString handleFor(const std::string& userId) const;
    void saveEntry(const data::DriveEntry& entry, const QString& notice);
    void loadPreview(const data::DriveEntry& entry);
    std::string pcBackupFolderId() const;
    /// Whether `entry` sits somewhere inside the folder `folderId`.
    bool isInside(const data::DriveEntry& entry, const std::string& folderId) const;
    void runPcBackup();
    /// Queues local files for upload into the current folder, each with `lock` (null: no extra lock).
    void enqueueUploads(const QList<QUrl>& files, std::shared_ptr<LockSpec> lock);

    /// A copy of `entry` with its file key filled in if it was unlocked this session.
    data::DriveEntry usable(const data::DriveEntry& entry) const;
    const data::FileKey* findKey(const std::string& id) const;
    /// Builds the lock for `mode`, opening the key first when needed (off the UI thread), then calls `then`.
    void withLockSpec(const QString& action, const QString& mode, const QString& keyId, const QString& keyPassword,
                      const QString& filePassword, std::function<void(std::shared_ptr<LockSpec>)> then);
    void setLockBusy(bool busy);
    void finishLock(const QString& action, const QString& error);

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const data::SelfIdentity> identity_;
    std::shared_ptr<data::DriveRepository> repository_;
    QPointer<ChatSession> chats_;
    QString webAppBase_;
    std::filesystem::path cacheDir_;
    /// Held while this session runs, so a sweep for abandoned caches skips this one.
    std::unique_ptr<QLockFile> cacheLock_;
    QThreadPool pool_;
    QThreadPool uploadPool_;
    QTimer refreshTimer_;
    QTimer pcBackupTimer_;

    bool loaded_ = false;
    /// Only the newest refresh is applied: they can finish out of order.
    int refreshGeneration_ = 0;
    QString error_;
    QString notice_;
    QString section_ = QStringLiteral("all");
    std::string folderId_;
    QString search_;
    data::DriveSnapshot snapshot_;
    std::deque<Upload> uploads_;
    bool uploading_ = false;
    QString selectedId_;
    std::map<std::string, QString> previews_;  // entry id -> local file URL
    std::map<std::string, std::vector<std::filesystem::path>> openedCopies_;  // entry id -> decrypted copies opened in other apps
    bool linkExpires_ = true;
    bool linkPassword_ = true;
    bool linkAllowDownload_ = false;
    QString pcBackupPath_;
    QString pcBackupStatus_;
    bool pcBackupRunning_ = false;

    std::shared_ptr<data::VaultRepository> vault_;
    std::shared_ptr<data::FileKeyRepository> keyRepository_;
    std::vector<data::FileKey> keys_;
    bool keysLoaded_ = false;
    QString selectedKeyId_;
    QString exportedKey_;
    bool lockBusy_ = false;
    /// Entry id -> its file key (base64), unlocked this session.
    std::map<std::string, std::string> unlocked_;
    /// Encryption keys opened this session (their password entered once), by vault item id.
    std::map<std::string, std::shared_ptr<crypto::SecretBytes>> openedKeys_;
    QTimer clipboardTimer_;
    QString clipboardText_;
};

/// An extra lock chosen for an upload or a change (built on the UI thread, used on the upload thread).
struct LockSpec {
    data::FileLock::Kind kind = data::FileLock::Kind::Password;
    std::string keyId;
    std::string fingerprint;
    std::shared_ptr<crypto::SecretBytes> key;
    std::string password;

    data::FileLock lock() const {
        return kind == data::FileLock::Kind::Key ? data::FileLock::withKey(keyId, fingerprint, key->view()) : data::FileLock::withPassword(password);
    }
};

}  // namespace redvault::app
