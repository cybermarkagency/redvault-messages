// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.Context
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.DriveEntry
import com.redvaultsystems.redvaultmessage.data.OwnerShare
import com.redvaultsystems.redvaultmessage.data.ShareOptions
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.DsSwitch
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SheetBar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import java.text.DateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

// Public share links for Files (docs/public-share-links.md): making a link
// to a file or folder that anyone can open in a browser, managing links, and
// reviewing what people uploaded. The wording follows the web and desktop apps.

private const val VIEWER_TEXT = "Anyone with this link can view and download files. Uploading and editing are disabled."
private const val CONTRIBUTOR_TEXT = "Anyone with this link can view and download files, and upload new files. They cannot delete or replace your files."
private const val CONTRIBUTOR_WARNING = "Anyone who receives or is forwarded this link can upload files until you change its permissions, revoke it, or it expires."
private const val PASSWORD_NOTE = "The password isn't part of the link. Send it separately (another app, or in person) so one leaked message isn't enough."
private const val MB = 1024L * 1024L

private enum class Expiry(val label: String, val hours: Long?) {
    Never("Never", null), Hour("1 hour", 1), Day("1 day", 24), Week("7 days", 24 * 7), Month("30 days", 24 * 30), Custom("On a date…", null)
}

private fun positive(text: String, scale: Long = 1): Long? = text.trim().replace(',', '.').toDoubleOrNull()?.takeIf { it > 0 }?.let { (it * scale).toLong() }

/** Bytes as the MB a limit field shows ("5", "2.5"): always with a point, whatever the phone's language. */
private fun mbText(bytes: Long): String = String.format(Locale.ROOT, "%.2f", bytes.toDouble() / MB).trimEnd('0').trimEnd('.')

/** What a limit field accepts: a count, or for sizes a decimal ("2.5", or "2,5" where that's the separator). */
private fun limitInput(text: String, decimal: Boolean): Boolean =
    text.length <= 12 && text.all { it.isDigit() || (decimal && (it == '.' || it == ',')) }

/** A limit with its name shown above it, so a filled-in field still says what it is. */
@Composable
private fun LimitField(label: String, value: String, onChange: (String) -> Unit, hint: String = "Empty: no limit", decimal: Boolean = false) {
    Column {
        FieldLabel(label)
        DsTextField(value, { if (limitInput(it, decimal)) onChange(it) }, hint, label = label)
    }
}

private fun dateText(ms: Long): String = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(ms))

/** The device's share sheet with the link, or nothing if there's none. */
private fun shareSheet(context: Context, url: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, url)
    runCatching { context.startActivity(Intent.createChooser(send, "Share link")) }
}

/** Picks a date, then a time, after now; calls back with epoch ms. */
private fun pickDateTime(context: Context, onPicked: (Long) -> Unit) {
    val now = Calendar.getInstance()
    DatePickerDialog(context, { _, year, month, day ->
        TimePickerDialog(context, { _, hour, minute ->
            val at = Calendar.getInstance().apply { set(year, month, day, hour, minute, 0); set(Calendar.MILLISECOND, 0) }
            onPicked(at.timeInMillis)
        }, now.get(Calendar.HOUR_OF_DAY), now.get(Calendar.MINUTE), true).show()
    }, now.get(Calendar.YEAR), now.get(Calendar.MONTH), now.get(Calendar.DAY_OF_MONTH)).apply { datePicker.minDate = now.timeInMillis }.show()
}

@Composable
private fun Chips(options: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    val colors = RedvaultTheme.colors
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        options.forEachIndexed { i, label ->
            val on = i == selected
            Text(
                label,
                color = if (on) colors.accentContrast else colors.text2,
                fontSize = 14.sp,
                fontWeight = if (on) FontWeight.SemiBold else FontWeight.Normal,
                modifier = Modifier
                    .clip(RoundedCornerShape(16.dp))
                    .background(if (on) colors.accent else colors.surface2)
                    .clickable(role = Role.RadioButton) { onSelect(i) }
                    .padding(horizontal = 14.dp, vertical = 8.dp),
            )
        }
    }
}

@Composable
private fun Tag(text: String, tone: CalloutTone = CalloutTone.Neutral) {
    val colors = RedvaultTheme.colors
    val (bg, fg) = when (tone) {
        CalloutTone.Ok -> colors.accentSoft to colors.accent
        CalloutTone.Error -> colors.dangerSoft to colors.danger
        else -> colors.surface2 to colors.text2
    }
    Text(text, color = fg, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.clip(RoundedCornerShape(11.dp)).background(bg).padding(horizontal = 9.dp, vertical = 3.dp))
}

@Composable
private fun Switch(label: String, detail: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    val colors = RedvaultTheme.colors
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(label, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
            Text(detail, color = colors.text2, fontSize = 13.sp)
        }
        DsSwitch(checked, onChange, label)
    }
}

/** The link that was just made: copy it, or hand it to another app. */
@Composable
private fun LinkResult(state: SharesState, onDone: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val url = state.madeUrl ?: return
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        SheetBar("Link ready", onDone, cancelText = "Done")
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Text(if (state.madePermission == "contributor") CONTRIBUTOR_TEXT else VIEWER_TEXT, color = colors.text2, fontSize = 14.sp)
            Text(
                url,
                color = colors.text,
                fontSize = 13.sp,
                modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(colors.surface2).padding(14.dp),
            )
            if (state.madeHasPassword) Callout(PASSWORD_NOTE, CalloutTone.Neutral, R.drawable.ui_info)
            PillButton("Copy link", { copyText(context, url, secret = false) }, Modifier.fillMaxWidth())
            PillButton("Share…", { shareSheet(context, url) }, Modifier.fillMaxWidth(), style = PillStyle.Secondary)
        }
    }
}

/** Share: the links already made to this item, and a new one. */
@Composable
fun ShareLinkScreen(shares: SharesSession, state: SharesState, entry: DriveEntry?, onBack: () -> Unit, onManage: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    DisposableEffect(Unit) {
        shares.clearMade()
        shares.dismissMessages()
        onDispose { shares.clearMade() }
    }
    if (entry == null) {
        LaunchedEffect(Unit) { onBack() }
        return
    }
    if (state.madeUrl != null) {
        LinkResult(state) { shares.clearMade(); onBack() }
        return
    }
    val folder = !entry.isFile
    var label by rememberSaveable { mutableStateOf("") }
    var contributor by rememberSaveable { mutableStateOf(false) }
    var withPassword by rememberSaveable { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }
    var expiry by rememberSaveable { mutableStateOf(Expiry.Never) }
    var customAt by rememberSaveable { mutableStateOf(0L) }
    var maxDownloads by rememberSaveable { mutableStateOf("") }
    var maxFileMb by rememberSaveable { mutableStateOf("") }
    var allowanceMb by rememberSaveable { mutableStateOf("") }
    var maxFiles by rememberSaveable { mutableStateOf("") }
    val existing = state.linksFor(entry.id)

    FullSheet(
        title = "Share link",
        onCancel = onBack,
        primary = if (state.busy) "Encrypting…" else "Create link",
        primaryEnabled = !state.busy && (!withPassword || password.length >= 8) && (expiry != Expiry.Custom || customAt > System.currentTimeMillis()),
        busy = state.busy,
        onPrimary = {
            val expiresAt = when (expiry) {
                Expiry.Never -> null
                Expiry.Custom -> customAt
                else -> System.currentTimeMillis() + expiry.hours!! * 3_600_000L
            }
            shares.createLink(
                entry,
                ShareOptions(
                    permission = if (contributor && folder) "contributor" else "viewer",
                    label = label.ifBlank { entry.name },
                    password = if (withPassword) password else "",
                    expiresAtMs = expiresAt,
                    maxDownloads = positive(maxDownloads),
                    maxFileSize = positive(maxFileMb, MB),
                    allowance = positive(allowanceMb, MB),
                    maxFiles = positive(maxFiles),
                ),
            )
        },
    ) {
        Text(entry.name, color = colors.text, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
        Text("Anyone with the link can open it in a browser, without an account. It's decrypted in their browser; the server never sees it.", color = colors.text2, fontSize = 13.sp)
        if (existing.isNotEmpty()) {
            FieldLabel("Links you made")
            existing.forEach { s ->
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    DsIcon(R.drawable.ui_link, colors.accent, 16.dp)
                    Text(s.box?.label ?: entry.name, color = colors.text, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                    Tag(if (s.link.permission == "contributor") "Contributor" else "Viewer")
                    Tag(statusText(s), statusTone(s))
                    if (s.active && s.url.isNotEmpty()) TextButton(onClick = { copyText(context, s.url, secret = false) }) { Text("Copy", color = colors.accent) }
                }
            }
            TextButton(onClick = onManage) { Text("Manage links", color = colors.accent) }
        }
        FieldLabel("New link")
        DsTextField(label, { label = it.take(120) }, "Name (only you see it): ${entry.name}")
        FieldLabel("Recipients can")
        OptionCard("View and download", VIEWER_TEXT, !contributor) { contributor = false }
        OptionCard("View, download and upload", if (folder) CONTRIBUTOR_TEXT else "Folder links only: a file link can't take uploads.", contributor && folder) {
            if (folder) contributor = true
        }
        if (contributor && folder) {
            Callout(CONTRIBUTOR_WARNING, CalloutTone.Warn, R.drawable.ui_alert_circle)
            LimitField("Largest file (MB)", maxFileMb, { maxFileMb = it }, decimal = true)
            LimitField("Total uploads (MB)", allowanceMb, { allowanceMb = it }, "Empty: your free space", decimal = true)
            LimitField("Most files", maxFiles, { maxFiles = it })
        }
        Switch("Password", "Needed to open the link, on top of the link itself.", withPassword) { withPassword = it }
        if (withPassword) {
            DsTextField(password, { password = it }, "At least 8 characters", password = true)
            Callout(PASSWORD_NOTE, CalloutTone.Neutral, R.drawable.ui_info)
        }
        FieldLabel("Expires")
        Chips(Expiry.entries.map { it.label }, expiry.ordinal) { i ->
            expiry = Expiry.entries[i]
            if (expiry == Expiry.Custom) pickDateTime(context) { customAt = it }
        }
        if (expiry == Expiry.Custom) {
            Text(
                if (customAt > System.currentTimeMillis()) "Expires ${dateText(customAt)} (your time zone)" else "Pick a date and time in the future.",
                color = if (customAt > System.currentTimeMillis()) colors.text2 else colors.warn,
                fontSize = 13.sp,
                modifier = Modifier.clickable { pickDateTime(context) { customAt = it } },
            )
        }
        LimitField("Download limit", maxDownloads, { maxDownloads = it })
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    }
}

private fun limitReached(s: OwnerShare): Boolean = s.link.maxDownloads != null && s.link.downloadCount >= s.link.maxDownloads

private fun statusText(s: OwnerShare): String = when {
    s.status == "revoked" -> "Revoked"
    s.status == "expired" -> "Expired"
    limitReached(s) -> "Download limit reached"
    else -> "Active"
}

/** Green only while the link still hands out files. */
private fun statusTone(s: OwnerShare): CalloutTone = if (s.active && !limitReached(s)) CalloutTone.Ok else CalloutTone.Error

/** Files › Shared links: every link, what it allows, its state and counts, and what you can do with it. */
@Composable
fun SharedLinksScreen(
    shares: SharesSession,
    state: SharesState,
    files: FilesState,
    onBack: () -> Unit,
    onEdit: (linkId: String, mode: String) -> Unit,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    var confirm by remember { mutableStateOf<Pair<String, OwnerShare>?>(null) } // ("revoke"|"delete", link)
    LaunchedEffect(Unit) { shares.refresh() }
    // Notices belong to this visit: one from here must not greet you in Incoming, nor one from there here.
    DisposableEffect(Unit) { onDispose { shares.dismissMessages() } }
    confirm?.let { (what, s) ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            containerColor = colors.surface,
            title = { Text(if (what == "revoke") "Revoke this link?" else "Delete this link?", color = colors.text) },
            text = {
                Text(
                    if (what == "revoke") "It stops working at once for everyone: no more opening, downloads or uploads. Files people already downloaded stay on their devices; nothing can take those back. This can't be undone."
                    else (if (s.active) "It stops working at once, and " else "It ") + "leaves your list. Files that came in through it stay in Incoming.",
                    color = colors.text2,
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    if (what == "revoke") shares.revoke(s.link.id) else shares.remove(s.link.id)
                    confirm = null
                }) { Text(if (what == "revoke") "Revoke" else "Delete", color = colors.danger) }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel", color = colors.text2) } },
        )
    }
    val order = mapOf("active" to 0, "expired" to 1, "revoked" to 2)
    val links = state.links.sortedWith(compareBy<OwnerShare> { order[it.status] ?: 3 }.thenByDescending { it.box?.created ?: 0 })
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        SheetBar("Shared links", onBack, cancelText = "Back")
        LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item { Text("Links anyone can open in a browser, without an account. Each one is separate: its own permission, password and expiry.", color = colors.text2, fontSize = 14.sp) }
            state.error?.let { item { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle, Modifier.clickable { shares.dismissMessages() }) } }
            state.notice?.let { item { Callout(it, CalloutTone.Ok, R.drawable.ui_check, Modifier.clickable { shares.dismissMessages() }) } }
            if (links.isEmpty()) item { Text(if (state.loaded) "No links yet. Use Share link on a file or folder." else "Loading…", color = colors.text3, fontSize = 14.sp) }
            items(links, key = { it.link.id }) { s ->
                val item = files.find(s.link.backupId)
                val d = s.link
                Column(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).border(1.dp, colors.borderStrong, RoundedCornerShape(16.dp)).padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        DsIcon(if (d.kind == "folder") R.drawable.ui_folder else R.drawable.ui_file, colors.accent, 18.dp)
                        Column(Modifier.weight(1f)) {
                            Text(s.box?.label ?: item?.name ?: "Shared item", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(
                                if (item == null) "The shared item is no longer in your files" else "${if (d.kind == "folder") "Folder" else "File"} · ${item.name}",
                                color = colors.text3, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Tag(if (d.permission == "contributor") "View, download, upload" else "View and download")
                        Tag(statusText(s), statusTone(s))
                        if (d.password != null) Tag("Password")
                    }
                    val facts = buildList {
                        add("Created" to (s.box?.let { dateText(it.created) } ?: "—"))
                        add("Expires" to (d.expiresAt?.let { runCatching { dateText(java.time.Instant.parse(it).toEpochMilli()) }.getOrDefault(it) } ?: "Never"))
                        add("Opened" to "${d.openCount}×")
                        add("Downloads" to (if (d.maxDownloads != null) "${d.downloadCount} of ${d.maxDownloads}" else "${d.downloadCount}"))
                        if (d.permission == "contributor" || d.upload.count > 0) {
                            add("Uploads" to "${d.upload.count} (${sizeText(d.upload.bytes)})" + (if (d.upload.inProgress > 0) " · ${d.upload.inProgress} in progress" else "") + (if (d.incoming > 0) " · ${d.incoming} to review" else ""))
                        }
                    }
                    facts.forEach { (k, v) ->
                        Row { Text(k, color = colors.text3, fontSize = 13.sp, modifier = Modifier.weight(0.35f)); Text(v, color = colors.text, fontSize = 13.sp, modifier = Modifier.weight(0.65f)) }
                    }
                    Text((if (d.permission == "contributor") CONTRIBUTOR_TEXT else VIEWER_TEXT) + " Visitors are anonymous: counts only, no names or addresses.", color = colors.text3, fontSize = 12.sp)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (s.active && s.url.isNotEmpty()) {
                            PillButton("Copy link", { copyText(context, s.url, secret = false) }, style = PillStyle.Secondary)
                            PillButton("Share…", { shareSheet(context, s.url) }, style = PillStyle.Secondary)
                        }
                        if (!d.revoked) {
                            PillButton("Settings", { onEdit(d.id, "settings") }, style = PillStyle.Secondary, enabled = s.box != null && !state.busy)
                            if (d.permission == "contributor") PillButton("Turn off uploads", { shares.turnOffUploads(d.id) }, style = PillStyle.Secondary, enabled = !state.busy)
                            PillButton(if (d.password != null) "Password" else "Add password", { onEdit(d.id, "password") }, style = PillStyle.Secondary, enabled = s.box != null && !state.busy)
                            PillButton("New link", { onEdit(d.id, "regenerate") }, style = PillStyle.Secondary, enabled = s.box != null && !state.busy)
                            PillButton("Revoke", { confirm = "revoke" to s }, style = PillStyle.Danger, enabled = !state.busy)
                        }
                        PillButton("Delete", { confirm = "delete" to s }, style = PillStyle.Danger, enabled = !state.busy)
                    }
                }
            }
        }
    }
}

/** One link's settings, its password, or a new address for it. */
@Composable
fun ShareLinkEditScreen(shares: SharesSession, state: SharesState, linkId: String, mode: String, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val s = state.link(linkId)
    var submitted by remember { mutableStateOf(false) }
    DisposableEffect(Unit) {
        shares.clearMade()
        shares.dismissMessages()
        onDispose { shares.clearMade() }
    }
    if (s == null) {
        LaunchedEffect(Unit) { onBack() }
        return
    }
    if (mode == "regenerate" && state.madeUrl != null) {
        LinkResult(state) { shares.clearMade(); onBack() }
        return
    }
    // Settings and passwords close once saved without an error.
    LaunchedEffect(state.busy, state.error) {
        if (submitted && !state.busy && state.error == null && mode != "regenerate") onBack()
    }
    val d = s.link
    var label by rememberSaveable { mutableStateOf(s.box?.label ?: "") }
    var contributor by rememberSaveable { mutableStateOf(d.permission == "contributor") }
    var expiry by rememberSaveable { mutableStateOf(-1) } // -1: keep
    var customAt by rememberSaveable { mutableStateOf(0L) }
    var maxDownloads by rememberSaveable { mutableStateOf(d.maxDownloads?.toString() ?: "") }
    var maxFileMb by rememberSaveable { mutableStateOf(d.upload.maxFileSize?.let(::mbText) ?: "") }
    var allowanceMb by rememberSaveable { mutableStateOf(d.upload.allowance?.let(::mbText) ?: "") }
    var maxFiles by rememberSaveable { mutableStateOf(d.upload.maxFiles?.toString() ?: "") }
    var password by remember { mutableStateOf("") }
    var withPassword by rememberSaveable { mutableStateOf(d.password != null) }

    FullSheet(
        title = when (mode) { "password" -> if (d.password != null) "Change the password" else "Add a password"; "regenerate" -> "Make a new link"; else -> "Link settings" },
        onCancel = onBack,
        primary = if (state.busy) "Working…" else if (mode == "regenerate") "Make new link" else "Save",
        primaryEnabled = !state.busy && when (mode) {
            "password" -> password.length >= 8
            "regenerate" -> !withPassword || password.length >= 8
            else -> expiry != Expiry.Custom.ordinal || customAt > System.currentTimeMillis()
        },
        busy = state.busy,
        onPrimary = {
            submitted = true
            when (mode) {
                "password" -> shares.setPassword(d.id, password)
                "regenerate" -> shares.regenerate(d.id, if (withPassword) password else "")
                else -> {
                    val expiresAt: Long? = when {
                        expiry < 0 -> null
                        Expiry.entries[expiry] == Expiry.Never -> 0L
                        Expiry.entries[expiry] == Expiry.Custom -> customAt
                        else -> System.currentTimeMillis() + Expiry.entries[expiry].hours!! * 3_600_000L
                    }
                    shares.saveSettings(
                        d.id, label, if (contributor && d.kind == "folder") "contributor" else "viewer", expiresAt,
                        positive(maxDownloads), positive(maxFileMb, MB), positive(allowanceMb, MB), positive(maxFiles),
                    )
                }
            }
        },
    ) {
        when (mode) {
            "password" -> {
                Text("Anyone opening the link will need it. The link itself stays the same; people who already downloaded files keep them.", color = colors.text2, fontSize = 14.sp)
                DsTextField(password, { password = it }, "At least 8 characters", password = true)
                Callout(PASSWORD_NOTE, CalloutTone.Neutral, R.drawable.ui_info)
                if (d.password != null) {
                    TextButton(onClick = { submitted = true; shares.setPassword(d.id, "") }, enabled = !state.busy) { Text("Remove password", color = colors.danger) }
                }
            }
            "regenerate" -> {
                Text("The current link stops working at once, including downloads in progress. Its settings and counts stay. Files people already downloaded stay with them.", color = colors.text2, fontSize = 14.sp)
                Switch("Password", if (d.password != null) "Type it again (or a new one): it isn't stored anywhere." else "Needed to open the new link.", withPassword) { withPassword = it }
                if (withPassword) DsTextField(password, { password = it }, "At least 8 characters", password = true)
            }
            else -> {
                DsTextField(label, { label = it.take(120) }, "Name (only you see it)")
                if (d.kind == "folder") {
                    Switch("Recipients can upload", if (contributor) CONTRIBUTOR_TEXT else VIEWER_TEXT, contributor) { contributor = it }
                    if (d.permission == "contributor" && !contributor) {
                        Callout("Uploads stop at once: any still in progress are cancelled. Files already received stay in Incoming.", CalloutTone.Neutral, R.drawable.ui_info)
                    }
                    if (contributor) {
                        LimitField("Largest file (MB)", maxFileMb, { maxFileMb = it }, decimal = true)
                        LimitField("Total uploads (MB)", allowanceMb, { allowanceMb = it }, "Empty: your free space", decimal = true)
                        LimitField("Most files", maxFiles, { maxFiles = it })
                    }
                }
                FieldLabel("Expires" + (d.expiresAt?.let { " (now: ${runCatching { dateText(java.time.Instant.parse(it).toEpochMilli()) }.getOrDefault(it)})" } ?: " (now: never)"))
                Chips(listOf("Keep") + Expiry.entries.map { it.label }, expiry + 1) { i ->
                    expiry = i - 1
                    if (expiry == Expiry.Custom.ordinal) pickDateTime(context) { customAt = it }
                }
                if (expiry == Expiry.Custom.ordinal) {
                    Text(
                        if (customAt > System.currentTimeMillis()) "Expires ${dateText(customAt)} (your time zone)" else "Pick a date and time in the future.",
                        color = if (customAt > System.currentTimeMillis()) colors.text2 else colors.warn,
                        fontSize = 13.sp,
                        modifier = Modifier.clickable { pickDateTime(context) { customAt = it } },
                    )
                }
                LimitField("Download limit", maxDownloads, { maxDownloads = it })
            }
        }
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    }
}

/** Files › Incoming: files people uploaded through your Contributor links, to review. */
@Composable
fun IncomingScreen(shares: SharesSession, state: SharesState, files: FilesState, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    var saving by remember { mutableStateOf<String?>(null) }
    var accepting by remember { mutableStateOf<String?>(null) }
    var rejecting by remember { mutableStateOf<String?>(null) }
    var risky by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) { shares.refresh() }
    DisposableEffect(Unit) { onDispose { shares.dismissMessages() } }
    val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val id = saving
        saving = null
        if (uri != null && id != null) shares.downloadIncoming(id) { context.contentResolver.openOutputStream(uri) }
    }
    fun save(id: String) {
        val file = state.incomingFile(id) ?: return
        saving = id
        saver.launch(file.name)
    }
    val labels = state.links.associate { it.link.id to (it.box?.label ?: "a share link") }

    risky?.let { id ->
        val file = state.incomingFile(id)
        AlertDialog(
            onDismissRequest = { risky = null },
            containerColor = colors.surface,
            title = { Text("Save a file that can run code?", color = colors.text) },
            text = { Text("“${file?.name}” is a kind of file that can run programs or scripts when opened. Someone anonymous uploaded it. Only open it if you know what it is.", color = colors.text2) },
            confirmButton = { TextButton(onClick = { risky = null; save(id) }) { Text("Save anyway", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { risky = null }) { Text("Cancel", color = colors.text2) } },
        )
    }
    rejecting?.let { id ->
        val file = state.incomingFile(id)
        AlertDialog(
            onDismissRequest = { rejecting = null },
            containerColor = colors.surface,
            title = { Text("Reject this file?", color = colors.text) },
            text = { Text("“${file?.name}” is deleted for good and stops counting toward your storage. Whoever uploaded it isn't told.", color = colors.text2) },
            confirmButton = { TextButton(onClick = { rejecting = null; shares.rejectIncoming(id) }) { Text("Reject", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { rejecting = null }) { Text("Cancel", color = colors.text2) } },
        )
    }
    accepting?.let { id ->
        val file = state.incomingFile(id)
        val byId = files.snapshot.entries.associateBy { it.id }
        val folders = files.snapshot.entries.filter { !it.isFile && !it.trashed }.map { f ->
            val parts = mutableListOf(f.name)
            var p = byId[f.parent]
            var depth = 0
            while (p != null && depth < 64) { parts.add(0, p.name); p = byId[p.parent]; depth++ }
            f.id to parts.joinToString(" / ")
        }.sortedBy { it.second.lowercase() }
        AlertDialog(
            onDismissRequest = { accepting = null },
            containerColor = colors.surface,
            title = { Text("Add “${file?.name}” to your files", color = colors.text) },
            text = {
                LazyColumn {
                    item { Text("It's moved, not copied: it stays encrypted, now with your own Files key. Choose a folder:", color = colors.text2, fontSize = 14.sp, modifier = Modifier.padding(bottom = 8.dp)) }
                    items(listOf("" to "My files (top level)") + folders) { (folderId, path) ->
                        Text(path, color = colors.text, fontSize = 15.sp, modifier = Modifier.fillMaxWidth().clickable { accepting = null; shares.acceptIncoming(id, folderId) }.padding(vertical = 12.dp))
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { accepting = null }) { Text("Cancel", color = colors.text2) } },
        )
    }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        SheetBar("Incoming", onBack, cancelText = "Back")
        LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            item { Text("Files people uploaded through your links. Review them, then add them to your files or reject them.", color = colors.text2, fontSize = 14.sp) }
            item {
                Callout(
                    "Anyone with a Contributor link can upload, and uploaders are anonymous. These files are encrypted for you alone, so RedVault can't scan them for malware: only open what you expect.",
                    CalloutTone.Warn,
                    R.drawable.ui_alert_circle,
                )
            }
            state.error?.let { item { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle, Modifier.clickable { shares.dismissMessages() }) } }
            state.notice?.let { item { Callout(it, CalloutTone.Ok, R.drawable.ui_check, Modifier.clickable { shares.dismissMessages() }) } }
            if (state.incoming.isEmpty()) item { Text(if (state.loaded) "Nothing waiting for review." else "Loading…", color = colors.text3, fontSize = 14.sp) }
            items(state.incoming, key = { it.dto.id }) { f ->
                val openable = f.meta != null
                val size = f.meta?.get("size")?.toString()?.toLongOrNull() ?: f.dto.encryptedSize
                val sent = f.meta?.get("sent")?.toString()?.toLongOrNull()
                Column(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(colors.surface).padding(14.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        DsIcon(if (SharesSession.isRisky(f.name)) R.drawable.ui_alert_circle else R.drawable.ui_file, if (SharesSession.isRisky(f.name)) colors.warn else colors.accent, 18.dp)
                        Column(Modifier.weight(1f)) {
                            Text(if (openable) f.name else "Can't be opened on this phone", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(
                                listOfNotNull(sizeText(size), sent?.let { dateText(it) }, "through " + (f.dto.shareId?.let { labels[it] } ?: "a link you deleted"), f.folder.takeIf { it.isNotEmpty() }?.let { "from $it/" })
                                    .joinToString(" · "),
                                color = colors.text3, fontSize = 12.sp,
                            )
                        }
                    }
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        PillButton("Save a copy", { if (SharesSession.isRisky(f.name)) risky = f.dto.id else save(f.dto.id) }, style = PillStyle.Secondary, enabled = openable && !state.busy)
                        PillButton("Add to Files", { accepting = f.dto.id }, style = PillStyle.Secondary, enabled = openable && !state.busy)
                        PillButton("Reject", { rejecting = f.dto.id }, style = PillStyle.Danger, enabled = !state.busy)
                    }
                }
            }
        }
    }
}
