// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import android.media.MediaPlayer
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.data.FileDownloadStage
import com.redvaultsystems.redvaultmessage.data.FileSendStage
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch
import java.io.File

typealias DownloadFile = suspend (FileDescriptor, onStage: (FileDownloadStage) -> Unit) -> File

private sealed interface DownloadState {
    data object Idle : DownloadState
    data class Busy(val stage: FileDownloadStage) : DownloadState
    data class Ready(val file: File) : DownloadState
    data class Failed(val message: String) : DownloadState
}

/** Port of ui/src/components/FileBubble.tsx, with Open and Save instead of a browser download. */
@Composable
fun FileBubbleContent(message: Message, foreground: Color, muted: Color, onDownload: DownloadFile, showIcon: Boolean = true) {
    val descriptor = message.file ?: return
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var state by remember(message.id) { mutableStateOf<DownloadState>(DownloadState.Idle) }
    val unavailable = when (message.fileStatus) {
        "expired" -> "File expired."
        "deleted" -> "File deleted."
        "failed" -> "This file failed to upload."
        else -> null
    }

    val appLock = LocalAppLock.current
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument(descriptor.mimeType)) { target ->
        val ready = state as? DownloadState.Ready ?: return@rememberLauncherForActivityResult
        if (target == null) return@rememberLauncherForActivityResult
        runCatching {
            context.contentResolver.openOutputStream(target)?.use { out -> ready.file.inputStream().use { it.copyTo(out) } }
        }.onFailure { state = DownloadState.Failed("Couldn't save the file.") }
    }

    if (descriptor.mimeType.startsWith("audio/") && unavailable == null) {
        VoiceBubbleContent(message, descriptor, foreground, muted, onDownload)
        return
    }

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (showIcon) Icon(painterResource(R.drawable.ic_file), contentDescription = null, tint = foreground, modifier = Modifier.size(18.dp))
            Text(descriptor.filename, color = foreground, fontWeight = FontWeight.Medium, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
        Text(FileMessage.formatBytes(descriptor.originalSize), color = muted, style = MaterialTheme.typography.labelSmall)
        when {
            unavailable != null -> Text(unavailable, color = muted, style = MaterialTheme.typography.labelSmall)
            else -> when (val current = state) {
                DownloadState.Idle -> FileAction("Download", foreground) {
                    state = DownloadState.Busy(FileDownloadStage.Downloading)
                    scope.launch {
                        state = try {
                            DownloadState.Ready(onDownload(descriptor) { stage -> state = DownloadState.Busy(stage) })
                        } catch (e: UserFacingException) {
                            DownloadState.Failed(e.message ?: "Download failed. Try again.")
                        } catch (e: Exception) {
                            DownloadState.Failed("Download failed. Try again.")
                        }
                    }
                }
                is DownloadState.Busy -> Text(
                    if (current.stage == FileDownloadStage.Downloading) "Downloading…" else "Decrypting…",
                    color = muted,
                    fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.labelSmall,
                )
                is DownloadState.Ready -> Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    FileAction("Open", foreground) { openFile(context, current.file, descriptor.mimeType) { state = DownloadState.Failed(it) } }
                    FileAction("Save", foreground) {
                        appLock?.expectExternalReturn()
                        save.launch(descriptor.filename)
                    }
                }
                is DownloadState.Failed -> Column {
                    Text(current.message, color = foreground, style = MaterialTheme.typography.labelSmall)
                    FileAction("Retry", foreground) { state = DownloadState.Idle }
                }
            }
        }
    }
}

/** A voice message: downloads and decrypts on first Play, then plays from the app's private cache. */
@Composable
private fun VoiceBubbleContent(message: Message, descriptor: FileDescriptor, foreground: Color, muted: Color, onDownload: DownloadFile) {
    val scope = rememberCoroutineScope()
    var file by remember(message.id) { mutableStateOf<File?>(null) }
    var busy by remember(message.id) { mutableStateOf<FileDownloadStage?>(null) }
    var error by remember(message.id) { mutableStateOf<String?>(null) }
    var playing by remember(message.id) { mutableStateOf(false) }
    val player = remember(message.id) { MediaPlayer() }
    var prepared by remember(message.id) { mutableStateOf(false) }
    DisposableEffect(message.id) { onDispose { player.release() } }

    fun startPlaying(source: File) {
        try {
            if (!prepared) {
                player.setDataSource(source.absolutePath)
                player.setOnCompletionListener { playing = false }
                player.prepare()
                prepared = true
            }
            player.start()
            playing = true
        } catch (e: Exception) {
            error = "Couldn't play this voice message."
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FileAction(if (playing) "❚❚ Pause" else "▶ Play", foreground) {
                val ready = file
                when {
                    playing -> {
                        player.pause()
                        playing = false
                    }
                    ready != null -> startPlaying(ready)
                    busy == null -> {
                        busy = FileDownloadStage.Downloading
                        error = null
                        scope.launch {
                            try {
                                val downloaded = onDownload(descriptor) { busy = it }
                                file = downloaded
                                startPlaying(downloaded)
                            } catch (e: UserFacingException) {
                                error = e.message
                            } catch (e: Exception) {
                                error = "Couldn't load this voice message."
                            } finally {
                                busy = null
                            }
                        }
                    }
                }
            }
            Text(
                when (busy) {
                    FileDownloadStage.Downloading -> "Downloading…"
                    FileDownloadStage.Decrypting -> "Decrypting…"
                    null -> "🎤 Voice message · ${FileMessage.formatBytes(descriptor.originalSize)}"
                },
                color = muted,
                style = MaterialTheme.typography.labelSmall,
            )
        }
        error?.let { Text(it, color = foreground, style = MaterialTheme.typography.labelSmall) }
    }
}

@Composable
private fun FileAction(label: String, color: Color, onClick: () -> Unit) {
    TextButton(onClick = onClick, contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 8.dp, vertical = 0.dp)) {
        Text(label, color = color, fontWeight = FontWeight.SemiBold)
    }
}

/** Hands the decrypted file to an app the user picks, with a one-time read grant. */
internal fun openFile(context: Context, file: File, mimeType: String, onError: (String) -> Unit) {
    val uri = FileProvider.getUriForFile(context, "${context.packageName}.files", file)
    val view = Intent(Intent.ACTION_VIEW).setDataAndType(uri, mimeType).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    try {
        context.startActivity(Intent.createChooser(view, "Open with").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
    } catch (_: ActivityNotFoundException) {
        onError("No app on this phone can open this file. Use Save instead.")
    }
}

/**
 * A file waiting in the message box: picked or shared from another app ([uri], read when it's sent), or
 * a picture pasted into the box ([bytes], held in memory only; [mimeType] says what it is).
 */
class PendingFile(val uri: Uri?, val name: String, val size: Long?, val mimeType: String? = null, val bytes: ByteArray? = null)

fun describePicked(context: Context, uri: Uri): PendingFile {
    context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
        if (c.moveToFirst()) {
            val name = c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 }?.let(c::getString)
            val size = c.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 && !c.isNull(it) }?.let(c::getLong)
            return PendingFile(uri, FileMessage.sanitizeFilename(name ?: "file"), size)
        }
    }
    return PendingFile(uri, "file", null)
}

/**
 * Port of the web thread's attachment bar, for one file or several (the desktop's "Send N files?"):
 * names, sizes, stage, errors, Send / Cancel. Several are sent one after another, in the order shown.
 */
@Composable
fun AttachmentBar(
    pending: List<PendingFile>,
    stage: FileSendStage?,
    /** While several are going out: which one (from 1) of how many. */
    progress: Pair<Int, Int>?,
    sent: Boolean,
    error: String?,
    onSend: () -> Unit,
    onCancel: () -> Unit,
    onRemove: (PendingFile) -> Unit,
) {
    if (pending.isEmpty() && error == null && !sent) return
    val colors = RedvaultTheme.colors
    Column(
        modifier = Modifier.fillMaxWidth().background(colors.surface2).padding(horizontal = 16.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        if (pending.isNotEmpty()) {
            Column(Modifier.heightIn(max = 168.dp).verticalScroll(rememberScrollState())) {
                pending.forEach { file ->
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            file.name + (file.size?.let { " · " + FileMessage.formatBytes(it) } ?: ""),
                            color = colors.text,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = MaterialTheme.typography.bodyMedium,
                            modifier = Modifier.weight(1f),
                        )
                        if (pending.size > 1 && stage == null) {
                            IconButton(onClick = { onRemove(file) }, modifier = Modifier.size(32.dp)) {
                                Icon(painterResource(R.drawable.ic_close), contentDescription = "Remove ${file.name}", tint = colors.text2, modifier = Modifier.size(16.dp))
                            }
                        }
                    }
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Column(Modifier.weight(1f)) {
                val of = progress?.takeIf { it.second > 1 }?.let { " ${it.first} of ${it.second}" }.orEmpty()
                val status = when {
                    sent -> "Sent"
                    stage == FileSendStage.Encrypting -> "Encrypting$of…"
                    stage == FileSendStage.Uploading -> "Uploading$of…"
                    stage == FileSendStage.Sending -> "Sending$of…"
                    pending.size > 1 -> "Each file is encrypted on this phone before it's uploaded."
                    else -> null
                }
                status?.let {
                    Text(
                        it,
                        color = if (stage == null && !sent) colors.text3 else colors.accent,
                        fontFamily = if (stage == null && !sent) null else FontFamily.Monospace,
                        style = MaterialTheme.typography.labelSmall,
                    )
                }
                error?.let { Text(it, color = colors.danger, style = MaterialTheme.typography.labelSmall) }
            }
            if (pending.isNotEmpty() && stage == null) {
                TextButton(onClick = onCancel) { Text("Cancel", color = colors.text2) }
                TextButton(onClick = onSend) {
                    Text(
                        when {
                            error != null -> "Retry"
                            pending.size > 1 -> "Send ${pending.size} files"
                            else -> "Send file"
                        },
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
        }
    }
}
