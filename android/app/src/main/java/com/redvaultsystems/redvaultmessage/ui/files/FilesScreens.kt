// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import androidx.compose.ui.res.stringResource
import com.redvaultsystems.redvaultmessage.ui.components.ActionSheet
import com.redvaultsystems.redvaultmessage.ui.components.SheetAction
import androidx.compose.ui.res.painterResource
import androidx.compose.material3.Icon
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.DriveEntry
import com.redvaultsystems.redvaultmessage.data.DriveRepository
import com.redvaultsystems.redvaultmessage.data.FileLocks
import com.redvaultsystems.redvaultmessage.ui.TimeFormat
import com.redvaultsystems.redvaultmessage.ui.chats.openFile
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.DsSwitch
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.components.TabHeader
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

/** A person you can share with (from your chats). */
data class ShareContact(val userId: String, val handle: String, val avatarUrl: String?)

private enum class FilesFilter(val label: String) { All("All"), Locked("Locked"), Photos("Photos"), Shared("Shared"), Trash("Trash") }

/** The Files tab: storage, search, filters, encryption keys, and the files and folders here. */
@Composable
fun FilesTab(
    files: FilesSession,
    state: FilesState,
    onOpenFile: (String) -> Unit,
    onOpenKeys: () -> Unit,
    onUpload: (List<Uri>, String) -> Unit,
    /** Files waiting in Incoming (uploaded through public share links). */
    incomingCount: Int = 0,
    activeLinks: Int = 0,
    onShareLink: (String) -> Unit = {},
    onOpenLinks: () -> Unit = {},
    onOpenIncoming: () -> Unit = {},
) {
    val colors = RedvaultTheme.colors
    var query by rememberSaveable { mutableStateOf("") }
    var filter by rememberSaveable { mutableStateOf(FilesFilter.All) }
    var folderId by rememberSaveable { mutableStateOf("") }
    var menuFor by remember { mutableStateOf<DriveEntry?>(null) }
    var naming by remember { mutableStateOf<Pair<String, String>?>(null) } // (mode, entry id)
    var moving by remember { mutableStateOf<DriveEntry?>(null) }
    var overflow by remember { mutableStateOf(false) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) onUpload(uris, if (filter == FilesFilter.All) folderId else "")
    }
    // "Upload a new version…": one picked file becomes the next version of the file the menu was opened on.
    var versionFor by remember { mutableStateOf<DriveEntry?>(null) }
    val versionPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val target = versionFor
        versionFor = null
        if (uri != null && target != null) files.uploadNewVersion(uri, target)
    }

    val current = state.currentFiles
    val q = query.trim()
    val folder = state.find(folderId)
    val folders = if (filter == FilesFilter.All && q.isEmpty()) {
        state.snapshot.entries.filter { !it.isFile && !it.trashed && it.parent == folderId }.sortedBy { it.name.lowercase() }
    } else emptyList()
    val shown = when (filter) {
        FilesFilter.All -> if (q.isNotEmpty()) current.filter { !it.trashed } + state.snapshot.incoming else current.filter { !it.trashed && it.parent == folderId }
        FilesFilter.Locked -> current.filter { !it.trashed && it.locked }
        FilesFilter.Photos -> current.filter { !it.trashed && it.mimeType.startsWith("image/") }
        FilesFilter.Shared -> state.snapshot.incoming
        FilesFilter.Trash -> state.snapshot.entries.filter { it.trashed && (!it.isFile || it in current) }
    }.filter { q.isEmpty() || it.name.contains(q, ignoreCase = true) }
        .let { list -> if (filter == FilesFilter.Trash) list.sortedByDescending { it.modifiedMs } else list.sortedByDescending { it.modifiedMs } }
    val uploads = if (filter == FilesFilter.All && q.isEmpty()) state.uploads.filter { it.parent == folderId } else emptyList()

    Column(Modifier.fillMaxSize()) {
        TabHeader(stringResource(R.string.ui_files)) {
            RoundButton(R.drawable.ui_key, "Encryption keys", onClick = onOpenKeys)
            Box {
                RoundButton(R.drawable.ui_dots, "More", onClick = { overflow = true })
                DropdownMenu(expanded = overflow, onDismissRequest = { overflow = false }, containerColor = colors.surface) {
                    DropdownMenuItem(text = { Text(stringResource(R.string.ui_new_folder), color = colors.text) }, onClick = { overflow = false; naming = "folder" to "" })
                    DropdownMenuItem(text = { Text(if (activeLinks > 0) "Shared links · $activeLinks" else "Shared links", color = colors.text) }, onClick = { overflow = false; onOpenLinks() })
                    DropdownMenuItem(text = { Text(if (incomingCount > 0) "Incoming · $incomingCount" else "Incoming", color = colors.text) }, onClick = { overflow = false; onOpenIncoming() })
                    if (filter == FilesFilter.All && folderId.isNotEmpty() && folder != null) {
                        DropdownMenuItem(text = { Text(stringResource(R.string.ui_share_this_folder), color = colors.text) }, onClick = { overflow = false; onShareLink(folderId) })
                    }
                    if (filter == FilesFilter.Trash) DropdownMenuItem(text = { Text(stringResource(R.string.ui_empty_trash), color = colors.danger) }, onClick = { overflow = false; files.emptyTrash() })
                }
            }
            RoundButton(R.drawable.ui_upload, "Upload", onClick = { picker.launch(arrayOf("*/*")) }, container = colors.accentFill, tint = colors.accentContrast)
        }
        LazyColumn(Modifier.fillMaxSize()) {
            item { StorageCard(state) }
            item { SearchBox(query, { query = it }, "Search your files", R.drawable.ui_search, Modifier.padding(horizontal = 20.dp, vertical = 12.dp)) }
            item {
                ChipRow(
                    options = FilesFilter.entries.toList(),
                    selected = filter,
                    label = { if (it == FilesFilter.Locked) "Locked · ${state.lockedCount}" else it.label },
                    onSelect = { filter = it; folderId = "" },
                    lockOption = FilesFilter.Locked,
                    lockIcon = R.drawable.ui_lock,
                )
            }
            item { KeysCard(state.keys.size, onOpenKeys) }
            if (incomingCount > 0) {
                item {
                    Callout(
                        if (incomingCount == 1) "1 file arrived through your sharing links. Review it in Incoming." else "$incomingCount files arrived through your sharing links. Review them in Incoming.",
                        CalloutTone.Ok,
                        R.drawable.ui_download,
                        Modifier.padding(horizontal = 20.dp, vertical = 6.dp).clickable(onClick = onOpenIncoming),
                    )
                }
            }
            state.error?.let { error ->
                item {
                    Callout(error, CalloutTone.Error, R.drawable.ui_alert_circle, Modifier.padding(horizontal = 20.dp, vertical = 6.dp).clickable { files.dismissMessages() })
                }
            }
            state.notice?.let { notice ->
                item { Callout(notice, CalloutTone.Ok, R.drawable.ui_check, Modifier.padding(horizontal = 20.dp, vertical = 6.dp).clickable { files.dismissMessages() }) }
            }
            if (folder != null && filter == FilesFilter.All && q.isEmpty()) {
                item {
                    Row(
                        Modifier.fillMaxWidth().clickable { folderId = folder.parent }.padding(horizontal = 20.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        DsIcon(R.drawable.ui_chevron_left, colors.accent, 18.dp, description = stringResource(R.string.ui_up_one_folder))
                        Text(folder.name, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                    }
                }
            }
            if (folders.isNotEmpty()) {
                item { SectionCaps(stringResource(R.string.ui_folders)) }
                items(folders, key = { it.id }) { f ->
                    FileRow(f, unlocked = false, subtitle = folderSubtitle(state, f), onClick = { folderId = f.id }, onLongClick = { menuFor = f })
                }
            }
            item {
                SectionCaps(
                    when {
                        q.isNotEmpty() -> "Results"
                        filter == FilesFilter.All && folder == null -> "Recent"
                        filter == FilesFilter.All -> "Files"
                        else -> filter.label
                    },
                )
            }
            items(uploads, key = { it.localId }) { upload -> UploadRow(upload, onCancel = { files.cancelUpload(upload.localId) }) }
            if (!state.loaded) {
                item { Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator(color = colors.accent) } }
            } else if (shown.isEmpty() && uploads.isEmpty() && folders.isEmpty()) {
                item {
                    Text(
                        when {
                            q.isNotEmpty() -> "Nothing matches that name."
                            filter == FilesFilter.Trash -> "The Trash is empty."
                            filter == FilesFilter.Shared -> "Files people share with you show up here."
                            filter == FilesFilter.Locked -> "Files with an extra lock (a key or a password) show up here."
                            filter == FilesFilter.Photos -> "Your photos show up here."
                            else -> "Upload files and they're encrypted on this phone before they leave it."
                        },
                        color = colors.text3,
                        textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth().padding(32.dp),
                    )
                }
            }
            items(shown, key = { it.id }) { entry ->
                FileRow(
                    entry,
                    unlocked = entry.id in state.unlocked,
                    subtitle = fileSubtitle(entry),
                    onClick = { if (entry.isFile) onOpenFile(entry.id) else folderId = entry.id },
                    onLongClick = { menuFor = entry },
                )
            }
            item { Spacer(Modifier.height(24.dp)) }
        }
    }

    menuFor?.let { entry ->
        EntryActions(
            entry = entry,
            onDismiss = { menuFor = null },
            onRename = { naming = "rename" to entry.id },
            onMove = { moving = entry },
            onNewVersion = {
                versionFor = entry
                versionPicker.launch(arrayOf("*/*"))
            },
            onTrash = { files.trash(entry.id) },
            onRestore = { files.restore(entry.id) },
            onDelete = { files.deleteForever(entry.id) },
            onShareLink = { onShareLink(entry.id) },
        )
    }
    naming?.let { (mode, id) ->
        NameDialog(
            title = if (mode == "folder") "New folder" else "Rename",
            initial = if (mode == "folder") "" else state.find(id)?.name.orEmpty(),
            confirm = if (mode == "folder") "Create" else "Rename",
            onDismiss = { naming = null },
            onConfirm = { name ->
                if (mode == "folder") files.newFolder(name, if (filter == FilesFilter.All) folderId else "") else files.rename(id, name)
                naming = null
            },
        )
    }
    moving?.let { entry ->
        val targets = listOf("" to "All files") + state.snapshot.entries.filter { !it.isFile && !it.trashed && it.id != entry.id }.map { it.id to it.name }
        AlertDialog(
            onDismissRequest = { moving = null },
            containerColor = colors.surface,
            title = { Text(stringResource(R.string.ui_move_to), color = colors.text) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    targets.forEach { (id, name) ->
                        Text(
                            name,
                            color = colors.text,
                            modifier = Modifier.fillMaxWidth().clickable { files.move(entry.id, id); moving = null }.padding(vertical = 12.dp),
                        )
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { moving = null }) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
        )
    }
}

private fun folderSubtitle(state: FilesState, folder: DriveEntry): String {
    val items = state.snapshot.entries.count { it.parent == folder.id && !it.trashed && (!it.isFile || it in state.currentFiles) }
    return if (items == 1) "1 item" else "$items items"
}

private fun fileSubtitle(entry: DriveEntry): String = buildString {
    if (entry.isFile) append(sizeText(entry.size)).append(" · ")
    append(if (entry.modifiedMs > 0) TimeFormat.listLabel(entry.modifiedMs) else "—")
    if (entry.trashed) append(" · in Trash")
}

@Composable
private fun StorageCard(state: FilesState) {
    val colors = RedvaultTheme.colors
    val usage = state.snapshot.usage
    val fraction = if (usage != null && usage.limit > 0) (usage.used.toFloat() / usage.limit).coerceIn(0f, 1f) else 0f
    val nearlyFull = fraction >= 0.9f
    Column(
        Modifier
            .padding(horizontal = 20.dp, vertical = 8.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(if (nearlyFull) colors.warnSoft else colors.accentSoft)
            .border(1.dp, if (nearlyFull) colors.warn.copy(alpha = 0.35f) else colors.accentBorder, RoundedCornerShape(16.dp))
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            DsIcon(R.drawable.ui_cloud, if (nearlyFull) colors.warn else colors.accent, 22.dp)
            Column(Modifier.weight(1f)) {
                Text(
                    if (usage != null) "${DriveRepository.planName(usage.tier)} · ${sizeText(usage.limit)}" else "Files",
                    color = if (nearlyFull) colors.warn else colors.accent,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(stringResource(R.string.ui_encrypted_and_hashed_on_this_phone), color = colors.text2, fontSize = 13.sp)
            }
            Text(usage?.let { sizeText(it.used) } ?: "", color = colors.text, fontFamily = MonoFamily, fontSize = 13.sp)
        }
        if (usage != null) {
            Box(Modifier.fillMaxWidth().height(4.dp).clip(RoundedCornerShape(2.dp)).background(colors.text.copy(alpha = 0.12f))) {
                Box(
                    Modifier.fillMaxWidth(fraction).height(4.dp).clip(RoundedCornerShape(2.dp))
                        .background(if (fraction >= 1f) colors.danger else if (nearlyFull) colors.warn else colors.accent),
                )
            }
            Text(
                if (fraction >= 1f) "Your storage is full. Delete files or empty the Trash to upload more." else "${sizeText(usage.remaining)} left",
                color = colors.text2,
                fontSize = 12.sp,
            )
            // More space: Pro 50 GB, Premium 100 GB. Offered once storage is getting full.
            if (nearlyFull && usage.tier != "premium") {
                Text(
                    if (usage.tier == "pro") "Get 100 GB with Premium" else "Get more space",
                    color = colors.accent,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .clickable(role = androidx.compose.ui.semantics.Role.Button) {
                            com.redvaultsystems.redvaultmessage.ui.components.PlanGate.ask(
                                if (usage.tier == "pro") "premium" else "pro",
                                "More Files storage",
                            )
                        }
                        .padding(vertical = 4.dp),
                )
            }
        }
    }
}

@Composable
private fun KeysCard(count: Int, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier
            .padding(horizontal = 20.dp, vertical = 12.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(colors.lockBg.copy(alpha = 0.6f))
            .border(1.dp, colors.lockBorder, RoundedCornerShape(16.dp))
            .clickable(role = Role.Button, onClick = onClick)
            .padding(14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Box(Modifier.size(38.dp).clip(RoundedCornerShape(10.dp)).background(colors.lockBg), contentAlignment = Alignment.Center) {
            DsIcon(R.drawable.ui_key, colors.lockText2, 18.dp)
        }
        Column(Modifier.weight(1f)) {
            Text(stringResource(R.string.ui_encryption_keys), color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
            Text(
                (if (count == 1) "1 key" else "$count keys") + " · lock files with your own key",
                color = colors.lockText2,
                fontSize = 13.sp,
            )
        }
        DsIcon(R.drawable.ui_chevron_right, colors.lockText2, 18.dp)
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun FileRow(entry: DriveEntry, unlocked: Boolean, subtitle: String, onClick: () -> Unit, onLongClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier
            .fillMaxWidth()
            .combinedClickable(onClick = onClick, onLongClick = onLongClick, onLongClickLabel = "More actions")
            .heightIn(min = 64.dp)
            .padding(horizontal = 20.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        FileTile(entry.name, !entry.isFile, lockedNow = entry.locked && !unlocked)
        Column(Modifier.weight(1f)) {
            Text(entry.name, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (entry.isFile) LockLine(entry, unlocked, subtitle) else Text(subtitle, color = colors.text3, fontSize = 13.sp)
        }
    }
}

@Composable
private fun UploadRow(upload: UploadItem, onCancel: () -> Unit) {
    val colors = RedvaultTheme.colors
    val percent = (upload.progress * 100).toInt()
    Row(
        Modifier.fillMaxWidth().heightIn(min = 64.dp).padding(horizontal = 20.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        FileTile(upload.name, folder = false, lockedNow = upload.lockKind.isNotEmpty())
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Text(upload.name, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (upload.stage == "failed") {
                Text(upload.error ?: "Didn't upload", color = colors.danger, fontSize = 13.sp, maxLines = 2)
            } else if (upload.stage == "paused") {
                Text(upload.error ?: "Paused", color = colors.warn, fontSize = 13.sp, maxLines = 2)
            } else {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Box(Modifier.weight(1f).height(4.dp).clip(RoundedCornerShape(2.dp)).background(colors.text.copy(alpha = 0.12f))) {
                        Box(Modifier.fillMaxWidth(upload.progress).height(4.dp).clip(RoundedCornerShape(2.dp)).background(colors.warn))
                    }
                    Text(
                        when (upload.stage) {
                            "encrypting" -> "Encrypting · $percent%"
                            "uploading" -> "Uploading · $percent%"
                            else -> "Waiting…"
                        },
                        color = colors.warn,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
        }
        // Stops it; an upload the server already knows is dropped there too.
        if (upload.stage != "failed") {
            Icon(
                painterResource(R.drawable.ui_close),
                contentDescription = stringResource(R.string.ui_cancel_upload),
                tint = colors.text3,
                modifier = Modifier.size(20.dp).clickable(onClickLabel = "Cancel upload", onClick = onCancel),
            )
        }
    }
}

@Composable
private fun EntryActions(
    entry: DriveEntry,
    onDismiss: () -> Unit,
    onRename: () -> Unit,
    onMove: () -> Unit,
    onNewVersion: () -> Unit,
    onTrash: () -> Unit,
    onRestore: () -> Unit,
    onDelete: () -> Unit,
    onShareLink: () -> Unit = {},
) {
    val colors = RedvaultTheme.colors
    var confirmDelete by remember { mutableStateOf(false) }
    if (confirmDelete) {
        AlertDialog(
            onDismissRequest = onDismiss,
            containerColor = colors.surface,
            title = { Text(stringResource(R.string.ui_delete_for_good), color = colors.text) },
            text = { Text(if (entry.isFile) "Every version of “${entry.name}” is deleted from storage. This can't be undone." else "“${entry.name}” and everything in it is deleted from storage. This can't be undone.", color = colors.text2) },
            confirmButton = { TextButton(onClick = { onDelete(); onDismiss() }) { Text(stringResource(R.string.ui_delete), color = colors.danger) } },
            dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
        )
        return
    }
    val actions = buildList {
        if (entry.incoming) return@buildList
        if (entry.trashed) {
            add(SheetAction("Restore", R.drawable.ui_refresh) { onRestore(); onDismiss() })
            add(SheetAction("Delete for good", R.drawable.ui_trash, danger = true) { confirmDelete = true })
        } else {
            if (!entry.locked) add(SheetAction("Share link…", R.drawable.ui_link) { onShareLink(); onDismiss() })
            add(SheetAction("Rename", R.drawable.ui_pen) { onRename(); onDismiss() })
            add(SheetAction("Move to…", R.drawable.ui_folder) { onMove(); onDismiss() })
            if (entry.isFile) add(SheetAction("Upload a new version…", R.drawable.ui_upload) { onNewVersion(); onDismiss() })
            add(SheetAction("Move to Trash", R.drawable.ui_trash, danger = true) { onTrash(); onDismiss() })
        }
    }
    ActionSheet(onDismiss = onDismiss, actions = actions, title = entry.name) {
        if (actions.isEmpty()) Text(stringResource(R.string.ui_shared_with_you_read_only), color = colors.text2, modifier = Modifier.padding(horizontal = 4.dp, vertical = 8.dp))
    }
}

@Composable
internal fun NameDialog(title: String, initial: String, confirm: String, onDismiss: () -> Unit, onConfirm: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    var name by remember { mutableStateOf(initial) }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text(title, color = colors.text) },
        text = { DsTextField(name, { name = it }, "Name") },
        confirmButton = { TextButton(onClick = { onConfirm(name) }, enabled = name.isNotBlank()) { Text(confirm, color = colors.accent) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
    )
}

// --- File details ---

/** One file: preview (once unlocked), details, its lock, sharing, and Save / Share. */
@Composable
fun FileDetailScreen(
    files: FilesSession,
    state: FilesState,
    id: String,
    contacts: List<ShareContact>,
    onBack: () -> Unit,
    onUnlock: (then: String) -> Unit,
    onChangeLock: () -> Unit,
    onAddLock: () -> Unit,
    onShare: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val entry = state.find(id)
    if (entry == null) {
        LaunchedEffect(Unit) { onBack() }
        return
    }
    val unlocked = entry.id in state.unlocked
    val lockedNow = entry.locked && !unlocked
    val lockKind = FileLocks.lockKind(entry.meta)
    val lockKey = state.key(FileLocks.lockKeyId(entry.meta))
    val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument(entry.mimeType.ifEmpty { "application/octet-stream" })) { uri ->
        if (uri != null) {
            scope.launch {
                val output = context.contentResolver.openOutputStream(uri) ?: return@launch
                files.saveTo(entry.id, output)?.let { error -> files.dismissMessages(); android.widget.Toast.makeText(context, error, android.widget.Toast.LENGTH_LONG).show() }
            }
        }
    }
    // Pictures, text, CSV, JSON, audio, video and PDF open in the in-app viewer (FileViewer.kt).
    val previewable = entry.isFile && FilePreview.kindFor(entry.name) != null
    var viewing by rememberSaveable(entry.id) { mutableStateOf(false) }
    if (viewing && !lockedNow) {
        FileViewerDialog(files, entry, onDismiss = { viewing = false }, onSave = { saver.launch(entry.name) })
    }
    val preview by produceState<ImageBitmap?>(null, entry.id, lockedNow) {
        value = null
        if (!lockedNow && entry.mimeType.startsWith("image/") && entry.size <= 25L * 1024 * 1024) {
            val (file, _) = files.openCopy(entry.id)
            value = file?.let { BitmapFactory.decodeFile(it.path)?.asImageBitmap() }
        }
    }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back", onClick = onBack, container = colors.bg)
        }
        Column(
            Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            // Preview, or the lock.
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(150.dp)
                    .clip(RoundedCornerShape(18.dp))
                    .background(if (lockedNow) colors.lockBg else colors.surface)
                    .border(1.dp, if (lockedNow) colors.lockBorder else colors.border, RoundedCornerShape(18.dp))
                    .clickable(enabled = !lockedNow && entry.isFile) {
                        if (previewable) {
                            viewing = true
                            return@clickable
                        }
                        scope.launch {
                            val (file, error) = files.openCopy(entry.id)
                            if (file != null) openFile(context, file, entry.mimeType) { android.widget.Toast.makeText(context, it, android.widget.Toast.LENGTH_LONG).show() }
                            else if (error != null) android.widget.Toast.makeText(context, error, android.widget.Toast.LENGTH_LONG).show()
                        }
                    },
                contentAlignment = Alignment.Center,
            ) {
                val image = preview
                when {
                    lockedNow -> Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        DsIcon(R.drawable.ui_lock, colors.lockText2, 26.dp)
                        Text(stringResource(R.string.ui_locked_no_preview_until_unlocked), color = colors.lockText, fontSize = 13.sp)
                    }
                    image != null -> Image(image, contentDescription = "Preview of ${entry.name}", contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
                    else -> Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        val (label, tint) = typeOf(entry.name, false)
                        Text(label, color = tint, fontFamily = MonoFamily, fontSize = 22.sp, fontWeight = FontWeight.Medium)
                        Text(
                            if (previewable) "[tap to preview · decrypted on this phone]" else stringResource(R.string.ui_tap_to_open_decrypted_on_this),
                            color = colors.text3, fontFamily = MonoFamily, fontSize = 11.sp,
                        )
                    }
                }
            }

            Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(entry.name, color = colors.text, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
                val versions = state.versionsOf(entry)
                Text(
                    "${sizeText(entry.size)} · modified ${TimeFormat.listLabel(entry.modifiedMs)} · " +
                        (if (entry.incoming) "shared with you" else if (entry.sharedWith.isEmpty()) "only you" else "shared with ${contacts.firstOrNull { it.userId == entry.sharedWith.first() }?.handle ?: "someone"}") +
                        (if (versions > 1) " · $versions versions" else ""),
                    color = colors.text2,
                    fontSize = 13.sp,
                )
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    DsIcon(R.drawable.ui_shield_check, colors.accent, 14.dp)
                    Text(stringResource(R.string.ui_encrypted_on_this_phone_hash_checked), color = colors.accent, fontSize = 12.sp)
                }
            }

            if (!entry.incoming && !entry.trashed) {
                LockCard(entry, unlocked, lockKind, lockKey?.name, lockKey?.passwordProtected == true, lockKey != null, onUnlock, onChangeLock, onAddLock) { files.lockNow(entry.id) }
                if (lockedNow) Callout(stringResource(R.string.ui_unlock_this_file_to_save_it), CalloutTone.Neutral, R.drawable.ui_info)
                if (unlocked) Callout("Sharing gives the other person a copy they can open without your ${if (lockKind == "password") "password" else "key"}.", CalloutTone.Warn, R.drawable.ui_alert_circle)
            }

            // Who it's shared with.
            if (!entry.incoming && !lockedNow && entry.sharedWith.isNotEmpty()) {
                entry.sharedWith.forEach { userId ->
                    val person = contacts.firstOrNull { it.userId == userId }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Avatar(person?.avatarUrl, person?.handle ?: userId, size = 36.dp)
                        Text(person?.handle ?: "Someone", color = colors.text, fontSize = 15.sp, modifier = Modifier.weight(1f))
                        Text(stringResource(R.string.ui_can_view), color = colors.text3, fontSize = 13.sp)
                        RoundButton(R.drawable.ui_close, "Stop sharing with ${person?.handle ?: "them"}", onClick = { files.unshare(entry.id, userId) }, size = 36.dp, iconSize = 16.dp)
                    }
                }
            }
            state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle, Modifier.clickable { files.dismissMessages() }) }
            state.notice?.let { Callout(it, CalloutTone.Ok, R.drawable.ui_check, Modifier.clickable { files.dismissMessages() }) }
            Spacer(Modifier.height(12.dp))
        }
        if (previewable) {
            PillButton(
                "Preview", { viewing = true }, Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 12.dp),
                style = PillStyle.Primary, enabled = !lockedNow, icon = R.drawable.ui_eye,
            )
        }
        Row(Modifier.fillMaxWidth().navigationBarsPadding().padding(horizontal = 20.dp, vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            PillButton(
                "Save", { saver.launch(entry.name) }, Modifier.weight(1f), style = PillStyle.Secondary, enabled = !lockedNow, icon = R.drawable.ui_download,
            )
            if (!entry.incoming && !entry.trashed) {
                PillButton(stringResource(R.string.ui_share), onShare, Modifier.weight(1f), style = PillStyle.Primary, enabled = !lockedNow, icon = R.drawable.ui_share)
            }
        }
    }
}

@Composable
private fun LockCard(
    entry: DriveEntry,
    unlocked: Boolean,
    lockKind: String,
    keyName: String?,
    keyHasPassword: Boolean,
    keyAvailable: Boolean,
    onUnlock: (then: String) -> Unit,
    onChangeLock: () -> Unit,
    onAddLock: () -> Unit,
    onLockNow: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val locked = entry.locked
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(18.dp))
            .background(if (unlocked) colors.accentSoft else if (locked) colors.lockBg.copy(alpha = 0.55f) else colors.surface)
            .border(1.dp, if (unlocked) colors.accentBorder else if (locked) colors.lockBorder else colors.border, RoundedCornerShape(18.dp))
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            when {
                unlocked -> "Unlocked for this session"
                lockKind == "key" -> "Locked with a key"
                lockKind == "password" -> "Locked with a password"
                else -> "No extra lock"
            },
            color = if (unlocked) colors.accent else if (locked) colors.lockText else colors.text,
            fontSize = 15.sp,
            fontWeight = FontWeight.SemiBold,
        )
        if (lockKind == "key") {
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(if (unlocked) colors.bg.copy(alpha = 0.4f) else colors.lockBg).padding(12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                DsIcon(R.drawable.ui_key, if (unlocked) colors.text2 else colors.lockText2, 16.dp)
                Column {
                    Text(keyName ?: "A key that isn't on this account", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    Text(FileLocks.lockFingerprint(entry.meta), color = if (unlocked) colors.accent else colors.lockText2, fontFamily = MonoFamily, fontSize = 12.sp)
                }
            }
        }
        Text(
            when {
                unlocked -> "The file's key is never saved back to the server unlocked, even if you rename or move it. It locks again when you close the app."
                lockKind == "key" && !keyAvailable -> "This key isn't among your encryption keys. Import it to open the file."
                lockKind == "key" -> if (keyHasPassword) "Opening it needs your account plus this key and its password." else "Opening it needs your account plus this key."
                lockKind == "password" -> "Opening it needs your account plus this file's own password."
                else -> "Your account protects this file. Add a key or a password if it needs more."
            },
            color = colors.text,
            fontSize = 14.sp,
        )
        when {
            unlocked -> Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                PillButton(stringResource(R.string.ui_lock_now), onLockNow, Modifier.weight(1f), style = PillStyle.Secondary, height = 46.dp)
                PillButton(stringResource(R.string.ui_change_lock), onChangeLock, Modifier.weight(1f), style = PillStyle.Secondary, height = 46.dp)
            }
            locked -> {
                PillButton(stringResource(R.string.ui_unlock_for_this_session), { onUnlock("") }, Modifier.fillMaxWidth(), height = 50.dp)
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    PillButton(stringResource(R.string.ui_change_lock), { onUnlock("changeLock") }, Modifier.weight(1f), style = PillStyle.Secondary, height = 46.dp)
                    PillButton(stringResource(R.string.ui_remove_lock), { onUnlock("removeLock") }, Modifier.weight(1f), style = PillStyle.Secondary, height = 46.dp)
                }
            }
            else -> PillButton(stringResource(R.string.ui_add_a_lock), onAddLock, Modifier.fillMaxWidth(), style = PillStyle.Secondary, height = 46.dp, icon = R.drawable.ui_lock)
        }
    }
}

// --- Sharing ---

/** Share a file: with a person from your chats, or with a link (expiry, password, download). */
@Composable
fun ShareFileScreen(files: FilesSession, state: FilesState, id: String, contacts: List<ShareContact>, onBack: () -> Unit, onPublicLink: () -> Unit = {}) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val entry = state.find(id) ?: run {
        LaunchedEffect(Unit) { onBack() }
        return
    }
    val links = state.snapshot.links.filter { it.backupId == entry.id }

    FullSheet(
        title = stringResource(R.string.ui_share),
        onCancel = onBack,
        primary = "Share link…",
        primaryEnabled = !entry.locked,
        busy = state.busy,
        onPrimary = onPublicLink,
    ) {
        Text(entry.name, color = colors.text, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
        Callout(stringResource(R.string.ui_sharing_gives_the_other_person_a), CalloutTone.Neutral, R.drawable.ui_info)
        SectionTitle("With a person")
        if (contacts.isEmpty()) Text(stringResource(R.string.ui_chat_with_someone_first_to_share), color = colors.text3, fontSize = 14.sp)
        contacts.forEach { person ->
            val shared = person.userId in entry.sharedWith
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).clickable(enabled = !shared) { files.shareWith(entry.id, person.userId, person.handle) }.padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Avatar(person.avatarUrl, person.handle, size = 40.dp)
                Text(person.handle, color = colors.text, fontSize = 15.sp, modifier = Modifier.weight(1f))
                Text(if (shared) "Shared" else "Share", color = if (shared) colors.text3 else colors.accent, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            }
        }
        SectionTitle("With a link")
        Text(stringResource(R.string.ui_a_link_anyone_can_open_in), color = colors.text2, fontSize = 14.sp)
        if (links.isNotEmpty()) Text(stringResource(R.string.ui_older_file_links_they_still_work), color = colors.text3, fontSize = 13.sp)
        links.forEach { link ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                DsIcon(R.drawable.ui_link, colors.accent, 16.dp)
                Text(
                    (if (link.expiresAt != null) "Expires ${link.expiresAt.take(10)}" else "Never expires") +
                        (if (link.hasPassword) " · password" else "") +
                        // Not "view only": a browser that shows the file holds all of it (2026-11-04 assessment, F-33).
                        (if (link.allowDownload) " · download" else " · no download button"),
                    color = colors.text2,
                    fontSize = 13.sp,
                    modifier = Modifier.weight(1f),
                )
                val url = files.linkUrl(entry.id, link.id)
                if (url != null) RoundButton(R.drawable.ui_copy, "Copy link", onClick = { copyText(context, url, secret = false) }, size = 36.dp, iconSize = 16.dp)
                RoundButton(R.drawable.ui_trash, "Remove link (it stops working)", onClick = { files.deleteLink(entry.id, link.id) }, size = 36.dp, iconSize = 16.dp)
            }
        }
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        state.notice?.let { Callout(it, CalloutTone.Ok, R.drawable.ui_check) }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, color = RedvaultTheme.colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(top = 6.dp))
}

/** Copies text; a secret is kept out of the keyboard's clipboard history where Android allows. */
fun copyText(context: Context, text: String, secret: Boolean) = SecretClipboard.copy(context, text, secret)

@Suppress("unused")
private fun Modifier.a11y(label: String) = this.semantics { contentDescription = label }
