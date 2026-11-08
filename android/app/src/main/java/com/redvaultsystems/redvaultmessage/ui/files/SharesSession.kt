// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import com.redvaultsystems.redvaultmessage.data.DriveEntry
import com.redvaultsystems.redvaultmessage.data.IncomingFile
import com.redvaultsystems.redvaultmessage.data.OwnerShare
import com.redvaultsystems.redvaultmessage.data.ShareOptions
import com.redvaultsystems.redvaultmessage.data.ShareRepository
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
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
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.OutputStream
import java.time.Instant

data class SharesState(
    val loaded: Boolean = false,
    val links: List<OwnerShare> = emptyList(),
    val incoming: List<IncomingFile> = emptyList(),
    /** A link operation is running (Argon2id for a password takes a moment). */
    val busy: Boolean = false,
    val error: String? = null,
    val notice: String? = null,
    /** The address of the link just made (or remade), to show and copy. */
    val madeUrl: String? = null,
    val madeHasPassword: Boolean = false,
    val madePermission: String = "viewer",
) {
    val activeLinks: Int get() = links.count { it.active }
    fun linksFor(itemId: String): List<OwnerShare> = links.filter { it.link.backupId == itemId }
    fun link(id: String): OwnerShare? = links.firstOrNull { it.link.id == id }
    fun incomingFile(id: String): IncomingFile? = incoming.firstOrNull { it.dto.id == id }
}

/**
 * Public share links for Files (docs/public-share-links.md) on this phone:
 * links anyone can open in a browser, to a file or a folder, view-only or
 * taking uploads; and the files people upload through them, waiting in
 * Incoming for review. Folder links follow Files: after each Files refresh
 * the links' items are brought in line (new files, renames, moves, the Trash).
 */
class SharesSession(
    private val repository: ShareRepository,
    private val files: FilesSession,
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(SharesState())
    val state: StateFlow<SharesState> = _state.asStateFlow()
    private val reconciling = Mutex()
    private var lastSignature = ""

    init {
        // Folder links follow Files.
        scope.launch {
            files.state.collect { s -> if (s.loaded) reconcileSoon(s.snapshot.entries) }
        }
    }

    /** Refreshes until cancelled. Run it while the app is on screen. */
    suspend fun poll() {
        while (true) {
            refreshNow()
            delay(60_000)
        }
    }

    fun refresh() {
        scope.launch { refreshNow() }
    }

    suspend fun refreshNow() {
        try {
            val listing = repository.refresh()
            _state.update { it.copy(loaded = true, links = listing.links, incoming = listing.incoming) }
            reconcileSoon(files.state.value.snapshot.entries)
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
        } catch (e: Exception) {
            // Shown on the next try; the badge keeps its last count.
        }
    }

    private fun reconcileSoon(entries: List<DriveEntry>) {
        val live = _state.value.links.filter { it.active && it.box != null }
        if (live.isEmpty()) return
        val signature = entries.joinToString("\n") { e ->
            "${e.id}|${e.name}|${e.parent}|${e.trashed}|${e.version}|${e.modifiedMs}|${e.meta.containsKey("key")}"
        } + live.joinToString(";") { "${it.link.id}:${it.link.keyEpoch}" }
        if (signature == lastSignature) return
        lastSignature = signature
        scope.launch {
            if (!reconciling.tryLock()) return@launch
            try {
                delay(1_500)
                repository.reconcileAll(_state.value.links, files.state.value.snapshot.entries)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
            } catch (e: Exception) {
                lastSignature = "" // try again next time
            } finally {
                reconciling.unlock()
            }
        }
    }

    fun dismissMessages() = _state.update { it.copy(error = null, notice = null) }

    fun clearMade() = _state.update { it.copy(madeUrl = null) }

    private fun run(notice: String? = null, work: suspend () -> Unit) {
        _state.update { it.copy(busy = true, error = null, notice = null) }
        scope.launch {
            try {
                work()
                _state.update { it.copy(busy = false, notice = notice ?: it.notice) }
                refreshNow()
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                _state.update { it.copy(busy = false) }
                if (e.status == 401) onSessionExpired() else _state.update { it.copy(error = e.message ?: "Something went wrong. Try again.") }
            } catch (e: UserFacingException) {
                _state.update { it.copy(busy = false, error = e.message) }
            } catch (e: Exception) {
                _state.update { it.copy(busy = false, error = "Something went wrong. Try again.") }
            }
        }
    }

    fun createLink(item: DriveEntry, options: ShareOptions) {
        if (options.password.isNotEmpty() && options.password.length < 8) {
            _state.update { it.copy(error = "Use a password of at least 8 characters.") }
            return
        }
        if (options.expiresAtMs != null && options.expiresAtMs <= System.currentTimeMillis()) {
            _state.update { it.copy(error = "Pick an expiry in the future.") }
            return
        }
        run {
            val made = repository.create(item, files.state.value.snapshot.entries, options)
            _state.update { it.copy(madeUrl = made.url, madeHasPassword = made.link.password != null, madePermission = made.link.permission) }
        }
    }

    /**
     * Saves a link's settings. [expiresAtMs]: null keeps it, 0 means never.
     * The limits are null for "no limit".
     */
    fun saveSettings(
        linkId: String,
        label: String?,
        permission: String,
        expiresAtMs: Long?,
        maxDownloads: Long?,
        maxFileSize: Long?,
        allowance: Long?,
        maxFiles: Long?,
    ) {
        val share = _state.value.link(linkId) ?: return
        if (expiresAtMs != null && expiresAtMs != 0L && expiresAtMs <= System.currentTimeMillis()) {
            _state.update { it.copy(error = "Pick an expiry in the future.") }
            return
        }
        val body = buildJsonObject {
            put("permission", permission)
            if (expiresAtMs != null) put("expiresAt", if (expiresAtMs == 0L) JsonNull else JsonPrimitive(Instant.ofEpochMilli(expiresAtMs).toString()))
            put("maxDownloads", maxDownloads?.let { JsonPrimitive(it) } ?: JsonNull)
            if (permission == "contributor") {
                put(
                    "upload",
                    buildJsonObject {
                        put("maxFileSize", maxFileSize?.let { JsonPrimitive(it) } ?: JsonNull)
                        put("allowance", allowance?.let { JsonPrimitive(it) } ?: JsonNull)
                        put("maxFiles", maxFiles?.let { JsonPrimitive(it) } ?: JsonNull)
                    },
                )
            }
        }
        run("Saved.") { repository.updateSettings(share, body, label?.trim()) }
    }

    fun turnOffUploads(linkId: String) {
        val share = _state.value.link(linkId) ?: return
        run("Uploads turned off.") { repository.updateSettings(share, JsonObject(mapOf("permission" to JsonPrimitive("viewer"))), null) }
    }

    fun setPassword(linkId: String, password: String) {
        val share = _state.value.link(linkId) ?: return
        if (password.isNotEmpty() && password.length < 8) {
            _state.update { it.copy(error = "Use a password of at least 8 characters.") }
            return
        }
        run(if (password.isEmpty()) "Password removed." else "Password changed.") { repository.setPassword(share, password) }
    }

    fun regenerate(linkId: String, password: String) {
        val share = _state.value.link(linkId) ?: return
        if (password.isNotEmpty() && password.length < 8) {
            _state.update { it.copy(error = "Use a password of at least 8 characters.") }
            return
        }
        run {
            val made = repository.regenerate(share, password, files.state.value.snapshot.entries)
            _state.update { it.copy(madeUrl = made.url, madeHasPassword = made.link.password != null, madePermission = made.link.permission) }
        }
    }

    fun revoke(linkId: String) {
        val share = _state.value.link(linkId) ?: return
        run("Link revoked. It stopped working for everyone.") { repository.revoke(share) }
    }

    fun remove(linkId: String) {
        val share = _state.value.link(linkId) ?: return
        run("Link deleted.") { repository.remove(share) }
    }

    /** Decrypts an incoming file into [output] (a file the person chose). */
    fun downloadIncoming(id: String, output: () -> OutputStream?) {
        val file = _state.value.incomingFile(id) ?: return
        run("Saved. Someone anonymous uploaded it: only open it if you expected it.") {
            val stream = output() ?: throw UserFacingException("Couldn't save the file there.")
            stream.use { repository.downloadIncoming(file, it) }
        }
    }

    fun acceptIncoming(id: String, folderId: String) {
        val file = _state.value.incomingFile(id) ?: return
        run("“${file.name}” was added to your files.") {
            repository.acceptIncoming(file, folderId)
            files.refreshNow()
        }
    }

    fun rejectIncoming(id: String) {
        val file = _state.value.incomingFile(id) ?: return
        run("Rejected. It's deleted, and its space is free again.") {
            repository.rejectIncoming(file)
            files.refreshNow()
        }
    }

    fun close() {
        scope.cancel()
    }

    companion object {
        // The desktop's list (model::file_message::opensInertly): pictures, audio, video, text and PDF. Not svg,
        // html or xml, which are really programs in a viewer. A deny-list missed types and names like "a.apk ".
        private val INERT = setOf(
            "jpg", "jpeg", "png", "gif", "webp", "bmp", "heic", "avif",
            "mp3", "m4a", "aac", "ogg", "oga", "opus", "wav", "flac",
            "mp4", "m4v", "mov", "webm", "mkv",
            "txt", "pdf",
        )

        /** Anything but an inert type gets a warning before saving a file someone anonymous uploaded. */
        fun isRisky(name: String): Boolean {
            val clean = FileMessage.sanitizeFilename(name)
            val dot = clean.lastIndexOf('.')
            return dot <= 0 || clean.substring(dot + 1).lowercase() !in INERT
        }
    }
}
