// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.FileLocks
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

/**
 * Upload with an extra lock (a key or a password), or add / change / remove
 * the lock of a file that's already stored ([changeId]). The lock wraps each
 * file's own key, so nothing is re-encrypted or re-uploaded to change it.
 */
@Composable
fun UploadLockSheet(
    files: FilesSession,
    state: FilesState,
    uris: List<Uri>,
    parent: String,
    changeId: String?,
    startMode: String?,
    onDone: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val changing = changeId?.let { state.find(it) }
    val currentLock = changing?.let { FileLocks.lockKind(it.meta) } ?: ""
    val described = remember(uris) {
        uris.map { uri ->
            var name = "file"
            var size = 0L
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 }?.let { name = c.getString(it) ?: name }
                    c.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 && !c.isNull(it) }?.let { size = c.getLong(it) }
                }
            }
            name to size
        }
    }
    var mode by remember { mutableStateOf(startMode ?: currentLock.ifEmpty { if (state.keys.isNotEmpty()) "key" else if (changing != null) "password" else "none" }) }
    var keyId by remember { mutableStateOf(changing?.let { FileLocks.lockKeyId(it.meta) }?.takeIf { id -> state.keys.any { it.id == id } } ?: state.keys.lastOrNull()?.id ?: "") }
    var keyPassword by remember { mutableStateOf("") }
    var filePassword by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    val plural = described.size != 1
    val needsKeyPassword = mode == "key" && files.keyNeedsPassword(keyId)

    FullSheet(
        title = when {
            changing == null -> if (plural) "Upload ${described.size} files" else "Upload 1 file"
            currentLock.isNotEmpty() -> "Change the lock"
            else -> "Add a lock"
        },
        onCancel = onDone,
        primary = when {
            changing == null -> if (mode == "none") "Upload" else "Upload and lock"
            mode == "none" -> "Remove lock"
            else -> "Save lock"
        },
        primaryEnabled = (mode != "key" || (keyId.isNotEmpty() && (!needsKeyPassword || keyPassword.isNotEmpty()))) &&
            (mode != "password" || files.passwordProblem(filePassword) == null) &&
            !(changing != null && mode == "none" && currentLock.isEmpty()),
        busy = state.busy,
        primaryDanger = changing != null && mode == "none",
        onPrimary = {
            error = null
            scope.launch {
                val choice = LockChoice(mode, keyId, keyPassword, filePassword)
                val result = if (changing == null) files.upload(uris, described, parent, choice) else files.changeLock(changing.id, choice)
                if (result == null) onDone() else error = result
            }
        },
    ) {
        if (changing != null) {
            Text(changing.name, color = colors.text2, fontSize = 14.sp)
        } else {
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface)) {
                described.forEach { (name, size) ->
                    Row(
                        Modifier.fillMaxWidth().heightIn(min = 54.dp).padding(horizontal = 14.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        FileTile(name, folder = false, lockedNow = false, size = 34.dp)
                        Text(name, color = colors.text, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        Text(sizeText(size), color = colors.text2, fontFamily = MonoFamily, fontSize = 13.sp)
                    }
                }
            }
        }
        Text("Extra lock", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        OptionCard(
            if (changing != null && currentLock.isNotEmpty()) "Remove the lock" else "No extra lock",
            if (changing == null) "Your account protects them." else "Your account protects it, like your other files.",
            mode == "none",
        ) { mode = "none"; error = null }
        OptionCard("Lock with a key", "Opening needs your account plus the key.", mode == "key") { mode = "key"; error = null }
        if (mode == "key") {
            Column(Modifier.padding(start = 24.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                KeyPicker(state.keys, keyId) { keyId = it; error = null }
                if (needsKeyPassword) DsTextField(keyPassword, { keyPassword = it }, "Key password", password = true)
            }
        }
        OptionCard("Lock with a password", "Opening needs your account plus this password.", mode == "password") { mode = "password"; error = null }
        if (mode == "password") {
            Column(Modifier.padding(start = 24.dp)) { LockPasswordField(files, "File password", filePassword) { filePassword = it } }
        }
        error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        Callout(
            "Our servers and storage can't tell locked files apart. You can change the lock later without re-uploading.",
            CalloutTone.Neutral,
            R.drawable.ui_info,
        )
    }
}

/** Unlocks a locked file for this session, with a key or the file's own password. */
@Composable
fun UnlockFileSheet(files: FilesSession, state: FilesState, id: String, onDone: (unlocked: Boolean) -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val entry = state.find(id) ?: run {
        LaunchedEffect(Unit) { onDone(false) }
        return
    }
    val lockKind = FileLocks.lockKind(entry.meta)
    val lockKeyId = FileLocks.lockKeyId(entry.meta)
    val lockKey = state.key(lockKeyId)
    var mode by remember { mutableStateOf(if (lockKind == "password") "password" else "key") }
    var keyId by remember { mutableStateOf(lockKey?.id ?: state.keys.firstOrNull()?.id ?: "") }
    var keyPassword by remember { mutableStateOf("") }
    var filePassword by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    val needsKeyPassword = mode == "key" && files.keyNeedsPassword(keyId)

    FullSheet(
        title = "Unlock file",
        onCancel = { onDone(false) },
        primary = "Unlock",
        primaryEnabled = if (mode == "password") filePassword.isNotEmpty() else keyId.isNotEmpty() && (!needsKeyPassword || keyPassword.isNotEmpty()),
        busy = state.busy,
        onPrimary = {
            error = null
            scope.launch {
                val result = files.unlock(entry.id, LockChoice(mode, keyId, keyPassword, filePassword))
                if (result == null) onDone(true) else error = result
            }
        },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(entry.name, color = colors.text, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
            if (lockKind == "password") {
                Text("Locked with a password", color = colors.text2, fontSize = 14.sp)
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("Locked with", color = colors.text2, fontSize = 14.sp)
                    Text(lockKey?.name ?: "a key", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    Text("· ${FileLocks.lockFingerprint(entry.meta).take(9)}", color = colors.lockText2, fontFamily = MonoFamily, fontSize = 13.sp)
                }
            }
        }
        // Use a key | Use a password
        Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(colors.surface).padding(4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("key" to "Use a key", "password" to "Use a password").forEach { (value, label) ->
                val on = mode == value
                Text(
                    label,
                    color = if (on) colors.text else colors.text2,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(10.dp))
                        .background(if (on) colors.surface3 else Color.Transparent)
                        .clickable(role = Role.Tab) { mode = value; error = null }
                        .semantics { selected = on }
                        .padding(vertical = 12.dp),
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                )
            }
        }
        if (mode == "key") {
            KeyPicker(state.keys, keyId) { keyId = it; error = null }
            if (needsKeyPassword) {
                FieldLabel("Key password")
                DsTextField(keyPassword, { keyPassword = it }, "Key password", password = true)
            } else if (keyId.isNotEmpty()) {
                val hint = if (state.key(keyId)?.passwordProtected == false) "This key has no password, so nothing else is needed." else "This key is already open this session."
                Text(hint, color = colors.text3, fontSize = 13.sp)
            }
        } else {
            FieldLabel("File password")
            DsTextField(filePassword, { filePassword = it }, "File password", password = true)
        }
        error?.let { message ->
            Callout(message, CalloutTone.Error, R.drawable.ui_alert_circle) {
                if (message.contains("newer version")) {
                    PillButton(
                        "Update the app",
                        {
                            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=${context.packageName}"))
                            runCatching { context.startActivity(intent) }
                        },
                        style = PillStyle.Secondary,
                        height = 38.dp,
                    )
                }
            }
        }
        Callout(
            "Unlocks for this session only. The file's key is never saved back to the server unlocked, even if you rename or move it.",
            CalloutTone.Ok,
            R.drawable.ui_shield,
        )
    }
}
