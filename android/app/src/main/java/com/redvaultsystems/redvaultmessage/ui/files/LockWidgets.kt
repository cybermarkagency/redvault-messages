// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.crypto.Strength
import com.redvaultsystems.redvaultmessage.data.DriveEntry
import com.redvaultsystems.redvaultmessage.data.DriveRepository
import com.redvaultsystems.redvaultmessage.data.FileKey
import com.redvaultsystems.redvaultmessage.data.FileLocks
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.SheetBar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/** The type tile's label and tint (the design's per-type colours). */
fun typeOf(name: String, folder: Boolean): Pair<String, Color> {
    if (folder) return "DIR" to Color(0xFF86D9AE)
    val ext = name.substringAfterLast('.', "").lowercase()
    val label = if (ext.isEmpty()) "FILE" else ext.take(4).uppercase()
    val tint = when (ext) {
        "pdf" -> 0xFFF2C38B
        "xlsx", "xls", "csv", "ods", "numbers" -> 0xFFB9D98C
        "jpg", "jpeg", "png", "gif", "webp", "heic", "bmp", "svg", "tif", "tiff" -> 0xFF9CC9E8
        "doc", "docx", "txt", "md", "rtf", "odt", "pages", "ppt", "pptx", "key" -> 0xFF9FB4FF
        "mp4", "mov", "webm", "mkv", "avi", "m4v" -> 0xFFE8A7A0
        else -> 0xFFC9CFCA
    }
    return label to Color(tint)
}

/** A file's tile: its type, or a lock while it's locked. */
@Composable
fun FileTile(name: String, folder: Boolean, lockedNow: Boolean, size: Dp = 44.dp) {
    val colors = RedvaultTheme.colors
    if (lockedNow) {
        Box(Modifier.size(size).clip(RoundedCornerShape(12.dp)).background(colors.lockBg), contentAlignment = Alignment.Center) {
            DsIcon(R.drawable.ui_lock, colors.lockText2, 18.dp, description = "Locked")
        }
        return
    }
    if (folder) {
        Box(Modifier.size(size).clip(RoundedCornerShape(12.dp)).background(colors.surface2), contentAlignment = Alignment.Center) {
            DsIcon(R.drawable.ui_folder, RedvaultTheme.tintFor(name, colors), 20.dp)
        }
        return
    }
    val (label, tint) = typeOf(name, false)
    Box(Modifier.size(size).clip(RoundedCornerShape(12.dp)).background(colors.surface2), contentAlignment = Alignment.Center) {
        Text(label, color = tint, fontFamily = RedvaultFonts.mono, fontSize = if (size > 60.dp) 22.sp else 10.sp, fontWeight = FontWeight.Medium)
    }
}

/** A file's lock status line, as on the Files list ("Locked with key · 7F3A-19C2"). */
@Composable
fun LockLine(entry: DriveEntry, unlocked: Boolean, fallback: String) {
    val colors = RedvaultTheme.colors
    val (text, color) = when {
        unlocked -> "Unlocked for this session" to colors.accent
        FileLocks.lockKind(entry.meta) == "key" -> "Locked with key · ${FileLocks.lockFingerprint(entry.meta).take(9)}" to colors.lockText2
        FileLocks.lockKind(entry.meta) == "password" -> "Locked with a password" to colors.lockText2
        else -> fallback to colors.text3
    }
    Text(text, color = color, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
}

/** A text input in the design's style (raised, 12px radius). */
@Composable
fun DsTextField(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    modifier: Modifier = Modifier,
    password: Boolean = false,
    mono: Boolean = false,
    singleLine: Boolean = true,
    minHeight: Dp = 50.dp,
    label: String = placeholder,
) {
    val colors = RedvaultTheme.colors
    var focused by remember { mutableStateOf(false) }
    Box(
        modifier
            .fillMaxWidth()
            .heightIn(min = minHeight)
            .background(colors.surface2, RoundedCornerShape(12.dp))
            .border(if (focused) 2.dp else 1.dp, if (focused) colors.accent else colors.border, RoundedCornerShape(12.dp))
            .padding(horizontal = 14.dp, vertical = 14.dp),
        contentAlignment = if (singleLine) Alignment.CenterStart else Alignment.TopStart,
    ) {
        if (value.isEmpty()) Text(placeholder, color = colors.placeholder, fontSize = 15.sp, fontFamily = if (mono) RedvaultFonts.mono else RedvaultFonts.sans)
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            singleLine = singleLine,
            textStyle = TextStyle(color = colors.text, fontSize = if (mono) 14.sp else 15.sp, fontFamily = if (mono) RedvaultFonts.mono else RedvaultFonts.sans),
            cursorBrush = SolidColor(colors.accent),
            visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
            keyboardOptions = if (password) KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false) else KeyboardOptions.Default,
            modifier = Modifier
                .fillMaxWidth()
                .semantics { contentDescription = label }
                .onFocusChanged { focused = it.isFocused },
        )
    }
}

/** A label over a field. */
@Composable
fun FieldLabel(text: String) {
    Text(text, color = RedvaultTheme.colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(bottom = 6.dp))
}

/** The strength bar: Strong accent full, Fair warning 60%, Weak danger 28%. */
@Composable
fun StrengthBar(strength: Strength?, length: Int, words: Boolean) {
    if (strength == null) return
    val colors = RedvaultTheme.colors
    val (fraction, tone) = when (strength) {
        Strength.Strong -> 1f to colors.accent
        Strength.Fair -> 0.6f to colors.warn
        Strength.Weak -> 0.28f to colors.danger
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(Modifier.weight(1f).height(5.dp).clip(RoundedCornerShape(3.dp)).background(colors.text.copy(alpha = 0.12f))) {
            Box(Modifier.fillMaxWidth(fraction).height(5.dp).clip(RoundedCornerShape(3.dp)).background(tone))
        }
        Text(
            if (strength == Strength.Weak) "Weak" else "${strength.name} · " + if (words) "6 words" else "$length characters",
            color = tone,
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

/** A password for a key or file: the generator's suggestion (24 characters or 6 words), regenerate, strength, and the weak-password error. */
@Composable
fun LockPasswordField(files: FilesSession, label: String, value: String, onValueChange: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    var words by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { if (value.isEmpty()) onValueChange(files.suggestPassword(false)) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            Row(Modifier.clip(RoundedCornerShape(10.dp)).background(colors.surface2).padding(3.dp)) {
                listOf(false to "24 characters", true to "6 words").forEach { (isWords, text) ->
                    val on = words == isWords
                    Text(
                        text,
                        color = if (on) colors.text else colors.text2,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier
                            .clip(RoundedCornerShape(8.dp))
                            .background(if (on) colors.surface3 else Color.Transparent)
                            .clickable(role = Role.Tab) {
                                words = isWords
                                onValueChange(files.suggestPassword(isWords))
                            }
                            .semantics { selected = on }
                            .padding(horizontal = 10.dp, vertical = 6.dp),
                    )
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            DsTextField(value, onValueChange, label, Modifier.weight(1f), mono = true)
            Box(
                Modifier.size(50.dp).clip(RoundedCornerShape(12.dp)).background(colors.surface2)
                    .clickable(role = Role.Button) { onValueChange(files.suggestPassword(words)) }
                    .semantics { contentDescription = "Generate another" },
                contentAlignment = Alignment.Center,
            ) { DsIcon(R.drawable.ui_refresh, colors.text, 20.dp) }
        }
        StrengthBar(files.strength(value), value.length, words)
        val problem = if (value.isEmpty()) null else files.passwordProblem(value)
        if (problem != null) Text(problem, color = colors.danger, fontSize = 13.sp)
    }
}

/** Chooses one of the person's keys: name and fingerprint in the lock colours. */
@Composable
fun KeyPicker(keys: List<FileKey>, selectedId: String, onSelect: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    var open by remember { mutableStateOf(false) }
    val current = keys.firstOrNull { it.id == selectedId }
    Box {
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 56.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(colors.lockBg)
                .border(1.dp, colors.lockBorder, RoundedCornerShape(14.dp))
                .clickable(role = Role.DropdownList) { open = true }
                .semantics { contentDescription = "Key: ${current?.name ?: "none chosen"}" }
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            DsIcon(R.drawable.ui_key, colors.lockText2, 18.dp)
            Column(Modifier.weight(1f)) {
                Text(
                    current?.name ?: if (keys.isEmpty()) "No keys yet: create or import one" else "Choose a key",
                    color = if (current != null) colors.text else colors.text3,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                if (current != null) Text(current.fingerprint, color = colors.lockText2, fontFamily = RedvaultFonts.mono, fontSize = 12.sp)
            }
            DsIcon(R.drawable.ui_chevron_down, colors.lockText, 16.dp)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }, containerColor = colors.surface) {
            keys.forEach { key ->
                DropdownMenuItem(
                    text = {
                        Column {
                            Text(key.name, color = colors.text)
                            Text(key.fingerprint, color = colors.text3, fontFamily = RedvaultFonts.mono, fontSize = 12.sp)
                        }
                    },
                    onClick = {
                        open = false
                        onSelect(key.id)
                    },
                )
            }
        }
    }
}

/** A radio option card ("Lock with a key"). */
@Composable
fun OptionCard(title: String, detail: String, selected: Boolean, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(if (selected) colors.accentSoft else colors.surface)
            .border(1.dp, if (selected) colors.accent else colors.borderStrong, RoundedCornerShape(16.dp))
            .clickable(role = Role.RadioButton, onClick = onClick)
            .semantics { this.selected = selected }
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Box(Modifier.size(20.dp).border(2.dp, if (selected) colors.accent else colors.text3, CircleShape), contentAlignment = Alignment.Center) {
            if (selected) Box(Modifier.size(9.dp).background(colors.accent, CircleShape))
        }
        Column(Modifier.weight(1f)) {
            Text(title, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
            Text(detail, color = colors.text2, fontSize = 13.sp)
        }
    }
}

/** A full-screen sheet: Cancel on the left, the title centred, and the primary action as a 52px pill at the bottom. */
@Composable
fun FullSheet(
    title: String,
    onCancel: () -> Unit,
    primary: String,
    primaryEnabled: Boolean,
    busy: Boolean,
    onPrimary: () -> Unit,
    primaryDanger: Boolean = false,
    content: @Composable ColumnScope.() -> Unit,
) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().imePadding()) {
        SheetBar(title, onCancel)
        Column(
            Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
            content = content,
        )
        PillButton(
            primary,
            onPrimary,
            modifier = Modifier.fillMaxWidth().navigationBarsPadding().padding(horizontal = 20.dp, vertical = 14.dp),
            enabled = primaryEnabled,
            busy = busy,
            style = if (primaryDanger) com.redvaultsystems.redvaultmessage.ui.components.PillStyle.Danger else com.redvaultsystems.redvaultmessage.ui.components.PillStyle.Primary,
        )
    }
}

/** The uploads-and-storage size text ("1.2 MB"). */
fun sizeText(bytes: Long): String = DriveRepository.formatBytes(bytes)

val MonoFamily: FontFamily get() = RedvaultFonts.mono
