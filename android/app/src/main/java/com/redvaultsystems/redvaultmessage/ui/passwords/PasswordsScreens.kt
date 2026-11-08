// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.passwords

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
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
import com.redvaultsystems.redvaultmessage.crypto.GeneratorOptions
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.DsSwitch
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.TabHeader
import com.redvaultsystems.redvaultmessage.ui.components.TouchGuard
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.files.FieldLabel
import com.redvaultsystems.redvaultmessage.ui.files.FullSheet
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.files.ShareContact
import com.redvaultsystems.redvaultmessage.ui.files.StrengthBar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

private enum class VaultFilter(val label: String) {
    All("All"), Logins("Logins"), Passkeys("Passkeys"), Cards("Cards"), Notes("Notes"), Wifi("Wi-Fi"), Shared("Shared"),
    Exposed("Exposed"), Reused("Reused"), Weak("Weak"),
}

private val ITEM_TYPES = listOf("login" to "Login", "passkey" to "Passkey", "card" to "Payment card", "note" to "Secure note", "wifi" to "Wi-Fi network")

/** Any touch inside the vault counts as use, so it doesn't lock under the user's finger. */
private fun Modifier.touchesVault(vault: VaultSession) = pointerInput(vault) {
    awaitEachGesture {
        awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
        vault.touch()
    }
}

/** The Passwords tab: lock status, the lock screen or the security check, categories and items. */
@Composable
fun PasswordsTab(vault: VaultSession, state: VaultState, onOpen: (String) -> Unit, onNew: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    var search by rememberSaveable { mutableStateOf("") }
    var filterName by rememberSaveable { mutableStateOf(VaultFilter.All.name) }
    val filter = VaultFilter.valueOf(filterName)
    var picking by remember { mutableStateOf(false) }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().touchesVault(vault)) {
        TabHeader("Passwords") {
            if (!state.locked) Box {
                RoundButton(
                    R.drawable.ui_plus, "New item", onClick = { picking = true },
                    container = colors.accent, tint = colors.accentContrast,
                )
                DropdownMenu(expanded = picking, onDismissRequest = { picking = false }, containerColor = colors.surface2) {
                    ITEM_TYPES.forEach { (type, label) ->
                        DropdownMenuItem(text = { Text(label, color = colors.text) }, onClick = { picking = false; onNew(type) })
                    }
                }
            }
        }
        LockBar(state.locked, onLock = vault::lockNow)
        if (state.locked) {
            LockedVault(vault, state)
            return@Column
        }

        Column(Modifier.padding(horizontal = 20.dp).padding(top = 12.dp, bottom = 8.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SearchBox(search, { search = it; vault.touch() }, if (state.loaded) "Search ${state.rows.size} ${if (state.rows.size == 1) "item" else "items"}" else "Search", R.drawable.ui_search)
            SecurityCheck(state, filter) { picked -> filterName = (if (filter == picked) VaultFilter.All else picked).name }
        }
        ChipRow(
            options = listOf(VaultFilter.All, VaultFilter.Logins, VaultFilter.Passkeys, VaultFilter.Cards, VaultFilter.Notes, VaultFilter.Wifi, VaultFilter.Shared),
            selected = filter,
            label = { it.label },
            onSelect = { filterName = it.name },
        )
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle, Modifier.padding(horizontal = 20.dp, vertical = 6.dp)) }

        val shown = state.rows
            .filter { matches(it, filter) }
            .filter { row ->
                val q = search.trim().lowercase()
                q.isEmpty() || titleOf(row.entry).lowercase().contains(q) || subtitleOf(row.entry).lowercase().contains(q)
            }
            .sortedBy { titleOf(it.entry).lowercase() }
        when {
            !state.loaded -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { Text("Opening your vault…", color = colors.text3) }
            shown.isEmpty() -> EmptyVault(
                if (search.isNotBlank()) "Nothing matches that." else if (state.rows.isEmpty()) "Your vault is empty. Tap + to save a login, card, note or Wi-Fi password." else "Nothing here.",
            )
            else -> LazyColumn(Modifier.fillMaxSize().padding(top = 6.dp)) {
                items(shown, key = { it.entry.id + (if (it.entry.incoming) ":in" else "") }) { row -> VaultRowItem(row) { onOpen(row.entry.id) } }
            }
        }
    }
}

private fun matches(row: VaultRow, filter: VaultFilter): Boolean {
    val e = row.entry
    return when (filter) {
        VaultFilter.All -> true
        VaultFilter.Logins -> !e.incoming && e.type == "login"
        VaultFilter.Passkeys -> !e.incoming && hasPasskey(e)
        VaultFilter.Cards -> !e.incoming && e.type == "card"
        VaultFilter.Notes -> !e.incoming && e.type == "note"
        VaultFilter.Wifi -> !e.incoming && e.type == "wifi"
        VaultFilter.Shared -> e.incoming
        VaultFilter.Exposed -> row.status == ItemStatus.Exposed
        VaultFilter.Reused -> row.status == ItemStatus.Reused
        VaultFilter.Weak -> row.status == ItemStatus.Weak
    }
}

@Composable
private fun LockBar(locked: Boolean, onLock: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier
            .padding(horizontal = 20.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(if (locked) colors.surface2 else Color(0xFF16271F))
            .border(1.dp, if (locked) colors.borderStrong else Color(0xFF244536), RoundedCornerShape(14.dp))
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        val ink = if (locked) colors.text2 else colors.accent
        DsIcon(if (locked) R.drawable.ui_lock else R.drawable.ui_lock_open, ink, 18.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(1.dp)) {
            Text(if (locked) "Locked" else "Unlocked · unlimited items", color = ink, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            Text(if (locked) "Unlock to view items" else "Locks after 5 min idle", color = colors.text3, fontSize = 12.sp)
        }
        if (!locked) {
            Text(
                "Lock now",
                color = colors.text,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier
                    .height(34.dp)
                    .clip(RoundedCornerShape(10.dp))
                    .background(colors.surface3)
                    .clickable(role = Role.Button, onClick = onLock)
                    .padding(horizontal = 12.dp, vertical = 8.dp),
            )
        }
    }
}

/** The lock screen: fingerprint / face, or the account password. No item data is on screen or in memory. */
@Composable
private fun LockedVault(vault: VaultSession, state: VaultState) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val biometrics = remember { Biometrics.available(context) }
    var usePassword by rememberSaveable { mutableStateOf(!biometrics) }
    var password by remember { mutableStateOf("") }
    var biometricError by remember { mutableStateOf<String?>(null) }
    fun promptBiometrics() {
        biometricError = null
        Biometrics.prompt(context) { ok, error ->
            if (ok) vault.unlockedWithBiometrics()
            else if (error == null) usePassword = true
            else biometricError = error
        }
    }
    Column(
        Modifier.fillMaxSize().imePadding().verticalScroll(rememberScrollState()).padding(horizontal = 32.dp, vertical = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
    ) {
        Spacer(Modifier.height(24.dp))
        Box(
            Modifier.size(84.dp).clip(CircleShape).background(colors.surface2).border(1.dp, colors.borderStrong, CircleShape),
            contentAlignment = Alignment.Center,
        ) { DsIcon(R.drawable.ui_lock, colors.accent, 36.dp) }
        Text("Vault locked", fontFamily = RedvaultFonts.display, fontSize = 26.sp, fontWeight = FontWeight.Bold, color = colors.text)
        Text(
            "Unlock to see your passwords. Nothing in the vault can be read until you do — not even by us.",
            color = colors.text3,
            fontSize = 14.sp,
            textAlign = TextAlign.Center,
        )
        if (!usePassword) {
            PillButton("Unlock with face or fingerprint", ::promptBiometrics, Modifier.fillMaxWidth(), icon = R.drawable.ui_fingerprint)
            biometricError?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
            Text(
                "Use master password",
                color = colors.accent,
                fontSize = 14.sp,
                fontWeight = FontWeight.Medium,
                modifier = Modifier.clickable(role = Role.Button) { usePassword = true }.padding(8.dp),
            )
        } else {
            DsTextField(password, { password = it; vault.clearMessages() }, "Your account password", password = true)
            state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
            PillButton(
                "Unlock",
                { scope.launch { if (vault.unlockWithPassword(password)) password = "" } },
                Modifier.fillMaxWidth(),
                enabled = password.isNotEmpty(),
                busy = state.busy,
            )
            if (biometrics) {
                Text(
                    "Use face or fingerprint",
                    color = colors.accent,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Medium,
                    modifier = Modifier.clickable(role = Role.Button) { usePassword = false; promptBiometrics() }.padding(8.dp),
                )
            }
        }
    }
    // Offer the fingerprint straight away, like the desktop's Windows Hello.
    LaunchedEffect(Unit) { if (biometrics && !usePassword) promptBiometrics() }
}

@Composable
private fun SecurityCheck(state: VaultState, filter: VaultFilter, onPick: (VaultFilter) -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(colors.surface).padding(horizontal = 14.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Security check", color = colors.text, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            Text(
                when {
                    state.breachChecked -> "Checked privately"
                    state.breachUnavailable -> "Breach check unavailable"
                    else -> "Checking…"
                },
                color = if (state.breachChecked) colors.accent else colors.text3,
                fontSize = 12.sp,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf(
                Triple(VaultFilter.Exposed, state.exposed, true),
                Triple(VaultFilter.Reused, state.reused, false),
                Triple(VaultFilter.Weak, state.weak, false),
            ).forEach { (kind, count, danger) ->
                val on = filter == kind
                val ink = when {
                    count == 0 -> colors.text3
                    danger -> colors.danger
                    else -> colors.warn
                }
                val fill = when {
                    count == 0 -> colors.surface2
                    danger -> colors.dangerSoft
                    else -> colors.warnSoft
                }
                Column(
                    Modifier
                        .weight(1f)
                        .height(52.dp)
                        .clip(RoundedCornerShape(12.dp))
                        .background(fill)
                        .then(if (on) Modifier.border(1.5.dp, ink, RoundedCornerShape(12.dp)) else Modifier)
                        .clickable(role = Role.Button) { onPick(kind) }
                        .semantics { contentDescription = "$count ${kind.label.lowercase()}" },
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    Text("$count", color = ink, fontFamily = MonoFamily, fontSize = 17.sp, fontWeight = FontWeight.Medium)
                    Text(kind.label, color = ink, fontSize = 11.5.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        }
    }
}

/** An item's square tile: its first letter on a tint picked from its name. */
@Composable
private fun ItemTile(title: String, size: androidx.compose.ui.unit.Dp) {
    Box(
        Modifier.size(size).clip(RoundedCornerShape(size / 4)).background(RedvaultTheme.tintFor(title)),
        contentAlignment = Alignment.Center,
    ) {
        Text(title.take(1).uppercase(), color = RedvaultTheme.colors.avatarInk, fontWeight = FontWeight.Bold, fontSize = (size.value * 0.4f).sp)
    }
}

@Composable
private fun VaultRowItem(row: VaultRow, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    val title = titleOf(row.entry)
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 20.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        ItemTile(title, 40.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            val sub = if (row.entry.incoming) "Shared with you · ${subtitleOf(row.entry)}".trimEnd(' ', '·') else subtitleOf(row.entry)
            if (sub.isNotBlank()) Text(sub, color = colors.text3, fontSize = 12.5.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        StatusBadge(row.status)
    }
}

@Composable
private fun StatusBadge(status: ItemStatus) {
    val colors = RedvaultTheme.colors
    val (text, fill, ink) = when (status) {
        ItemStatus.Exposed -> Triple("Exposed", Color(0xFF3A1F1C), colors.danger)
        ItemStatus.Reused -> Triple("Reused", colors.warnSoft, colors.warn)
        ItemStatus.Weak -> Triple("Weak", colors.warnSoft, colors.warn)
        ItemStatus.None -> return
    }
    Text(
        text,
        color = ink,
        fontSize = 11.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.clip(RoundedCornerShape(11.dp)).background(fill).padding(horizontal = 8.dp, vertical = 3.dp),
    )
}

@Composable
private fun EmptyVault(text: String) {
    Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
        Text(text, color = RedvaultTheme.colors.text3, fontSize = 14.sp, textAlign = TextAlign.Center)
    }
}

// --- One item ---

/** One item: fields with copy buttons, password shown only on request, one-time code, generator, and sharing. */
@Composable
fun PasswordItemScreen(
    vault: VaultSession,
    state: VaultState,
    id: String,
    contacts: List<ShareContact>,
    onBack: () -> Unit,
    onEdit: () -> Unit,
    onShare: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val row = state.find(id)
    if (state.locked || row == null) {
        LaunchedEffect(state.locked, row == null) { onBack() }
        return
    }
    val entry = row.entry
    val d = entry.data
    val type = entry.type
    val title = titleOf(entry)
    // Passwords are hidden again when you leave the item.
    var revealed by remember(id) { mutableStateOf(false) }
    var genOptions by remember { mutableStateOf(GeneratorOptions()) }
    var generated by remember { mutableStateOf(vault.generate(GeneratorOptions())) }
    fun copy(text: String, secret: Boolean) {
        if (text.isEmpty()) return
        SecretClipboard.copy(context, text, secret)
        vault.notice(if (secret) "Copied. It clears from the clipboard in 30 seconds." else "Copied.")
    }
    val website = VaultSession.firstUrl(d)
    val secret = vault.secretOf(id)

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding().touchesVault(vault)) {
        Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 12.dp, top = 8.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back to passwords", onClick = onBack, container = Color.Transparent, iconSize = 24.dp)
            Spacer(Modifier.weight(1f))
            if (!entry.incoming) {
                Text(
                    "Edit",
                    color = colors.accent,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.clickable(role = Role.Button, onClick = onEdit).padding(horizontal = 12.dp, vertical = 12.dp),
                )
            }
        }
        Column(
            Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                ItemTile(title, 56.dp)
                Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Text(title, fontFamily = RedvaultFonts.display, fontSize = 26.sp, fontWeight = FontWeight.Bold, color = colors.text)
                    val host = VaultSession.hostOf(website)
                    val sub = when {
                        host.isNotEmpty() -> host
                        else -> ITEM_TYPES.firstOrNull { it.first == type }?.second.orEmpty()
                    }
                    if (sub.isNotEmpty()) Text(sub, fontFamily = MonoFamily, fontSize = 13.sp, color = colors.text3)
                }
            }
            if (row.status == ItemStatus.Exposed) {
                Callout(
                    "This password has appeared in a data breach. Change it on the site, then save the new one here.",
                    CalloutTone.Error, R.drawable.ui_alert_circle,
                )
            } else if (row.status == ItemStatus.Reused) {
                val n = vault.reusedWith(id)
                Callout("You use this password on $n other ${if (n == 1) "item" else "items"}. Give each site its own.", CalloutTone.Warn, R.drawable.ui_repeat)
            }

            FieldsCard {
                if (type == "login" || type == "passkey") {
                    val username = d.strField("username")
                    if (username.isNotEmpty()) CopyRow("Username", username, mono = true) { copy(username, false) }
                }
                if (type == "card") {
                    val holder = d.nested2("card", "holder")
                    if (holder.isNotEmpty()) CopyRow("Name on card", holder) { copy(holder, false) }
                    val number = d.nested2("card", "number")
                    if (number.isNotEmpty()) SecretRow("Card number", number, revealed, { revealed = !revealed }) { copy(number, true) }
                    val expiry = d.nested2("card", "expiry")
                    if (expiry.isNotEmpty()) CopyRow("Expires", expiry, mono = true, copyable = false) {}
                    val cvv = d.nested2("card", "cvv")
                    if (cvv.isNotEmpty()) SecretRow("Security code", cvv, revealed, { revealed = !revealed }) { copy(cvv, true) }
                }
                if (type == "wifi") {
                    val ssid = d.nested2("wifi", "ssid")
                    if (ssid.isNotEmpty()) CopyRow("Network name", ssid, mono = true) { copy(ssid, false) }
                    val security = d.nested2("wifi", "security")
                    if (security.isNotEmpty()) CopyRow("Security", security, copyable = false) {}
                }
                if (secret.isNotEmpty() && (type == "login" || type == "wifi")) {
                    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        SecretLine("Password", secret, revealed, { revealed = !revealed }) { copy(secret, true) }
                        val strength = vault.strength(secret)
                        StrengthBar(strength?.strength, secret.length, words = false)
                    }
                    Divider()
                }
                TotpRow(vault, id) { code -> copy(code.replace(" ", ""), true) }
                val rpId = d.nested2("passkey", "rpId")
                if (rpId.isNotEmpty()) {
                    Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        DsIcon(R.drawable.ui_key, colors.accent, 20.dp)
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("Passkey saved", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                            Text("For $rpId · sign in without a password", color = colors.text3, fontSize = 12.5.sp)
                        }
                    }
                    Divider()
                }
                if (website.isNotEmpty()) CopyRow("Website", website, mono = true) { copy(website, false) }
                val notes = d.strField("notes")
                if (notes.isNotEmpty()) {
                    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                        Text("Notes", fontSize = 12.sp, color = colors.text3)
                        Text(notes, fontSize = 14.sp, color = colors.text)
                    }
                }
            }

            if (!entry.incoming && (type == "login" || type == "wifi")) {
                GeneratorCard(
                    generated = generated,
                    options = genOptions,
                    onOptions = { genOptions = it; generated = vault.generate(it) },
                    onRegenerate = { generated = vault.generate(genOptions) },
                    onCopy = { copy(generated, true) },
                    onUse = {
                        val fields = vault.fieldsFor(id) ?: return@GeneratorCard
                        val next = if (type == "wifi") fields.copy(wifiPassword = generated) else fields.copy(password = generated)
                        // Saved straight away, like the desktop; the old password is gone from this item.
                        scope.launch { if (vault.save(next) != null) generated = vault.generate(genOptions) }
                    },
                )
            }

            Column(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(horizontal = 14.dp, vertical = 4.dp),
            ) {
                if (entry.incoming) {
                    val owner = contacts.firstOrNull { it.userId == entry.ownerId }?.handle ?: "Someone"
                    InfoLine("Shared by", owner)
                } else {
                    val names = entry.sharedWith.map { uid -> contacts.firstOrNull { it.userId == uid }?.handle ?: "Someone" }
                    InfoLine("Shared with", if (names.isEmpty()) "No one" else names.joinToString(", "))
                }
                InfoLine("Last changed", dayText(VaultSession.run { d.long("passwordChanged") ?: d.long("updated") } ?: 0))
                if (secret.isNotEmpty()) {
                    InfoLine(
                        "Breach check",
                        when {
                            row.status == ItemStatus.Exposed -> "Found in a breach"
                            state.breachChecked -> "Checked privately"
                            state.breachUnavailable -> "Unavailable right now"
                            else -> "Checking…"
                        },
                        ink = if (row.status == ItemStatus.Exposed) colors.danger else colors.accent,
                    )
                }
            }
            state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
            state.notice?.let { Callout(it, CalloutTone.Ok, R.drawable.ui_check) }
            Spacer(Modifier.height(4.dp))
        }
        if (!entry.incoming) {
            PillButton("Share securely", onShare, Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp), icon = R.drawable.ui_share)
        }
    }
    DisposableEffect(id) { onDispose { vault.clearMessages() } }
}

@Composable
private fun FieldsCard(content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(RedvaultTheme.colors.surface), content = content)
}

@Composable
private fun Divider() {
    Box(Modifier.fillMaxWidth().height(1.dp).background(RedvaultTheme.colors.border))
}

@Composable
private fun CopyRow(label: String, value: String, mono: Boolean = false, copyable: Boolean = true, onCopy: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(Modifier.fillMaxWidth().heightIn(min = 60.dp).padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(label, fontSize = 12.sp, color = colors.text3)
            Text(value, fontSize = 14.sp, color = colors.text, fontFamily = if (mono) MonoFamily else RedvaultFonts.sans, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
        if (copyable) SquareButton(R.drawable.ui_copy, "Copy ${label.lowercase()}", onClick = onCopy)
    }
    Divider()
}

@Composable
private fun SecretRow(label: String, value: String, revealed: Boolean, onToggle: () -> Unit, onCopy: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp)) { SecretLine(label, value, revealed, onToggle, onCopy) }
    Divider()
}

@Composable
private fun SecretLine(label: String, value: String, revealed: Boolean, onToggle: () -> Unit, onCopy: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(label, fontSize = 12.sp, color = colors.text3)
            Text(
                if (revealed) value else "•".repeat(minOf(value.length, 16)),
                fontFamily = MonoFamily,
                fontSize = 15.sp,
                letterSpacing = 0.6.sp,
                color = colors.text,
                maxLines = if (revealed) 3 else 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        SquareButton(if (revealed) R.drawable.ui_eye_off else R.drawable.ui_eye, if (revealed) "Hide ${label.lowercase()}" else "Show ${label.lowercase()}", on = revealed, onClick = onToggle)
        SquareButton(R.drawable.ui_copy, "Copy ${label.lowercase()}", onClick = onCopy)
    }
}

@Composable
private fun SquareButton(icon: Int, description: String, on: Boolean = false, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Box(
        Modifier
            .size(44.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(if (on) colors.accentSoft else colors.surface3)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center,
    ) { DsIcon(icon, if (on) colors.accent else colors.text, 17.dp) }
}

/** The one-time code, refreshed every second with a countdown ring. Computed on the phone; the setup key never leaves it. */
@Composable
private fun TotpRow(vault: VaultSession, id: String, onCopy: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    var now by remember { mutableLongStateOf(System.currentTimeMillis() / 1000) }
    LaunchedEffect(id) {
        while (true) {
            now = System.currentTimeMillis() / 1000
            delay(1000 - System.currentTimeMillis() % 1000)
        }
    }
    val (code, left, period) = vault.totp(id, now) ?: return
    Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text("One-time code", fontSize = 12.sp, color = colors.text3)
            Text(code, fontFamily = MonoFamily, fontSize = 22.sp, fontWeight = FontWeight.Medium, letterSpacing = 1.7.sp, color = colors.accent)
        }
        Box(Modifier.size(36.dp).semantics { contentDescription = "$left seconds left" }, contentAlignment = Alignment.Center) {
            val track = colors.borderStrong
            val ring = if (left <= 5) colors.warn else colors.accent
            Canvas(Modifier.size(36.dp)) {
                val stroke = 3.dp.toPx()
                val inset = stroke / 2 + 1.5.dp.toPx()
                val arc = Size(size.width - inset * 2, size.height - inset * 2)
                drawArc(track, 0f, 360f, false, Offset(inset, inset), arc, style = Stroke(stroke))
                drawArc(ring, -90f, 360f * left / period, false, Offset(inset, inset), arc, style = Stroke(stroke, cap = StrokeCap.Round))
            }
            Text("$left", fontFamily = MonoFamily, fontSize = 11.sp, color = colors.text2)
        }
        SquareButton(R.drawable.ui_copy, "Copy code") { onCopy(code) }
    }
    Divider()
}

@Composable
private fun GeneratorCard(
    generated: String,
    options: GeneratorOptions,
    onOptions: (GeneratorOptions) -> Unit,
    onRegenerate: () -> Unit,
    onCopy: () -> Unit,
    onUse: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    var confirming by remember { mutableStateOf(false) }
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(horizontal = 14.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Generate a new password", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            SquareButton(R.drawable.ui_refresh, "Generate another", onClick = onRegenerate)
        }
        Text(
            generated,
            fontFamily = MonoFamily,
            fontSize = 14.sp,
            letterSpacing = 0.4.sp,
            color = colors.text,
            modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(colors.surface2).padding(horizontal = 12.dp, vertical = 10.dp),
        )
        GeneratorOptionsRow(options, onOptions)
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            PillButton("Copy", onCopy, Modifier.weight(1f), style = PillStyle.Secondary, height = 42.dp, icon = R.drawable.ui_copy)
            PillButton("Use this one", { confirming = true }, Modifier.weight(1f), style = PillStyle.Secondary, height = 42.dp)
        }
    }
    if (confirming) {
        AlertDialog(
            onDismissRequest = { confirming = false },
            containerColor = colors.surface2,
            title = { Text("Replace the saved password?", color = colors.text) },
            text = {
                TouchGuard()
                Text("Change it on the site too, or you won't be able to sign in with the new one.", color = colors.text2)
            },
            confirmButton = { TextButton(onClick = { confirming = false; onUse() }) { Text("Replace", color = colors.accent) } },
            dismissButton = { TextButton(onClick = { confirming = false }) { Text("Cancel", color = colors.text2) } },
        )
    }
}

/** Length, character sets or words: the desktop generator's options. */
@Composable
private fun GeneratorOptionsRow(options: GeneratorOptions, onOptions: (GeneratorOptions) -> Unit) {
    val colors = RedvaultTheme.colors
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(if (options.words) "${options.length} words" else "${options.length} characters", color = colors.text2, fontSize = 13.sp, modifier = Modifier.weight(1f))
            val min = if (options.words) 4 else 8
            val max = if (options.words) 12 else 64
            SquareButton(R.drawable.ui_minus, "Shorter") { if (options.length > min) onOptions(options.copy(length = options.length - 1)) }
            SquareButton(R.drawable.ui_plus, "Longer") { if (options.length < max) onOptions(options.copy(length = options.length + 1)) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OptionToggle("Words", options.words) { onOptions(if (it) options.copy(words = true, length = 6) else options.copy(words = false, length = 24)) }
            if (!options.words) {
                OptionToggle("A-Z", options.upper) { onOptions(options.copy(upper = it)) }
                OptionToggle("0-9", options.digits) { onOptions(options.copy(digits = it)) }
                OptionToggle("!#$", options.symbols) { onOptions(options.copy(symbols = it)) }
            }
        }
    }
}

@Composable
private fun OptionToggle(label: String, on: Boolean, onChange: (Boolean) -> Unit) {
    val colors = RedvaultTheme.colors
    Text(
        label,
        color = if (on) colors.accentContrast else colors.text2,
        fontSize = 13.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier
            .clip(RoundedCornerShape(16.dp))
            .background(if (on) colors.accent else colors.surface2)
            .clickable(role = Role.Checkbox) { onChange(!on) }
            .semantics { contentDescription = "$label ${if (on) "on" else "off"}" }
            .padding(horizontal = 12.dp, vertical = 7.dp),
    )
}

@Composable
private fun InfoLine(label: String, value: String, ink: Color = RedvaultTheme.colors.text) {
    Row(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        Text(label, color = RedvaultTheme.colors.text3, fontSize = 14.sp, modifier = Modifier.weight(1f))
        Text(value, color = ink, fontSize = 14.sp, textAlign = TextAlign.End, modifier = Modifier.weight(1.4f))
    }
}

private fun dayText(ms: Long): String {
    if (ms <= 0) return "—"
    val day = Calendar.getInstance().apply { timeInMillis = ms }
    val thisYear = Calendar.getInstance().get(Calendar.YEAR)
    return if (day.get(Calendar.YEAR) == thisYear) SimpleDateFormat("MMM d", Locale.getDefault()).format(Date(ms)) else day.get(Calendar.YEAR).toString()
}

private fun kotlinx.serialization.json.JsonObject.strField(key: String) = with(VaultSession) { str(key) }
private fun kotlinx.serialization.json.JsonObject.nested2(obj: String, key: String) = with(VaultSession) { nested(obj, key) }

// --- Editing ---

/** New or edited item: the fields for its type, with the generator for passwords. */
@Composable
fun PasswordEditSheet(vault: VaultSession, state: VaultState, id: String?, newType: String, onDone: (savedId: String?) -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    val initial = remember(id) { (id?.let { vault.fieldsFor(it) }) ?: vault.blankFields(newType) }
    var f by remember(id) { mutableStateOf(initial) }
    var deleting by remember { mutableStateOf(false) }
    if (state.locked) {
        LaunchedEffect(Unit) { onDone(null) }
        return
    }
    val typeName = ITEM_TYPES.firstOrNull { it.first == f.type }?.second?.lowercase() ?: "item"
    FullSheet(
        title = (if (id == null) "New " else "Edit ") + typeName,
        onCancel = { vault.clearMessages(); onDone(null) },
        primary = "Save",
        primaryEnabled = f.title.isNotBlank(),
        busy = state.busy,
        onPrimary = { scope.launch { vault.save(f)?.let { onDone(it) } } },
    ) {
        Column(Modifier.touchesVault(vault), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Field("Name") { DsTextField(f.title, { f = f.copy(title = it) }, "Name") }
            if (f.type == "login" || f.type == "passkey") Field("Username or email") { DsTextField(f.username, { f = f.copy(username = it) }, "you@example.com") }
            if (f.type == "login") PasswordField("Password", f.password, vault) { f = f.copy(password = it) }
            if (f.type == "login" || f.type == "passkey") Field("Website") { DsTextField(f.url, { f = f.copy(url = it) }, "example.com", mono = true) }
            if (f.type == "login") Field("One-time code setup key (optional)") { DsTextField(f.totp, { f = f.copy(totp = it) }, "The key or otpauth:// link the site shows", mono = true) }
            if (f.type == "passkey") {
                Field("Site (relying party)") { DsTextField(f.passkeyRpId, { f = f.copy(passkeyRpId = it) }, "example.com", mono = true) }
                Field("Account name on the passkey") { DsTextField(f.passkeyUser, { f = f.copy(passkeyUser = it) }, "you@example.com") }
            }
            if (f.type == "card") {
                Field("Name on card") { DsTextField(f.cardHolder, { f = f.copy(cardHolder = it) }, "Name on card") }
                Field("Card number") { DsTextField(f.cardNumber, { f = f.copy(cardNumber = it) }, "Card number", mono = true) }
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Column(Modifier.weight(1f)) { Field("Expires (MM/YY)") { DsTextField(f.cardExpiry, { f = f.copy(cardExpiry = it) }, "MM/YY", mono = true) } }
                    Column(Modifier.weight(1f)) { Field("Security code") { DsTextField(f.cardCvv, { f = f.copy(cardCvv = it) }, "CVV", mono = true, password = true) } }
                }
            }
            if (f.type == "wifi") {
                Field("Network name (SSID)") { DsTextField(f.wifiSsid, { f = f.copy(wifiSsid = it) }, "Network name") }
                Field("Security") { DsTextField(f.wifiSecurity, { f = f.copy(wifiSecurity = it) }, "WPA3") }
                PasswordField("Password", f.wifiPassword, vault) { f = f.copy(wifiPassword = it) }
            }
            Field("Notes") { DsTextField(f.notes, { f = f.copy(notes = it) }, if (f.type == "note") "Write your note" else "Optional", singleLine = false, minHeight = 110.dp) }
            state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
            Callout("Encrypted on this phone before it's saved. It syncs to your other devices; we can't read it.", CalloutTone.Neutral, R.drawable.ui_shield)
            if (id != null) {
                PillButton("Delete", { deleting = true }, Modifier.fillMaxWidth(), style = PillStyle.Danger, icon = R.drawable.ui_trash)
            }
        }
    }
    if (deleting && id != null) {
        AlertDialog(
            onDismissRequest = { deleting = false },
            containerColor = colors.surface2,
            title = { Text("Delete “${f.title.ifBlank { "this item" }}”?", color = colors.text) },
            text = {
                TouchGuard()
                Text("It's removed from all your devices, and from anyone you shared it with.", color = colors.text2)
            },
            confirmButton = { TextButton(onClick = { deleting = false; vault.delete(id); onDone("") }) { Text("Delete", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { deleting = false }) { Text("Cancel", color = colors.text2) } },
        )
    }
}

@Composable
private fun Field(label: String, content: @Composable () -> Unit) {
    Column {
        FieldLabel(label)
        content()
    }
}

/** A password with show/hide, the generator and its strength. */
@Composable
private fun PasswordField(label: String, value: String, vault: VaultSession, onChange: (String) -> Unit) {
    var shown by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        FieldLabel(label)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            DsTextField(value, onChange, label, modifier = Modifier.weight(1f), password = !shown, mono = true)
            SquareButton(if (shown) R.drawable.ui_eye_off else R.drawable.ui_eye, if (shown) "Hide password" else "Show password", on = shown) { shown = !shown }
            SquareButton(R.drawable.ui_refresh, "Generate a password") { onChange(vault.generate(GeneratorOptions())) }
        }
        StrengthBar(vault.strength(value)?.strength, value.length, words = false)
    }
}

// --- Sharing ---

/** Share an item with people you chat with: they get their own sealed copy, kept in step when you edit it. */
@Composable
fun PasswordShareSheet(vault: VaultSession, state: VaultState, id: String, contacts: List<ShareContact>, onDone: () -> Unit) {
    val colors = RedvaultTheme.colors
    val row = state.find(id)
    if (state.locked || row == null) {
        LaunchedEffect(Unit) { onDone() }
        return
    }
    FullSheet(title = "Share securely", onCancel = onDone, primary = "Done", primaryEnabled = true, busy = state.busy, onPrimary = onDone) {
        Text(titleOf(row.entry), color = colors.text, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
        Callout(
            "They get their own copy, sealed to them on this phone, and it stays in step when you edit it. They can see the password, so share only with people you trust.",
            CalloutTone.Neutral, R.drawable.ui_info,
        )
        if (contacts.isEmpty()) {
            Text("Chat with someone first to share with them.", color = colors.text3, fontSize = 14.sp)
        }
        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface)) {
            contacts.forEach { contact ->
                val shared = contact.userId in row.entry.sharedWith
                Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    ItemTile(contact.handle, 36.dp)
                    Text(contact.handle, color = colors.text, fontSize = 15.sp, modifier = Modifier.weight(1f))
                    DsSwitch(shared, { on -> if (on) vault.share(id, contact.userId, contact.handle) else vault.unshare(id, contact.userId) }, "Share with ${contact.handle}")
                }
            }
        }
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        state.notice?.let { Callout(it, CalloutTone.Ok, R.drawable.ui_check) }
        Spacer(Modifier.width(1.dp))
    }
}
