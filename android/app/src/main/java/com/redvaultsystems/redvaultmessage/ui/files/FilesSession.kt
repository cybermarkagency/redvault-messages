package com.redvaultsystems.redvaultmessage.ui.files

import kotlinx.coroutines.Job
import com.redvaultsystems.redvaultmessage.tor.TorController
import com.redvaultsystems.redvaultmessage.data.TransferInterruptedException
import android.net.Uri
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.Strength
import com.redvaultsystems.redvaultmessage.crypto.VaultCrypto
import com.redvaultsystems.redvaultmessage.data.DriveEntry
import com.redvaultsystems.redvaultmessage.data.DriveRepository
import com.redvaultsystems.redvaultmessage.data.DriveSnapshot
import com.redvaultsystems.redvaultmessage.data.DriveStage
import com.redvaultsystems.redvaultmessage.data.FileKey
import com.redvaultsystems.redvaultmessage.data.FileKeyRepository
import com.redvaultsystems.redvaultmessage.data.FileLock
import com.redvaultsystems.redvaultmessage.data.FileLocks
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import java.io.File
import java.io.OutputStream
import java.util.UUID

/** A file being encrypted and uploaded. */
data class UploadItem(
    val localId: String,
    val name: String,
    val size: Long,
    val parent: String,
    /** "queued", "encrypting", "uploading", "paused" (Tor or the connection dropped; it resumes) or "failed". */
    val stage: String,
    val progress: Float = 0f,
    val error: String? = null,
    val lockKind: String = "",
    val lockShort: String = "",
    /** A resumable upload the server already knows: it continues from the first part the server is missing. */
    val resumeObjectId: String? = null,
)

data class FilesState(
    val loaded: Boolean = false,
    val snapshot: DriveSnapshot = DriveSnapshot(),
    val uploads: List<UploadItem> = emptyList(),
    /** Entries whose file key is unlocked this session (their ids). */
    val unlocked: Set<String> = emptySet(),
    val keys: List<FileKey> = emptyList(),
    val keysLoaded: Boolean = false,
    /** Keys whose password was entered this session (their ids). */
    val openedKeys: Set<String> = emptySet(),
    /** A lock or key operation is running (Argon2id takes a moment). */
    val busy: Boolean = false,
    val error: String? = null,
    val notice: String? = null,
) {
    /** The current version of each file, by fileId. */
    val currentFiles: List<DriveEntry>
        get() = snapshot.entries.filter { it.isFile }.groupBy { it.fileId }.values.map { versions -> versions.maxBy { it.version } }

    val lockedCount: Int get() = currentFiles.count { !it.trashed && it.locked }

    /** Uploads waiting for Tor (or the connection) to come back. */
    val pausedUploads: Int get() = uploads.count { it.stage == "paused" }

    fun find(id: String): DriveEntry? = snapshot.entries.firstOrNull { it.id == id } ?: snapshot.incoming.firstOrNull { it.id == id }

    fun versionsOf(entry: DriveEntry): Int = snapshot.entries.count { it.isFile && it.fileId == entry.fileId }

    fun filesLockedWith(keyId: String): Int = currentFiles.count { FileLocks.lockKeyId(it.meta) == keyId }

    fun key(id: String): FileKey? = keys.firstOrNull { it.id == id }
}

/** An extra lock chosen in a dialog: "none", "key" or "password". */
data class LockChoice(val mode: String, val keyId: String = "", val keyPassword: String = "", val filePassword: String = "")

/**
 * Files on this phone (a port of the desktop app's FilesSession): the
 * listing, the upload queue, locks and the person's own encryption keys.
 * Unlocked file keys and opened encryption keys live only in this object
 * and are wiped by [close], so unlocking lasts for this session only.
 */
class FilesSession(
    private val repository: DriveRepository,
    private val keyRepository: FileKeyRepository,
    private val locks: FileLocks,
    private val drive: DriveCrypto,
    private val vault: VaultCrypto,
    private val onSessionExpired: () -> Unit,
    /** Tor routing: uploads are resumable and pause while Tor is down (data/DriveRepository.enableResumable). */
    private val tor: TorController? = null,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(FilesState())
    val state: StateFlow<FilesState> = _state.asStateFlow()
    private var uploadJob: Job? = null
    private var uploadingId: String? = null

    init {
        if (tor != null && repository.resumable) {
            // Unfinished uploads from last time come back paused, and go on once Tor is up.
            val pending = repository.pendingUploads().map {
                UploadItem(
                    localId = "upload:" + it.objectId, name = it.name, size = it.size, parent = it.parent,
                    stage = "paused", error = PAUSED, resumeObjectId = it.objectId,
                )
            }
            if (pending.isNotEmpty()) _state.update { it.copy(uploads = it.uploads + pending) }
            scope.launch { tor.becameConnected.collect { resumePaused() } }
            // Dispatched (not immediate): the first try runs after construction, not inside it.
            scope.launch(Dispatchers.Main) {
                while (true) {
                    if (tor.connected) resumePaused()
                    delay(20_000)
                }
            }
        }
    }

    /** Paused uploads go on now (Tor is back). */
    fun resumePaused() {
        if (tor != null && tor.gate() == null) return
        if (_state.value.uploads.none { it.stage == "paused" }) return
        _state.update { s -> s.copy(uploads = s.uploads.map { if (it.stage == "paused") it.copy(stage = "queued", error = null) else it }) }
        startNextUpload()
    }

    /** Stops an upload (running, waiting or paused); one the server already knows is dropped there too. */
    fun cancelUpload(localId: String) {
        val item = _state.value.uploads.firstOrNull { it.localId == localId } ?: return
        if (uploadingId == localId) uploadJob?.cancel()
        _state.update { s -> s.copy(uploads = s.uploads.filterNot { it.localId == localId }) }
        uploadLocks.remove(localId)
        uploadUris.remove(localId)
        uploadReplacing.remove(localId)
        item.resumeObjectId?.let { id -> scope.launch { runCatching { repository.abandonUpload(id) } } }
    }

    private val unlockedKeys = HashMap<String, String>() // entry id -> file key (base64)
    private val openedCopies = HashMap<String, MutableList<File>>() // entry id -> decrypted copies in the cache
    private val openedKeys = HashMap<String, ByteArray>() // key id -> the key
    private val uploadLocks = HashMap<String, FileLock?>() // local upload id -> its lock
    private val uploadUris = HashMap<String, Uri>()
    private val uploadReplacing = HashMap<String, DriveEntry>() // local upload id -> the file it is a new version of
    private val uploadMutex = Mutex()
    private var uploading = false

    val webAppBase: String get() = repository.webAppBase

    /** Refreshes until cancelled. Run it while Files is on screen. */
    suspend fun poll() {
        while (true) {
            refreshNow()
            delay(60_000)
        }
    }

    suspend fun refreshNow() {
        guarded { _state.update { it.copy(snapshot = repository.refresh(), loaded = true) } }
        if (!_state.value.keysLoaded) refreshKeys()
    }

    fun refresh() {
        scope.launch { refreshNow() }
    }

    /** A copy of [entry] with its file key filled in if it was unlocked this session. */
    fun usable(entry: DriveEntry): DriveEntry {
        val key = unlockedKeys[entry.id] ?: return entry
        return entry.copy(meta = JsonObject(entry.meta + ("key" to JsonPrimitive(key))))
    }

    // --- Passwords ---

    fun suggestPassword(words: Boolean): String = locks.suggestPassword(words)

    fun passwordProblem(password: String): String? = locks.passwordProblem(password)

    fun strength(password: String): Strength? = if (password.isEmpty()) null else vault.estimateStrength(password).strength

    fun keyNeedsPassword(keyId: String): Boolean {
        val key = _state.value.key(keyId) ?: return false
        return key.passwordProtected && !openedKeys.containsKey(key.id)
    }

    // --- Locks ---

    /** Builds the lock for [choice], opening the key first when needed. Throws [UserFacingException]. */
    private suspend fun lockFor(choice: LockChoice): FileLock? = when (choice.mode) {
        "none" -> null
        "password" -> {
            locks.passwordProblem(choice.filePassword)?.let { throw UserFacingException(it) }
            FileLock.WithPassword(choice.filePassword)
        }
        else -> {
            val key = _state.value.key(choice.keyId) ?: throw UserFacingException("Choose a key.")
            val secret = openedKeys[key.id] ?: keyRepository.open(key, choice.keyPassword).also {
                openedKeys[key.id] = it
                _state.update { s -> s.copy(openedKeys = openedKeys.keys.toSet()) }
            }
            FileLock.WithKey(key.id, key.fingerprint, secret)
        }
    }

    /** Uploads picked files into [parent] with an extra lock. Returns an error to show, or null once queued. */
    suspend fun upload(uris: List<Uri>, names: List<Pair<String, Long>>, parent: String, choice: LockChoice): String? = busy {
        val lock = lockFor(choice)
        val key = (lock as? FileLock.WithKey)
        val items = uris.mapIndexed { i, uri ->
            val id = "upload:" + UUID.randomUUID()
            uploadLocks[id] = lock
            uploadUris[id] = uri
            UploadItem(
                localId = id,
                name = names.getOrNull(i)?.first ?: "file",
                size = names.getOrNull(i)?.second ?: 0,
                parent = parent,
                stage = "queued",
                lockKind = when (lock) {
                    is FileLock.WithKey -> "key"
                    is FileLock.WithPassword -> "password"
                    null -> ""
                },
                lockShort = key?.fingerprint?.take(9) ?: "",
            )
        }
        _state.update { it.copy(uploads = it.uploads + items) }
        startNextUpload()
    }

    /**
     * Uploads [uri] as the next version of [entry]: same name, folder and file id, version + 1. Like the desktop's
     * "Upload a new version", the new version carries no extra lock (a lock belongs to one version's key).
     */
    fun uploadNewVersion(uri: Uri, entry: DriveEntry) {
        if (!entry.isFile || entry.trashed || entry.incoming) return
        val id = "upload:" + UUID.randomUUID()
        uploadLocks[id] = null
        uploadUris[id] = uri
        uploadReplacing[id] = entry
        val item = UploadItem(localId = id, name = entry.name, size = entry.size, parent = entry.parent, stage = "queued")
        _state.update { it.copy(uploads = it.uploads + item) }
        startNextUpload()
    }

    private fun startNextUpload() {
        if (uploading) return
        val next = _state.value.uploads.firstOrNull { it.stage == "queued" } ?: return
        uploading = true
        uploadingId = next.localId
        // A resumed upload is already encrypted: it only sends what the server is missing.
        setUpload(next.localId) { it.copy(stage = if (next.resumeObjectId != null) "uploading" else "encrypting") }
        uploadJob = scope.launch {
            try {
                val onProgress: (DriveStage, Long, Long) -> Unit = { stage, done, total ->
                    val progress = if (total > 0) (done.toFloat() / total).coerceIn(0f, 1f) else 0f
                    scope.launch {
                        setUpload(next.localId) {
                            it.copy(stage = if (stage == DriveStage.Encrypting) "encrypting" else "uploading", progress = progress)
                        }
                    }
                }
                val entry = uploadMutex.withLock {
                    val resumeId = next.resumeObjectId
                    if (resumeId != null) {
                        repository.resumeUpload(resumeId, onProgress)
                    } else {
                        repository.upload(
                            uploadUris.getValue(next.localId), next.parent, uploadReplacing[next.localId], uploadLocks[next.localId], onProgress,
                            onStarted = { id -> scope.launch { setUpload(next.localId) { it.copy(resumeObjectId = id) } } },
                        )
                    }
                }
                uploadLocks.remove(next.localId)
                uploadUris.remove(next.localId)
                uploadReplacing.remove(next.localId)
                _state.update { s ->
                    s.copy(
                        uploads = s.uploads.filterNot { it.localId == next.localId },
                        snapshot = s.snapshot.copy(entries = s.snapshot.entries + entry),
                    )
                }
                refresh()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (e is ApiException && e.status == 401) return@launch onSessionExpired()
                if (e is TransferInterruptedException && tor != null && repository.resumable) {
                    // Over Tor: paused, not failed. It goes on from where it stopped once Tor is back.
                    setUpload(next.localId) { it.copy(stage = "paused", error = PAUSED) }
                    return@launch
                }
                val message = e.message ?: "Upload failed. Try again."
                setUpload(next.localId) { it.copy(stage = "failed", error = message) }
                if (message.startsWith("Not enough storage")) {
                    // Nothing else in the queue fits either until space is freed.
                    _state.update { s ->
                        s.copy(uploads = s.uploads.map { if (it.stage == "queued") it.copy(stage = "failed", error = "Not uploaded: your storage is full.") else it })
                    }
                }
                _state.update { it.copy(error = message) }
            } finally {
                uploading = false
                uploadingId = null
                startNextUpload()
            }
        }
    }

    private fun setUpload(localId: String, change: (UploadItem) -> UploadItem) {
        _state.update { s -> s.copy(uploads = s.uploads.map { if (it.localId == localId) change(it) else it }) }
    }

    /** Clears failed uploads and messages. */
    fun dismissMessages() {
        _state.update { s -> s.copy(error = null, notice = null, uploads = s.uploads.filterNot { it.stage == "failed" }) }
    }

    /** Unlocks a file for this session. Returns an error to show, or null. */
    suspend fun unlock(id: String, choice: LockChoice): String? = busy {
        val entry = _state.value.find(id) ?: return@busy
        if (!entry.locked) return@busy
        val lock = if (choice.mode == "password") {
            if (choice.filePassword.isEmpty()) throw UserFacingException("Enter the file's password.")
            FileLock.WithPassword(choice.filePassword)
        } else {
            lockFor(choice.copy(mode = "key"))!!
        }
        val unlockedEntry = repository.unlock(entry, lock)
        unlockedKeys[entry.id] = (unlockedEntry.meta["key"] as JsonPrimitive).content
        _state.update { it.copy(unlocked = unlockedKeys.keys.toSet()) }
    }

    /** Forgets a file's unlocked key: it's locked again. */
    fun lockNow(id: String) {
        unlockedKeys.remove(id)
        // Its decrypted copies go too: a locked file shouldn't leave plaintext behind on the phone.
        synchronized(openedCopies) { openedCopies.remove(id) }?.forEach { it.parentFile?.deleteRecursively() }
        _state.update { it.copy(unlocked = unlockedKeys.keys.toSet(), notice = "Locked again.") }
    }

    /** Adds, changes or (mode "none") removes a file's lock. It must be unlocked first. Returns an error or null. */
    suspend fun changeLock(id: String, choice: LockChoice): String? = busy {
        val found = _state.value.find(id) ?: return@busy
        val entry = usable(found)
        if (entry.needsUnlock) throw UserFacingException("Unlock this file first.")
        val lock = lockFor(choice)
        val saved = repository.setLock(entry, lock)
        if (lock != null) unlockedKeys[saved.id] = (saved.meta["key"] as JsonPrimitive).content else unlockedKeys.remove(saved.id)
        val stored = saved.copy(meta = JsonObject(saved.meta - "key"), sharedWith = found.sharedWith)
        _state.update { s ->
            s.copy(
                snapshot = s.snapshot.copy(entries = s.snapshot.entries.map { if (it.id == stored.id) stored else it }),
                unlocked = unlockedKeys.keys.toSet(),
                notice = if (lock != null) "Lock saved. Nothing was re-encrypted or re-uploaded." else "Lock removed.",
            )
        }
    }

    // --- Files ---

    suspend fun saveTo(id: String, output: OutputStream): String? = busy {
        val entry = _state.value.find(id) ?: return@busy
        output.use { repository.download(usable(entry), it) }
        _state.update { it.copy(notice = "Saved a decrypted copy of ${entry.name}.") }
    }

    suspend fun openCopy(id: String): Pair<File?, String?> {
        val entry = _state.value.find(id) ?: return null to null
        return try {
            val file = repository.downloadToCache(usable(entry))
            synchronized(openedCopies) { openedCopies.getOrPut(entry.id) { mutableListOf() }.add(file) }
            file to null
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (e is ApiException && e.status == 401) onSessionExpired()
            null to (e.message ?: "Couldn't open it. Try again.")
        }
    }

    fun newFolder(name: String, parent: String) = launchAction { entry ->
        val folder = repository.createFolder(name, parent)
        _state.update { s -> s.copy(snapshot = s.snapshot.copy(entries = s.snapshot.entries + folder)) }
        entry
    }

    fun rename(id: String, name: String) = applyToVersions(id, null) { it.copy(name = name.trim()) }

    fun move(id: String, folderId: String) = applyToVersions(id, "Moved.") { it.copy(parent = folderId) }

    fun trash(id: String) = applyToVersions(id, "Moved to Trash. It still counts toward your storage until you empty the Trash.") {
        it.copy(trashed = true, trashedMs = System.currentTimeMillis())
    }

    fun restore(id: String) = applyToVersions(id, "Restored.") { e ->
        val parentGone = e.parent.isNotEmpty() && _state.value.find(e.parent) == null
        e.copy(trashed = false, trashedMs = 0, parent = if (parentGone) "" else e.parent)
    }

    /** Deletes for good: a file with all its versions, a folder with everything in it. */
    fun deleteForever(id: String) {
        val s = _state.value
        val e = s.find(id) ?: return
        if (e.incoming) return
        val doomed = if (e.isFile) {
            s.snapshot.entries.filter { it.isFile && it.fileId == e.fileId }
        } else {
            val folders = mutableListOf(e.id)
            val out = mutableListOf(e)
            var i = 0
            while (i < folders.size) {
                for (child in s.snapshot.entries) {
                    if (child.parent != folders[i]) continue
                    out += child
                    if (!child.isFile) folders += child.id
                }
                i++
            }
            out
        }
        launchAction { _ ->
            doomed.forEach { repository.remove(it) }
            val ids = doomed.map { it.id }.toSet()
            _state.update { st -> st.copy(snapshot = st.snapshot.copy(entries = st.snapshot.entries.filterNot { it.id in ids }), notice = "Deleted for good.") }
            refresh()
        }
    }

    fun emptyTrash() {
        val s = _state.value
        s.snapshot.entries.filter { it.trashed && (!it.isFile || s.currentFiles.contains(it)) }.forEach { deleteForever(it.id) }
    }

    fun shareWith(id: String, userId: String, handle: String) = launchAction { _ ->
        val entry = _state.value.find(id) ?: return@launchAction
        repository.share(usable(entry), userId)
        _state.update { s ->
            s.copy(
                snapshot = s.snapshot.copy(entries = s.snapshot.entries.map { if (it.id == id && userId !in it.sharedWith) it.copy(sharedWith = it.sharedWith + userId) else it }),
                notice = "Shared with $handle. Only they can open it.",
            )
        }
    }

    fun unshare(id: String, userId: String) = launchAction { _ ->
        val entry = _state.value.find(id) ?: return@launchAction
        repository.unshare(entry, userId)
        _state.update { s -> s.copy(snapshot = s.snapshot.copy(entries = s.snapshot.entries.map { if (it.id == id) it.copy(sharedWith = it.sharedWith - userId) else it })) }
    }

    /** Creates a share link; returns its URL, or null with the error set. */
    suspend fun createLink(id: String, options: DriveRepository.LinkOptions): String? {
        var url: String? = null
        val error = busy {
            val entry = _state.value.find(id) ?: return@busy
            val (link, saved) = repository.createLink(usable(entry), options)
            url = link
            _state.update { s ->
                s.copy(snapshot = s.snapshot.copy(entries = s.snapshot.entries.map { if (it.id == saved.id) saved else it }), notice = "Link created and copied. Send the password separately.")
            }
            refresh()
        }
        if (error != null) _state.update { it.copy(error = error) }
        return url
    }

    fun linkUrl(id: String, linkId: String): String? = _state.value.find(id)?.let { repository.linkUrl(it, linkId) }

    fun deleteLink(id: String, linkId: String) = launchAction { _ ->
        val entry = _state.value.find(id) ?: return@launchAction
        val saved = repository.deleteLink(entry, linkId)
        _state.update { s -> s.copy(snapshot = s.snapshot.copy(entries = s.snapshot.entries.map { if (it.id == saved.id) saved else it }), notice = "Link removed. It no longer works.") }
        refresh()
    }

    // --- Encryption keys ---

    fun refreshKeys() {
        scope.launch {
            guarded {
                val list = keyRepository.list()
                openedKeys.keys.retainAll(list.map { it.id }.toSet())
                _state.update { it.copy(keys = list, keysLoaded = true, openedKeys = openedKeys.keys.toSet()) }
            }
        }
    }

    suspend fun createKey(name: String, password: String): String? = busy {
        val key = keyRepository.create(name.trim(), password)
        _state.update { it.copy(keys = it.keys + key, notice = "Key created. Export a copy for backup and keep it offline.") }
    }

    /** Checks a pasted key as it's typed. */
    fun checkImport(text: String): ImportCheck {
        val compact = text.filterNot { it.isWhitespace() }
        if (compact.isEmpty()) return ImportCheck.Empty
        val hex = compact.filterNot { it == '-' || it == ':' }
        val looksHex = hex.all { it in '0'..'9' || it.lowercaseChar() in 'a'..'f' }
        val key = drive.parseFileLockKey(text)
            ?: return ImportCheck.Invalid(
                if (looksHex) "That's ${hex.length * 4} bits. A key must be exactly 256 bits (64 hex characters)."
                else {
                    val bytes = runCatching { java.util.Base64.getDecoder().decode(compact.replace('-', '+').replace('_', '/')) }.getOrNull()?.size ?: 0
                    if (bytes > 0) "That's ${bytes * 8} bits. A key must be exactly 256 bits (64 hex characters)."
                    else "That isn't a key. Paste 64 hex characters, or the base64 or base64url form of a 256-bit key."
                },
            )
        val fingerprint = drive.fileLockKeyFingerprint(key)
        key.fill(0)
        _state.value.keys.firstOrNull { it.fingerprint == fingerprint }?.let { return ImportCheck.Duplicate(it.name, fingerprint) }
        return ImportCheck.Valid(
            when {
                looksHex -> "hex"
                compact.contains('-') || compact.contains('_') -> "base64url"
                else -> "base64"
            },
            fingerprint,
        )
    }

    suspend fun importKey(name: String, text: String, password: String): String? = busy {
        val key = keyRepository.import(name.trim(), text, password)
        _state.update { it.copy(keys = it.keys + key, notice = "Key imported. Files locked with it can be unlocked here now.") }
    }

    fun renameKey(id: String, name: String) = launchAction { _ ->
        val key = _state.value.key(id) ?: return@launchAction
        val renamed = keyRepository.rename(key, name)
        _state.update { s -> s.copy(keys = s.keys.map { if (it.id == id) renamed else it }) }
    }

    suspend fun setKeyPassword(id: String, current: String, next: String): String? = busy {
        val key = _state.value.key(id) ?: throw UserFacingException("That key isn't there anymore.")
        if (next.isNotEmpty()) locks.passwordProblem(next)?.let { throw UserFacingException(it) }
        val updated = keyRepository.setPassword(key, current, next)
        _state.update { s ->
            s.copy(
                keys = s.keys.map { if (it.id == id) updated else it },
                notice = when {
                    next.isEmpty() -> "Password removed. Your account alone protects this key now."
                    key.passwordProtected -> "Password changed. Locked files weren't re-encrypted."
                    else -> "Password set. Using this key now needs it too."
                },
            )
        }
    }

    /** The key as 64 hex characters in groups of 8, for backup. Throws [UserFacingException]. */
    suspend fun exportKey(id: String, password: String): String {
        val key = _state.value.key(id) ?: throw UserFacingException("That key isn't there anymore.")
        val secret = openedKeys[id] ?: keyRepository.open(key, password).also {
            openedKeys[id] = it
            _state.update { s -> s.copy(openedKeys = openedKeys.keys.toSet()) }
        }
        return drive.fileLockKeyHex(secret).chunked(8).joinToString(" ")
    }

    fun deleteKey(id: String) = launchAction { _ ->
        val key = _state.value.key(id) ?: return@launchAction
        keyRepository.remove(key)
        openedKeys.remove(id)?.fill(0)
        _state.update { s -> s.copy(keys = s.keys.filterNot { it.id == id }, openedKeys = openedKeys.keys.toSet(), notice = "Key deleted.") }
    }

    // --- Plumbing ---

    private fun applyToVersions(id: String, notice: String?, change: (DriveEntry) -> DriveEntry) {
        val s = _state.value
        val e = s.find(id) ?: return
        if (e.incoming) return
        val targets = s.snapshot.entries.filter { it.id == e.id || (e.isFile && it.isFile && it.fileId == e.fileId) }
        launchAction { _ ->
            for (target in targets) {
                val saved = repository.save(change(target))
                _state.update { st -> st.copy(snapshot = st.snapshot.copy(entries = st.snapshot.entries.map { if (it.id == saved.id) saved else it })) }
            }
            if (notice != null) _state.update { it.copy(notice = notice) }
        }
    }

    private fun <T> launchAction(block: suspend (Unit) -> T) {
        scope.launch { guarded { block(Unit) } }
    }

    /** Runs [block], turning failures into a message; returns it, or null on success. Marks the session busy meanwhile. */
    private suspend fun busy(block: suspend () -> Unit): String? {
        _state.update { it.copy(busy = true, notice = null, error = null) } // a new action replaces the last one's message
        return try {
            block()
            null
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            e.message ?: "Something went wrong. Try again."
        } catch (e: UserFacingException) {
            e.message
        } catch (e: Exception) {
            "Something went wrong. Try again."
        } finally {
            _state.update { it.copy(busy = false) }
        }
    }

    private suspend fun guarded(block: suspend () -> Unit) {
        try {
            block()
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) return onSessionExpired()
            _state.update { it.copy(error = e.message) }
        } catch (e: Exception) {
            _state.update { it.copy(error = e.message ?: "Something went wrong. Try again.") }
        }
    }

    /** Ends the session: forgets every unlocked file key and opened encryption key, and removes decrypted copies. */
    fun close() {
        scope.cancel()
        unlockedKeys.clear()
        synchronized(openedCopies) { openedCopies.clear() }
        openedKeys.values.forEach { it.fill(0) }
        openedKeys.clear()
        repository.clearCache()
    }
}

sealed interface ImportCheck {
    data object Empty : ImportCheck
    data class Invalid(val message: String) : ImportCheck
    data class Duplicate(val name: String, val fingerprint: String) : ImportCheck
    data class Valid(val format: String, val fingerprint: String) : ImportCheck
}

private const val PAUSED = "Paused · resumes when Tor connects"
