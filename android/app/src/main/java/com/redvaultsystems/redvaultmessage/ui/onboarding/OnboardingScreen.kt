// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.onboarding

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.platform.LocalViewConfiguration
import androidx.compose.ui.platform.ViewConfiguration
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.SignupConsent
import com.redvaultsystems.redvaultmessage.data.model.LegalDocument
import com.redvaultsystems.redvaultmessage.ui.AuthStatus
import com.redvaultsystems.redvaultmessage.ui.AuthUiState
import com.redvaultsystems.redvaultmessage.ui.OnboardingMode
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.KeyGlyph
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.recovery.RecoverAccountForm
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

private const val MIN_USERNAME_LENGTH = 3
private const val MAX_USERNAME_LENGTH = 64 // matches the server's registerSchema
private const val MIN_PASSWORD_LENGTH = 8

/** Port of ui/src/screens/Onboarding.tsx: create identity, log in, and the MFA code step, in the app's design. */
@Composable
fun OnboardingScreen(
    state: AuthUiState,
    onRegister: (handle: String, password: String, consent: SignupConsent) -> Unit,
    onLogin: (handle: String, password: String) -> Unit,
    onSubmitMfa: (code: String) -> Unit,
    onCancelMfa: () -> Unit,
    onModeChange: (OnboardingMode) -> Unit,
    onRegenerateHandle: () -> Unit,
    onOpenTransparency: () -> Unit,
    onRecover: (handle: String, recoveryKey: String, newPassword: String, code: String?) -> Unit,
) {
    val colors = RedvaultTheme.colors
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(colors.bg)
            .systemBarsPadding()
            .imePadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 24.dp, vertical = 28.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
    ) {
        Column(Modifier.widthIn(max = 440.dp).fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Wordmark()
            when {
                state.status == AuthStatus.MfaPending -> MfaStep(state, onSubmitMfa, onCancelMfa)
                state.mode == OnboardingMode.Recover -> RecoverAccountForm(state, onRecover, onBack = { onModeChange(OnboardingMode.Login) })
                state.mode == OnboardingMode.Login -> LoginForm(state, onLogin, onModeChange)
                else -> CreateForm(state, onRegister, onModeChange, onRegenerateHandle)
            }
            Spacer(Modifier.height(4.dp))
            LinkText("See the live encrypted network", onOpenTransparency, Modifier.align(Alignment.CenterHorizontally))
        }
    }
}

/** The name, a shield, and the one-line promise. */
@Composable
private fun Wordmark() {
    val colors = RedvaultTheme.colors
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(Modifier.size(40.dp).clip(RoundedCornerShape(12.dp)).background(colors.accent), contentAlignment = Alignment.Center) {
            DsIcon(R.drawable.ui_shield_check, colors.accentContrast, 22.dp)
        }
        Text("Redvaultsystem", fontFamily = RedvaultFonts.display, fontSize = 26.sp, fontWeight = FontWeight.Bold, color = colors.text, letterSpacing = (-0.5).sp)
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        DsIcon(R.drawable.ui_lock, colors.accent, 13.dp)
        Text("End-to-end encrypted · keys stay on this device", color = colors.accent, fontFamily = RedvaultFonts.mono, fontSize = 11.sp)
    }
}

@Composable
private fun Title(text: String, sub: String) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(text, fontFamily = RedvaultFonts.display, fontSize = 32.sp, fontWeight = FontWeight.Bold, color = RedvaultTheme.colors.text, letterSpacing = (-0.6).sp)
        Text(sub, color = RedvaultTheme.colors.text2, fontSize = 15.sp)
    }
}

@Composable
private fun ColumnScope.CreateForm(
    state: AuthUiState,
    onRegister: (String, String, SignupConsent) -> Unit,
    onModeChange: (OnboardingMode) -> Unit,
    onRegenerateHandle: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    var username by rememberSaveable { mutableStateOf(state.candidateHandle) }
    var appliedCandidate by rememberSaveable { mutableStateOf(state.candidateHandle) }
    // Passwords use plain remember, never rememberSaveable: saved instance
    // state can be written to disk by the system.
    var password by remember { mutableStateOf("") }
    var confirmPassword by remember { mutableStateOf("") }
    // The three sign-up boxes: never pre-ticked, and the server refuses the account without all of them.
    var termsAccepted by rememberSaveable { mutableStateOf(false) }
    var privacyAcknowledged by rememberSaveable { mutableStateOf(false) }
    var age18Confirmed by rememberSaveable { mutableStateOf(false) }
    var reading by rememberSaveable { mutableStateOf<LegalDocument?>(null) }
    val consent = SignupConsent(termsAccepted, privacyAcknowledged, age18Confirmed)

    // "Suggest one" replaces the field with the new suggestion;
    // typing a handle of your own afterwards isn't overwritten.
    LaunchedEffect(state.candidateHandle) {
        if (state.candidateHandle != appliedCandidate) {
            appliedCandidate = state.candidateHandle
            username = state.candidateHandle
        }
    }

    val trimmed = username.trim()
    val usernameValid = trimmed.length in MIN_USERNAME_LENGTH..MAX_USERNAME_LENGTH
    val busy = state.status == AuthStatus.Authenticating
    val canSubmit = !busy && usernameValid && password.length >= MIN_PASSWORD_LENGTH && password == confirmPassword && consent.complete

    Title("Create your identity", "No phone number, no email, no contact book.")
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        KeyGlyph(seed = trimmed.ifEmpty { state.candidateHandle }, size = 56.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text("Your key, made on this phone", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            Text("Its private half never leaves your devices unencrypted.", color = colors.text3, fontSize = 12.5.sp)
        }
    }

    Field("Username") {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Input(
                username,
                { if (it.length <= MAX_USERNAME_LENGTH) username = it },
                "e.g. ShadowWolf",
                mono = true,
                modifier = Modifier.weight(1f),
            )
            Text(
                "Suggest",
                color = colors.accent,
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.clip(RoundedCornerShape(12.dp)).background(colors.surface2).clickable(role = Role.Button, onClick = onRegenerateHandle)
                    .padding(horizontal = 14.dp, vertical = 15.dp),
            )
        }
    }
    if (!usernameValid && trimmed.isNotEmpty()) Problem("Handles need to be at least $MIN_USERNAME_LENGTH characters.")
    Field("Password", "At least $MIN_PASSWORD_LENGTH characters") { Input(password, { password = it }, "Password", password = true) }
    Field("Confirm password") { Input(confirmPassword, { confirmPassword = it }, "Confirm password", password = true) }
    if (confirmPassword.isNotEmpty() && confirmPassword != password) Problem("Passwords don't match.")

    ConsentBox(termsAccepted, { termsAccepted = it }, "I agree to the RedVault Messages ", "Terms of Service") { reading = LegalDocument.Terms }
    ConsentBox(privacyAcknowledged, { privacyAcknowledged = it }, "I acknowledge the RedVault Messages ", "Privacy Policy") { reading = LegalDocument.Privacy }
    ConsentBox(age18Confirmed, { age18Confirmed = it }, "I confirm I am 18 or older")
    state.error?.let { Problem(it) }

    PillButton(if (busy) "Creating…" else "Create Account", { onRegister(trimmed, password, consent) }, Modifier.fillMaxWidth(), enabled = canSubmit, busy = busy)
    Callout(
        "There's no password reset by email. After you create your account you'll get a recovery key: it's the only way back in if you forget your password, so keep it safe.",
        CalloutTone.Neutral,
        R.drawable.ui_key,
    )
    LinkText("I already have an identity. Log in", { onModeChange(OnboardingMode.Login) }, Modifier.align(Alignment.CenterHorizontally))

    reading?.let { LegalDocumentDialog(it, onDismiss = { reading = null }) }
}

/**
 * One sign-up box: tapping the row ticks it; [link] (if any) opens its
 * document instead, without ticking anything.
 */
@Composable
private fun ConsentBox(checked: Boolean, onChange: (Boolean) -> Unit, text: String, link: String? = null, onOpenLink: () -> Unit = {}) {
    val colors = RedvaultTheme.colors
    val label = buildAnnotatedString {
        append(text)
        if (link != null) {
            withLink(
                LinkAnnotation.Clickable(
                    tag = link,
                    styles = TextLinkStyles(SpanStyle(color = colors.accent, fontWeight = FontWeight.SemiBold, textDecoration = TextDecoration.Underline)),
                ) { onOpenLink() },
            ) {
                // Kept on one line: a link that wraps is tappable across its whole
                // bounding box, which would swallow taps meant to tick the box.
                append(link.replace(' ', ' '))
            }
        }
    }
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .toggleable(value = checked, role = Role.Checkbox, onValueChange = onChange)
            .padding(vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Checkbox(
            checked = checked,
            onCheckedChange = null,
            colors = CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3),
        )
        // No 48dp touch-target growth for the link: grown, it would reach over the
        // words above it, and a tap meant to tick the box would open the document.
        val base = LocalViewConfiguration.current
        val exact = remember(base) {
            object : ViewConfiguration by base {
                override val minimumTouchTargetSize: DpSize get() = DpSize.Zero
            }
        }
        CompositionLocalProvider(LocalViewConfiguration provides exact) {
            Text(label, color = colors.text2, fontSize = 14.sp, lineHeight = 20.sp, modifier = Modifier.weight(1f))
        }
    }
}

@Composable
private fun ColumnScope.LoginForm(state: AuthUiState, onLogin: (String, String) -> Unit, onModeChange: (OnboardingMode) -> Unit) {
    var handle by rememberSaveable { mutableStateOf(state.lastHandle ?: "") }
    var password by remember { mutableStateOf("") }
    val busy = state.status == AuthStatus.Authenticating
    val canSubmit = !busy && handle.isNotBlank() && password.isNotEmpty()

    Title("Welcome back", "Log in with your handle and password.")
    Field("Handle") { Input(handle, { handle = it }, "Your handle", mono = true) }
    Field("Password") { Input(password, { password = it }, "Password", password = true, onDone = { if (canSubmit) onLogin(handle.trim(), password) }) }
    state.error?.let { Problem(it) }
    PillButton(if (busy) "Logging in…" else "Log in", { onLogin(handle.trim(), password) }, Modifier.fillMaxWidth(), enabled = canSubmit, busy = busy)
    LinkText("Forgot your password? Use your recovery key", { onModeChange(OnboardingMode.Recover) }, Modifier.align(Alignment.CenterHorizontally))
    PillButton("Create a new identity", { onModeChange(OnboardingMode.Create) }, Modifier.fillMaxWidth(), style = PillStyle.Secondary)
}

@Composable
private fun ColumnScope.MfaStep(state: AuthUiState, onSubmit: (String) -> Unit, onCancel: () -> Unit) {
    var code by rememberSaveable { mutableStateOf("") }
    var submitted by rememberSaveable { mutableStateOf(false) }
    // A failed attempt comes back with an error: allow another try.
    LaunchedEffect(state.error) { if (state.error != null) submitted = false }

    Title("Two-factor code", "Enter the 6-digit code from your authenticator app.")
    Input(
        code,
        { code = it.filter(Char::isDigit).take(6) },
        "123 456",
        mono = true,
        number = true,
        large = true,
        onDone = { if (code.length == 6 && !submitted) { submitted = true; onSubmit(code) } },
    )
    state.error?.let { Problem(it) }
    PillButton(
        if (submitted) "Verifying…" else "Verify",
        {
            submitted = true
            onSubmit(code)
        },
        Modifier.fillMaxWidth(),
        enabled = !submitted && code.length == 6,
        busy = submitted,
    )
    LinkText("Back", onCancel, Modifier.align(Alignment.CenterHorizontally))
}

@Composable
private fun Field(label: String, hint: String? = null, content: @Composable () -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(label, color = RedvaultTheme.colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        content()
        if (hint != null) Text(hint, color = RedvaultTheme.colors.text3, fontSize = 12.5.sp)
    }
}

/** The design's input: 12px radius, accent ring when focused. Never autocorrected (handles and passwords). */
@Composable
private fun Input(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    modifier: Modifier = Modifier,
    password: Boolean = false,
    mono: Boolean = false,
    number: Boolean = false,
    large: Boolean = false,
    onDone: (() -> Unit)? = null,
) {
    val colors = RedvaultTheme.colors
    var focused by remember { mutableStateOf(false) }
    val font = if (mono) RedvaultFonts.mono else RedvaultFonts.sans
    val size = if (large) 26.sp else 16.sp
    Box(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(colors.surface2)
            .border(if (focused) 2.dp else 1.dp, if (focused) colors.accent else colors.border, RoundedCornerShape(12.dp))
            .padding(horizontal = 14.dp, vertical = if (large) 16.dp else 14.dp),
        contentAlignment = if (large) Alignment.Center else Alignment.CenterStart,
    ) {
        if (value.isEmpty()) {
            Text(placeholder, color = colors.placeholder, fontSize = size, fontFamily = font, textAlign = if (large) TextAlign.Center else TextAlign.Start, modifier = Modifier.fillMaxWidth())
        }
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            singleLine = true,
            textStyle = TextStyle(color = colors.text, fontSize = size, fontFamily = font, textAlign = if (large) TextAlign.Center else TextAlign.Start, letterSpacing = if (large) 4.sp else 0.sp),
            cursorBrush = SolidColor(colors.accent),
            visualTransformation = if (password) PasswordVisualTransformation() else androidx.compose.ui.text.input.VisualTransformation.None,
            keyboardOptions = KeyboardOptions(
                capitalization = KeyboardCapitalization.None,
                autoCorrectEnabled = false,
                keyboardType = when {
                    number -> KeyboardType.NumberPassword
                    password -> KeyboardType.Password
                    else -> KeyboardType.Ascii
                },
                imeAction = if (onDone != null) androidx.compose.ui.text.input.ImeAction.Done else androidx.compose.ui.text.input.ImeAction.Next,
            ),
            keyboardActions = androidx.compose.foundation.text.KeyboardActions(onDone = { onDone?.invoke() }),
            modifier = Modifier.fillMaxWidth().semantics { contentDescription = placeholder }.onFocusChanged { focused = it.isFocused },
        )
    }
}

@Composable
private fun Problem(text: String) = Callout(text, CalloutTone.Error, R.drawable.ui_alert_circle)

@Composable
private fun LinkText(text: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    Text(
        text,
        color = RedvaultTheme.colors.accent,
        fontSize = 14.sp,
        fontWeight = FontWeight.SemiBold,
        textAlign = TextAlign.Center,
        modifier = modifier.clip(RoundedCornerShape(10.dp)).clickable(role = Role.Button, onClick = onClick).padding(horizontal = 10.dp, vertical = 10.dp),
    )
}

