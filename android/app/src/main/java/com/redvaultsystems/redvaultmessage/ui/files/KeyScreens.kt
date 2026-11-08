// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.FileKey
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.SwitchCard
import com.redvaultsystems.redvaultmessage.ui.components.TouchGuard
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

/** "Created Sep 3", "Imported today". */
fun createdText(key: FileKey): String {
    val verb = if (key.imported) "Imported" else "Created"
    if (key.createdMs <= 0) return verb
    val day = Calendar.getInstance().apply { timeInMillis = key.createdMs }
    val today = Calendar.getInstance()
    fun same(a: Calendar, b: Calendar) = a.get(Calendar.YEAR) == b.get(Calendar.YEAR) && a.get(Calendar.DAY_OF_YEAR) == b.get(Calendar.DAY_OF_YEAR)
    if (same(day, today)) return "$verb today"
    today.add(Calendar.DAY_OF_YEAR, -1)
    if (same(day, today)) return "$verb yesterday"
    val pattern = if (day.get(Calendar.YEAR) == Calendar.getInstance().get(Calendar.YEAR)) "MMM d" else "MMM d, yyyy"
    return "$verb ${SimpleDateFormat(pattern, Locale.getDefault()).format(Date(key.createdMs))}"
}

/** The encryption keys list: create, import, and each key's fingerprint and protection. */
@Composable
fun FileKeysScreen(state: FilesState, onBack: () -> Unit, onCreate: () -> Unit, onImport: () -> Unit, onOpen: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back", onClick = onBack, container = colors.bg)
            Spacer(Modifier.weight(1f))
            Text(
                "Import",
                color = colors.accent,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.clip(RoundedCornerShape(10.dp)).clickable(role = Role.Button, onClick = onImport).padding(horizontal = 12.dp, vertical = 10.dp),
            )
        }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Text("Encryption keys", style = RedvaultFonts.title, color = colors.text)
            Text(
                "Your own 256-bit keys for locking files. Saved in your password vault as file keys, so they stay encrypted and sync to your devices.",
                color = colors.text2,
                fontSize = 15.sp,
            )
            PillButton("Create key", onCreate, Modifier.fillMaxWidth(), icon = R.drawable.ui_plus)
            if (state.keys.isNotEmpty()) {
                Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface)) {
                    state.keys.forEachIndexed { i, key ->
                        if (i > 0) Box(Modifier.fillMaxWidth().height(1.dp).background(colors.border))
                        val filesCount = state.filesLockedWith(key.id)
                        Row(
                            Modifier.fillMaxWidth().clickable { onOpen(key.id) }.padding(14.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(14.dp),
                        ) {
                            Box(Modifier.size(40.dp).clip(RoundedCornerShape(10.dp)).background(colors.lockBg), contentAlignment = Alignment.Center) {
                                DsIcon(R.drawable.ui_key, colors.lockText2, 18.dp)
                            }
                            Column(Modifier.weight(1f)) {
                                Text(key.name, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                                Text(key.fingerprint, color = colors.lockText2, fontFamily = RedvaultFonts.mono, fontSize = 12.sp)
                                Text(
                                    (if (key.passwordProtected) "Account + password" else "Account only") + " · " +
                                        (if (filesCount == 0) "no files yet" else if (filesCount == 1) "1 file" else "$filesCount files") +
                                        (if (key.imported) " · imported" else ""),
                                    color = if (key.passwordProtected) colors.text3 else colors.warn,
                                    fontSize = 13.sp,
                                )
                            }
                        }
                    }
                }
            } else if (state.keysLoaded) {
                Text("No keys yet. Create one, or import a key you made elsewhere.", color = colors.text3, fontSize = 14.sp)
            }
            Callout(
                "Fingerprints are the same on every device and don't reveal the key. File keys stay off your Passwords list.",
                CalloutTone.Neutral,
                R.drawable.ui_info,
            )
            state.notice?.let { Callout(it, CalloutTone.Ok, R.drawable.ui_check) }
            Spacer(Modifier.height(24.dp))
        }
    }
}

/** One key: fingerprint, its password, export for backup, rename and delete. */
@Composable
fun FileKeyScreen(files: FilesSession, state: FilesState, id: String, onBack: () -> Unit, onPassword: (action: String) -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val key = state.key(id) ?: run {
        LaunchedEffect(Unit) { onBack() }
        return
    }
    var exported by remember { mutableStateOf<String?>(null) }
    var askingPassword by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    val filesCount = state.filesLockedWith(key.id)
    // The export is hidden again when leaving the screen.
    DisposableEffect(id) { onDispose { exported = null } }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back", onClick = onBack, container = colors.bg)
            Spacer(Modifier.weight(1f))
            Text(
                "Rename",
                color = colors.accent,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.clip(RoundedCornerShape(10.dp)).clickable(role = Role.Button) { renaming = true }.padding(horizontal = 12.dp, vertical = 10.dp),
            )
        }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                Box(Modifier.size(52.dp).clip(RoundedCornerShape(14.dp)).background(colors.lockBg), contentAlignment = Alignment.Center) {
                    DsIcon(R.drawable.ui_key, colors.lockText2, 24.dp)
                }
                Column {
                    Text(key.name, fontFamily = RedvaultFonts.display, fontWeight = FontWeight.Bold, fontSize = 26.sp, color = colors.text)
                    Text("${createdText(key)} · " + if (filesCount == 1) "1 locked file" else "$filesCount locked files", color = colors.text3, fontSize = 13.sp)
                }
            }
            // Fingerprint
            Column(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.lockBg.copy(alpha = 0.6f)).border(1.dp, colors.lockBorder, RoundedCornerShape(16.dp)).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Text("Fingerprint", color = colors.lockText2, fontSize = 12.sp)
                Text(key.fingerprint, color = colors.text, fontFamily = RedvaultFonts.mono, fontSize = 19.sp, fontWeight = FontWeight.Medium)
                Text("Same on every device. Doesn't reveal the key.", color = colors.lockText2, fontSize = 12.sp)
            }
            // Key password
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Key password", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                    Text(if (key.passwordProtected) "On" else "Off", color = if (key.passwordProtected) colors.accent else colors.warn, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                }
                Text(
                    if (key.passwordProtected) "Using this key needs your account and its password (Argon2id, stronger setting)."
                    else "Your account alone protects this key. Add a password so a signed-in device isn’t enough to open locked files.",
                    color = colors.text2,
                    fontSize = 14.sp,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    if (key.passwordProtected) {
                        PillButton("Change", { onPassword("change") }, Modifier.weight(1f), style = PillStyle.Secondary, height = 44.dp)
                        PillButton("Remove", { onPassword("remove") }, Modifier.weight(1f), style = PillStyle.Secondary, height = 44.dp)
                    } else {
                        PillButton("Set a password", { onPassword("set") }, Modifier.weight(1f), style = PillStyle.Secondary, height = 44.dp)
                    }
                }
                Text("Instant: locked files aren't re-encrypted.", color = colors.accent, fontSize = 12.sp)
            }
            // Export for backup
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Export for backup", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                    RoundButton(
                        if (exported != null) R.drawable.ui_eye_off else R.drawable.ui_eye,
                        if (exported != null) "Hide the key" else "Show the key",
                        onClick = {
                            if (exported != null) exported = null
                            else if (files.keyNeedsPassword(key.id)) askingPassword = true
                            else scope.launch { exported = runCatching { files.exportKey(key.id, "") }.getOrNull() }
                        },
                        size = 38.dp,
                        iconSize = 17.dp,
                    )
                    RoundButton(
                        R.drawable.ui_copy,
                        "Copy the key (clears from the clipboard in 30 seconds)",
                        onClick = {
                            val text = exported?.replace(" ", "") ?: return@RoundButton
                            SecretClipboard.copy(context, text, secret = true)
                        },
                        size = 38.dp,
                        iconSize = 17.dp,
                        enabled = exported != null,
                    )
                }
                Text(
                    exported ?: "•••••••• •••••••• •••••••• •••••••• •••••••• •••••••• •••••••• ••••••••",
                    color = colors.text,
                    fontFamily = RedvaultFonts.mono,
                    fontSize = 14.sp,
                    lineHeight = 20.sp,
                    modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(colors.bg).padding(12.dp),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    DsIcon(R.drawable.ui_alert_circle, colors.warn, 15.dp)
                    Text("This is the raw key. The key password doesn't protect it, so store it offline.", color = colors.text, fontSize = 13.sp)
                }
            }
            Text(
                if (filesCount == 0) "No files are locked with this key."
                else "${if (filesCount == 1) "1 file is" else "$filesCount files are"} locked with this key. After you delete it they can't be opened unless you have an export.",
                color = colors.text3,
                fontSize = 13.sp,
            )
            PillButton("Delete key", { deleting = true }, Modifier.fillMaxWidth(), style = PillStyle.Danger, height = 48.dp, icon = R.drawable.ui_trash)
            Spacer(Modifier.height(24.dp))
        }
    }

    if (askingPassword) {
        var password by remember { mutableStateOf("") }
        var error by remember { mutableStateOf<String?>(null) }
        AlertDialog(
            onDismissRequest = { askingPassword = false },
            containerColor = colors.surface,
            title = { Text("Show the key", color = colors.text) },
            text = {
                TouchGuard()
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("Enter the password of “${key.name}” to show it for backup.", color = colors.text2)
                    DsTextField(password, { password = it }, "Key password", password = true)
                    error?.let { Text(it, color = colors.danger, fontSize = 13.sp) }
                }
            },
            confirmButton = {
                TextButton(
                    enabled = password.isNotEmpty() && !state.busy,
                    onClick = {
                        scope.launch {
                            try {
                                exported = files.exportKey(key.id, password)
                                askingPassword = false
                            } catch (e: UserFacingException) {
                                error = e.message
                            }
                        }
                    },
                ) { Text("Show key", color = colors.accent) }
            },
            dismissButton = { TextButton(onClick = { askingPassword = false }) { Text("Cancel", color = colors.text2) } },
        )
    }
    if (renaming) {
        NameDialog("Rename key", key.name, "Rename", onDismiss = { renaming = false }) { name ->
            files.renameKey(key.id, name)
            renaming = false
        }
    }
    if (deleting) {
        AlertDialog(
            onDismissRequest = { deleting = false },
            containerColor = colors.surface,
            title = { Text("Delete this key?", color = colors.text) },
            text = {
                TouchGuard()
                Text(
                    if (filesCount > 0) "${if (filesCount == 1) "1 file is" else "$filesCount files are"} locked with “${key.name}”. After you delete it they can't be opened unless you have an export."
                    else "No files are locked with “${key.name}”.",
                    color = colors.text2,
                )
            },
            confirmButton = { TextButton(onClick = { files.deleteKey(key.id); deleting = false; onBack() }) { Text("Delete key", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { deleting = false }) { Text("Cancel", color = colors.text2) } },
        )
    }
}

/** New key: a random 256-bit key made on this phone, with an optional password of its own. */
@Composable
fun KeyCreateSheet(files: FilesSession, state: FilesState, onDone: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var name by remember { mutableStateOf("") }
    var protect by remember { mutableStateOf(true) }
    var password by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    FullSheet(
        title = "New key",
        onCancel = onDone,
        primary = "Create key",
        primaryEnabled = name.isNotBlank() && (!protect || files.passwordProblem(password) == null),
        busy = state.busy,
        onPrimary = {
            error = null
            scope.launch {
                val result = files.createKey(name, if (protect) password else "")
                if (result == null) onDone() else error = result
            }
        },
    ) {
        Text("A random 256-bit key is made on this phone and saved to your vault as a file key.", color = colors.text2, fontSize = 15.sp)
        Column {
            FieldLabel("Name")
            DsTextField(name, { name = it }, "Legal documents")
        }
        SwitchCard(
            "Protect with a password",
            if (protect) "Using the key will need your account and this password." else "Your account alone will protect this key.",
            protect,
            { protect = it },
        )
        if (protect) {
            LockPasswordField(files, "Key password", password) { password = it }
            Text(
                "You'll need it to open files locked with this key, so keep it somewhere safe. Stretched with Argon2id at the stronger setting.",
                color = colors.text2,
                fontSize = 13.sp,
            )
        }
        error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    }
}

/** Import key: a 256-bit key made outside the app, checked as it's pasted. Its password is set on a second step. */
@Composable
fun KeyImportSheet(files: FilesSession, state: FilesState, onDone: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var keyText by remember { mutableStateOf("") }
    var name by remember { mutableStateOf("") }
    var protect by remember { mutableStateOf(true) }
    var step by remember { mutableStateOf(1) }
    var password by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    val check = remember(keyText, state.keys) { files.checkImport(keyText) }
    DisposableEffect(Unit) { onDispose { keyText = "" } }

    FullSheet(
        title = if (step == 1) "Import key" else "Key password",
        onCancel = { if (step == 2) step = 1 else onDone() },
        primary = "Import key",
        primaryEnabled = check is ImportCheck.Valid && name.isNotBlank() && (step == 1 || files.passwordProblem(password) == null),
        busy = state.busy,
        onPrimary = {
            error = null
            if (step == 1 && protect) {
                step = 2
                return@FullSheet
            }
            scope.launch {
                val result = files.importKey(name, keyText, if (step == 2) password else "")
                if (result == null) onDone() else error = result
            }
        },
    ) {
        if (step == 1) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Key", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                Text(
                    "Paste",
                    color = colors.accent,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.clip(RoundedCornerShape(8.dp)).clickable(role = Role.Button) {
                        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                        clipboard.primaryClip?.getItemAt(0)?.text?.toString()?.let { keyText = it }
                    }.padding(horizontal = 8.dp, vertical = 4.dp),
                )
            }
            DsTextField(keyText, { keyText = it }, "8d41 f07c 2b9e 55a3 …", mono = true, singleLine = false, minHeight = 96.dp, label = "Key")
            Text("64 hex characters, or base64 / base64url. Spaces, line breaks and dashes are ignored.", color = colors.text2, fontSize = 13.sp)
            when (check) {
                is ImportCheck.Valid -> Callout("Valid 256-bit key · ${check.format}", CalloutTone.Ok, R.drawable.ui_check, detail = "Fingerprint ${check.fingerprint}")
                is ImportCheck.Invalid -> Callout(check.message, CalloutTone.Error, R.drawable.ui_alert_circle)
                is ImportCheck.Duplicate -> Callout("You already have this key: ${check.name} (${check.fingerprint}).", CalloutTone.Error, R.drawable.ui_alert_circle)
                ImportCheck.Empty -> {}
            }
            Column {
                FieldLabel("Name")
                DsTextField(name, { name = it }, "Offsite backup key")
            }
            SwitchCard(
                "Protect with a password",
                if (protect) "You'll set it on the next step." else "Your account alone will protect this key.",
                protect,
                { protect = it },
            )
        } else {
            Text("Using “${name.trim()}” will need your account and this password.", color = colors.text2, fontSize = 15.sp)
            LockPasswordField(files, "Key password", password) { password = it }
        }
        error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    }
}

/** Set, change or remove a key's own password ([action]: "set", "change", "remove"). */
@Composable
fun KeyPasswordSheet(files: FilesSession, state: FilesState, id: String, action: String, onDone: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    val key = state.key(id) ?: run {
        LaunchedEffect(Unit) { onDone() }
        return
    }
    var current by remember { mutableStateOf("") }
    var next by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    FullSheet(
        title = when (action) {
            "set" -> "Set a password"
            "remove" -> "Remove password"
            else -> "Change password"
        },
        onCancel = onDone,
        primary = if (action == "remove") "Remove password" else "Save password",
        primaryEnabled = (action == "set" || current.isNotEmpty()) && (action == "remove" || files.passwordProblem(next) == null),
        busy = state.busy,
        primaryDanger = action == "remove",
        onPrimary = {
            error = null
            scope.launch {
                val result = files.setKeyPassword(key.id, current, if (action == "remove") "" else next)
                if (result == null) onDone() else error = result
            }
        },
    ) {
        Text(
            if (action == "remove") "Your account alone will protect “${key.name}”. Files locked with it aren't re-encrypted."
            else "Using “${key.name}” will need your account and this password. Files locked with it aren't re-encrypted.",
            color = colors.text2,
            fontSize = 15.sp,
        )
        if (action != "set") {
            Column {
                FieldLabel("Current password")
                DsTextField(current, { current = it }, "Current password", password = true)
            }
        }
        if (action != "remove") LockPasswordField(files, "New password", next) { next = it }
        error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    }
}
