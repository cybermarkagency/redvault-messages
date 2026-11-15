// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.recovery

import androidx.compose.ui.res.stringResource
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.ui.AuthStatus
import com.redvaultsystems.redvaultmessage.ui.AuthUiState
import com.redvaultsystems.redvaultmessage.ui.account.AccountSession
import com.redvaultsystems.redvaultmessage.ui.account.Profile
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.NoticeTone
import com.redvaultsystems.redvaultmessage.ui.components.PasswordInput
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.SecureWindow
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.TouchGuard
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

private const val MIN_PASSWORD_LENGTH = 8

/** Shown once after sign-up: the new recovery key, until the user confirms they've saved it. */
@Composable
fun RecoveryKeyIssuedScreen(recoveryKey: String, onDone: () -> Unit) {
    TouchGuard()
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(RedvaultTheme.colors.bg)
            .systemBarsPadding()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
    ) {
        Text(stringResource(R.string.ui_your_recovery_key), style = MaterialTheme.typography.titleLarge, color = RedvaultTheme.colors.text)
        RecoveryKeyContent(recoveryKey, onDone)
    }
}

/** The key itself, Copy, and "I've saved it". Blocks screenshots while it's on screen. */
@Composable
fun ColumnScope.RecoveryKeyContent(recoveryKey: String, onDone: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    var saved by remember { mutableStateOf(false) }
    var copied by remember { mutableStateOf(false) }
    SecureWindow()

    Text(
        "If you ever forget your password, this key is the only way back into your account and messages. " +
            "Write it down or keep it in a password manager. It won't be shown again, and nobody (including us) can recover it for you.",
        color = colors.text2,
        style = MaterialTheme.typography.bodyMedium,
        textAlign = TextAlign.Center,
    )
    Text(
        recoveryKey.split("-").chunked(4).joinToString("\n") { it.joinToString("-") },
        fontFamily = FontFamily.Monospace,
        fontSize = 18.sp,
        textAlign = TextAlign.Center,
        color = colors.text,
        modifier = Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(16.dp),
    )
    TextButton(onClick = {
        SecretClipboard.copy(context, recoveryKey, secret = true)
        copied = true
    }) { Text(if (copied) "Copied. Clears in 30 seconds" else "Copy") }
    Row(
        Modifier.fillMaxWidth().clickable { saved = !saved },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Checkbox(checked = saved, onCheckedChange = { saved = it })
        Text(stringResource(R.string.ui_i_ve_saved_my_recovery_key), color = colors.text)
    }
    PrimaryButton(text = stringResource(R.string.ui_done), enabled = saved, onClick = onDone)
}

/** "Forgot your password?": recover with the recovery key and choose a new password. */
@Composable
fun RecoverAccountForm(
    state: AuthUiState,
    onRecover: (handle: String, recoveryKey: String, newPassword: String, code: String?) -> Unit,
    onBack: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    // The recovery key is typed here, before sign-in, so no signed-in screen covers it.
    SecureWindow()
    TouchGuard()
    var handle by rememberSaveable { mutableStateOf(state.lastHandle ?: "") }
    // Secrets use plain remember: never saved instance state.
    var key by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    val busy = state.status == AuthStatus.Authenticating
    val valid = handle.isNotBlank() && key.isNotBlank() && password.length >= MIN_PASSWORD_LENGTH && password == confirm &&
        (!state.recoverNeedsCode || code.length == 6)

    Text(stringResource(R.string.ui_recover_your_account_with_your_recovery), color = colors.text2, textAlign = TextAlign.Center)
    Column(Modifier.widthIn(max = 420.dp).fillMaxWidth().imePadding(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        DsTextField(
            handle,
            { handle = it },
            "Handle",
            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
        )
        DsTextField(
            key,
            { key = it },
            "Recovery key (XXXX-XXXX-…)",
            mono = true,
            singleLine = false,
            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Characters, autoCorrectEnabled = false, keyboardType = KeyboardType.Password),
        )
        PasswordInput(password, { password = it }, "New password (min. $MIN_PASSWORD_LENGTH characters)")
        PasswordInput(confirm, { confirm = it }, "Confirm new password")
        if (state.recoverNeedsCode) {
            DsTextField(
                code,
                { code = it.filter(Char::isDigit).take(6) },
                "6-digit code from your authenticator app",
                mono = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
            )
        }
        if (confirm.isNotEmpty() && confirm != password) Notice(stringResource(R.string.ui_passwords_don_t_match))
        state.error?.let { Notice(it) }
        PrimaryButton(text = if (busy) "Recovering…" else "Recover account", enabled = valid, busy = busy) {
            onRecover(handle.trim(), key, password, code.takeIf { state.recoverNeedsCode })
        }
        Text(
            stringResource(R.string.ui_this_signs_out_every_other_device),
            color = colors.text3,
            style = MaterialTheme.typography.bodySmall,
        )
    }
    TextButton(onClick = onBack) { Text(stringResource(R.string.ui_back_to_log_in)) }
}

/** Settings > Recovery key: create one, or replace the existing one. */
@Composable
fun RecoveryKeySettingsScreen(account: AccountSession, profile: Profile, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    var password by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var issued by remember { mutableStateOf<String?>(null) }

    SubScreen(title = stringResource(R.string.ui_recovery_key), onBack = onBack) {
        val shown = issued
        if (shown != null) {
            RecoveryKeyContent(shown, onDone = onBack)
            return@SubScreen
        }
        Text(
            when (profile.hasRecoveryKey) {
                null -> "Checking…"
                true -> "Your account has a recovery key. Creating a new one replaces it: the old key stops working."
                false -> "Your account has no recovery key. Without one, a forgotten password can't be recovered."
            },
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        if (profile.hasRecoveryKey == false) Notice(stringResource(R.string.ui_create_a_recovery_key_now_so), NoticeTone.Warn)
        PasswordInput(password, { password = it }, "Current password")
        error?.let { Notice(it) }
        PrimaryButton(
            text = if (profile.hasRecoveryKey == true) "Create a new recovery key" else "Create a recovery key",
            enabled = password.isNotEmpty(),
            busy = busy,
        ) {
            busy = true
            error = null
            scope.launch {
                try {
                    issued = account.createRecoveryKey(password)
                    password = ""
                } catch (e: Exception) {
                    error = describeError(e, "Couldn't create a recovery key.")
                } finally {
                    busy = false
                }
            }
        }
    }
}
