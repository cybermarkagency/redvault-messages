package com.redvaultsystems.redvaultmessage.ui.lock

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.AppLockStore
import com.redvaultsystems.redvaultmessage.data.LockTimeout
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.NoticeTone
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.SettingsRow
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.TouchGuard
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Covers the whole app until the right PIN is entered. */
@Composable
fun LockScreen(lock: AppLock, onLockedOut: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    val length = lock.store.pinLength
    var pin by remember { mutableStateOf("") }
    var checking by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf<String?>(null) }
    var confirmForget by remember { mutableStateOf(false) }

    fun submit(entered: String) {
        checking = true
        scope.launch {
            when (val result = lock.unlock(entered)) {
                UnlockResult.Unlocked -> {}
                is UnlockResult.Wrong -> message =
                    if (result.attemptsLeft <= 5) "Wrong PIN. ${result.attemptsLeft} tries left before you're logged out." else "Wrong PIN."
                UnlockResult.LockedOut -> onLockedOut()
            }
            pin = ""
            checking = false
        }
    }

    fun press(digit: Char) {
        if (checking || pin.length >= length) return
        pin += digit
        message = null
        if (pin.length == length) submit(pin)
    }

    Column(
        modifier = Modifier.fillMaxSize().background(colors.bg).systemBarsPadding().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(20.dp, Alignment.CenterVertically),
    ) {
        Text("Redvaultsystem", fontFamily = FontFamily.Serif, fontSize = 26.sp, color = colors.text)
        Text(if (checking) "Checking…" else "Enter your PIN", color = colors.text2)
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            repeat(length) { i ->
                Box(
                    Modifier.size(14.dp).then(
                        if (i < pin.length) Modifier.background(colors.accent, CircleShape)
                        else Modifier.border(1.5.dp, colors.text3, CircleShape),
                    ),
                )
            }
        }
        message?.let { Text(it, color = colors.warn, textAlign = TextAlign.Center) }
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            listOf("123", "456", "789").forEach { row ->
                Row(horizontalArrangement = Arrangement.spacedBy(18.dp)) { row.forEach { KeyButton(it.toString()) { press(it) } } }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(18.dp)) {
                Box(Modifier.size(72.dp))
                KeyButton("0") { press('0') }
                Box(Modifier.size(72.dp).clickable(enabled = pin.isNotEmpty() && !checking) { pin = pin.dropLast(1) }, contentAlignment = Alignment.Center) {
                    Icon(painterResource(R.drawable.ic_backspace), contentDescription = "Delete", tint = colors.text2, modifier = Modifier.size(26.dp))
                }
            }
        }
        TextButton(onClick = { confirmForget = true }) { Text("Forgot PIN?") }
    }

    if (confirmForget) {
        AlertDialog(
            onDismissRequest = { confirmForget = false },
            title = { Text("Forgot your PIN?") },
            text = {
                TouchGuard()
                Text("You'll be logged out and the PIN removed. Log back in with your password, then set a new PIN in Settings.")
            },
            confirmButton = {
                TextButton(onClick = {
                    lock.forget()
                    onLockedOut()
                }) { Text("Log out") }
            },
            dismissButton = { TextButton(onClick = { confirmForget = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun KeyButton(label: String, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Box(
        Modifier.size(72.dp).background(colors.surface2, CircleShape).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, fontSize = 26.sp, color = colors.text, fontWeight = FontWeight.Medium)
    }
}

/** Settings > App lock: set, change or remove the PIN, and pick the timeout. */
@Composable
fun AppLockSettingsScreen(lock: AppLock, onBack: () -> Unit) {
    val store = lock.store
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var enabled by remember { mutableStateOf(store.enabled) }
    var timeout by remember { mutableStateOf(store.timeout) }
    // Changing or removing an existing PIN asks for it first.
    var currentPin by remember { mutableStateOf("") }
    var newPin by remember { mutableStateOf("") }
    var confirmPin by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var done by remember { mutableStateOf<String?>(null) }

    suspend fun currentPinOk(): Boolean {
        if (!store.enabled) return true
        val ok = withContext(Dispatchers.Default) { store.verify(currentPin) }
        if (!ok) error = "Your current PIN is wrong."
        return ok
    }

    SubScreen(title = "App lock", onBack = onBack) {
        Text(
            "Ask for a PIN when you come back to the app after it's been in the background. " +
                "The PIN is stored only on this phone, as a one-way hash. After ${AppLockStore.MAX_ATTEMPTS} wrong tries you're logged out.",
            color = colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        done?.let { Notice(it, NoticeTone.Success) }
        error?.let { Notice(it) }

        if (enabled) {
            SectionLabel("Lock after")
            LockTimeout.entries.forEach { option ->
                Row(
                    Modifier.fillMaxWidth().clickable {
                        timeout = option
                        store.timeout = option
                    },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    RadioButton(selected = timeout == option, onClick = {
                        timeout = option
                        store.timeout = option
                    })
                    Text(option.label, color = colors.text)
                }
            }
            SectionLabel("Current PIN")
            PinField(currentPin, { currentPin = it }, "Current PIN")
        }

        SectionLabel(if (enabled) "Change PIN" else "Set a PIN")
        PinField(newPin, { newPin = it }, "New PIN (${AppLockStore.MIN_PIN_LENGTH}–${AppLockStore.MAX_PIN_LENGTH} digits)")
        PinField(confirmPin, { confirmPin = it }, "Confirm new PIN")
        if (confirmPin.isNotEmpty() && confirmPin != newPin) Notice("PINs don't match.")
        PrimaryButton(
            text = if (enabled) "Change PIN" else "Turn on app lock",
            enabled = AppLockStore.isValidPin(newPin) && newPin == confirmPin && (!enabled || currentPin.isNotEmpty()),
            busy = busy,
        ) {
            busy = true
            error = null
            done = null
            scope.launch {
                if (currentPinOk()) {
                    withContext(Dispatchers.Default) { store.setPin(newPin) }
                    lock.pinChanged()
                    done = if (enabled) "PIN changed." else "App lock is on."
                    enabled = true
                    currentPin = ""
                    newPin = ""
                    confirmPin = ""
                }
                busy = false
            }
        }

        if (enabled) {
            TextButton(enabled = currentPin.isNotEmpty() && !busy, onClick = {
                busy = true
                error = null
                done = null
                scope.launch {
                    if (currentPinOk()) {
                        store.clear()
                        lock.pinChanged()
                        enabled = false
                        currentPin = ""
                        done = "App lock is off."
                    }
                    busy = false
                }
            }) { Text("Turn off app lock", color = colors.warn) }
        }
    }
}

@Composable
private fun PinField(value: String, onValueChange: (String) -> Unit, label: String) {
    OutlinedTextField(
        value = value,
        onValueChange = { onValueChange(it.filter(Char::isDigit).take(AppLockStore.MAX_PIN_LENGTH)) },
        label = { Text(label) },
        singleLine = true,
        visualTransformation = PasswordVisualTransformation(),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
        modifier = Modifier.fillMaxWidth().widthIn(max = 420.dp),
    )
}

/** Settings row summarizing the lock state. */
@Composable
fun AppLockRow(enabled: Boolean, timeout: LockTimeout, onClick: () -> Unit) {
    SettingsRow(
        title = "App lock",
        description = if (enabled) "On · ${timeout.label.lowercase()}" else "Off. Require a PIN to open the app",
        onClick = onClick,
        icon = com.redvaultsystems.redvaultmessage.R.drawable.ui_lock,
    )
}
