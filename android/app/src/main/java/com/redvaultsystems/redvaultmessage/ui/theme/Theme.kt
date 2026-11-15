// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R

// Design tokens for the RedVaultMessages redesign (the same values as the
// desktop's Theme.qml): graphite surfaces (#101114, #17191E, #20232A), a
// restrained burgundy accent (#A72640) and crisp dividers; dark by default,
// plus a light palette with the same roles. Settings > Appearance picks
// System, Dark or Light (AppearanceStore).
//
// The burgundy is too dark to read as text on graphite, so it has two roles:
// accentFill (white text on it) for buttons, outgoing bubbles, badges and
// selections, and accent, a lighter raspberry for text and icons. "ok"
// (green) is only for confirmed states (Verified, Tor connected); danger is
// coral so an error never looks like the accent. Every state colour is paired
// with an icon or words where it's used.

@Immutable
data class RedvaultColors(
    val bg: Color,
    /** Conversation header and message bar. */
    val bar: Color,
    /** Cards, settings groups, system notices. */
    val surface: Color,
    /** "raised": inputs, secondary buttons, chips. */
    val surface2: Color,
    /** "control": call control buttons. */
    val surface3: Color,
    /** Highlighted row, incoming bubble. */
    val selected: Color,
    val text: Color,
    val text2: Color,
    val text3: Color,
    val placeholder: Color,
    val border: Color,
    val borderStrong: Color,
    val accent: Color,
    val accentSoft: Color,
    val accentBorder: Color,
    val accentContrast: Color,
    /** Burgundy fills: primary buttons, badges, selected marks; accentContrast on top. */
    val accentFill: Color,
    /** Keyboard / switch-access focus outline. */
    val focusRing: Color,
    /** Charcoal incoming bubble. */
    val bubbleIn: Color,
    val bubbleOut: Color,
    /** Text on an outgoing bubble. */
    val bubbleOutText: Color,
    val bubbleOutTime: Color,
    /** Confirmed states: Verified, Tor connected. */
    val ok: Color,
    val okSoft: Color,
    val okBorder: Color,
    val warn: Color,
    val warnSoft: Color,
    /** Text: missed calls, Block, Report. */
    val danger: Color,
    /** Fills: end / decline call. */
    val dangerStrong: Color,
    val dangerSoft: Color,
    /** Borders of error and warning callouts. */
    val dangerBorder: Color,
    val warnBorder: Color,
    val toggleOnBg: Color,
    val toggleOnFg: Color,
    /** Extra locks on files. */
    val lockBg: Color,
    val lockText: Color,
    val lockText2: Color,
    val lockBorder: Color,
    val avatarInk: Color,
    val avatarTints: List<Color>,
)

private val Tokens = RedvaultColors(
    bg = Color(0xFF101114),
    bar = Color(0xFF17191E),
    surface = Color(0xFF17191E),
    surface2 = Color(0xFF20232A),
    surface3 = Color(0xFF282B33),
    selected = Color(0xFF22252C),
    text = Color(0xFFEDEEF0),
    text2 = Color(0xFFC3C6CD),
    text3 = Color(0xFF9599A3),
    placeholder = Color(0xFF858A94),
    border = Color(0xFF25282F),
    borderStrong = Color(0xFF323640),
    accent = Color(0xFFE7677F),
    accentSoft = Color(0xFF2A1820),
    accentBorder = Color(0xFF5C2635),
    accentContrast = Color(0xFFFFFFFF),
    accentFill = Color(0xFFA72640),
    focusRing = Color(0xFFF08AA0),
    bubbleIn = Color(0xFF20232A),
    bubbleOut = Color(0xFF8E2238),
    bubbleOutText = Color(0xFFFFFFFF),
    bubbleOutTime = Color(0xFFF0C3CD),
    ok = Color(0xFF6FCF97),
    okSoft = Color(0xFF15241C),
    okBorder = Color(0xFF24432F),
    warn = Color(0xFFEBBE7A),
    warnSoft = Color(0xFF29231A),
    danger = Color(0xFFF68C72),
    dangerStrong = Color(0xFFC9402B),
    dangerSoft = Color(0xFF2B1814),
    dangerBorder = Color(0xFF5E2C22),
    warnBorder = Color(0xFF4B3F27),
    toggleOnBg = Color(0xFFEDEEF0),
    toggleOnFg = Color(0xFF101114),
    lockBg = Color(0xFF1C2133),
    lockText = Color(0xFFC6D0F5),
    lockText2 = Color(0xFF9FB0F0),
    lockBorder = Color(0xFF2B3352),
    avatarInk = Color(0xFF141413),
    avatarTints = listOf(
        Color(0xFFC9B6E4), Color(0xFF9CC9E8), Color(0xFFF2C38B),
        Color(0xFFB9D98C), Color(0xFFE8A7A0), Color(0xFFE6D48A),
    ),
)

private val LightTokens = RedvaultColors(
    bg = Color(0xFFF6F6F8),
    bar = Color(0xFFFFFFFF),
    surface = Color(0xFFFFFFFF),
    surface2 = Color(0xFFEFEFF2),
    surface3 = Color(0xFFE4E4E9),
    selected = Color(0xFFECE7EA),
    text = Color(0xFF15161A),
    text2 = Color(0xFF3E4049),
    text3 = Color(0xFF646772),
    placeholder = Color(0xFF7A7D87),
    border = Color(0xFFE2E2E7),
    borderStrong = Color(0xFFCBCBD3),
    accent = Color(0xFFA72640),
    accentSoft = Color(0xFFF8E7EB),
    accentBorder = Color(0xFFEBC1CB),
    accentContrast = Color(0xFFFFFFFF),
    accentFill = Color(0xFFA72640),
    focusRing = Color(0xFFA72640),
    bubbleIn = Color(0xFFFFFFFF),
    bubbleOut = Color(0xFFA72640),
    bubbleOutText = Color(0xFFFFFFFF),
    bubbleOutTime = Color(0xFFF7D9E0),
    ok = Color(0xFF1E7A4C),
    okSoft = Color(0xFFE3F2E9),
    okBorder = Color(0xFFB7DCC6),
    warn = Color(0xFF94590A),
    warnSoft = Color(0xFFFCF1DF),
    danger = Color(0xFFB3361F),
    dangerStrong = Color(0xFFC9402B),
    dangerSoft = Color(0xFFFCEAE5),
    dangerBorder = Color(0xFFF1C2B6),
    warnBorder = Color(0xFFEED3A8),
    toggleOnBg = Color(0xFF15161A),
    toggleOnFg = Color(0xFFFFFFFF),
    lockBg = Color(0xFFE8EDFF),
    lockText = Color(0xFF2F44A8),
    lockText2 = Color(0xFF4B5FC4),
    lockBorder = Color(0xFFC4CFF5),
    avatarInk = Color(0xFF141413),
    avatarTints = Tokens.avatarTints,
)

/**
 * Inter for text and titles (bundled, SIL OFL; licenses/inter-OFL.txt), IBM Plex Mono for anything exact
 * (keys, codes, safety numbers). Sizes are in sp, so they follow the system font size.
 */
object RedvaultFonts {
    val sans = FontFamily(
        Font(R.font.inter_regular, FontWeight.Normal),
        Font(R.font.inter_medium, FontWeight.Medium),
        Font(R.font.inter_semibold, FontWeight.SemiBold),
        Font(R.font.inter_bold, FontWeight.Bold),
    )
    val display = FontFamily(Font(R.font.inter_bold, FontWeight.Bold), Font(R.font.inter_semibold, FontWeight.SemiBold))
    val mono = FontFamily(
        Font(R.font.ibm_plex_mono_regular, FontWeight.Normal),
        Font(R.font.ibm_plex_mono_medium, FontWeight.Medium),
    )

    /** Tab titles ("Chats", "Files"): 26sp, tight. */
    val title = TextStyle(fontFamily = display, fontWeight = FontWeight.Bold, fontSize = 26.sp, letterSpacing = (-0.02).em)
}

private fun schemeFor(it: RedvaultColors, dark: Boolean) =
    (if (dark) darkColorScheme() else lightColorScheme()).copy(
        primary = it.accent, onPrimary = it.accentContrast,
        primaryContainer = it.accentSoft, onPrimaryContainer = it.accent,
        secondaryContainer = it.surface2, onSecondaryContainer = it.text,
        background = it.bg, onBackground = it.text,
        surface = it.bg, onSurface = it.text,
        surfaceVariant = it.surface2, onSurfaceVariant = it.text2,
        surfaceContainer = it.surface, surfaceContainerLow = it.surface, surfaceContainerHigh = it.surface2,
        surfaceContainerHighest = it.surface2,
        outline = it.borderStrong, outlineVariant = it.border,
        error = it.danger, onError = it.accentContrast,
        errorContainer = it.dangerSoft, onErrorContainer = it.danger,
    )

private val DarkScheme = schemeFor(Tokens, dark = true)
private val LightScheme = schemeFor(LightTokens, dark = false)

private val Type = Typography().let { base ->
    fun TextStyle.sans() = copy(fontFamily = RedvaultFonts.sans)
    Typography(
        displayLarge = base.displayLarge.copy(fontFamily = RedvaultFonts.display),
        displayMedium = base.displayMedium.copy(fontFamily = RedvaultFonts.display),
        displaySmall = base.displaySmall.copy(fontFamily = RedvaultFonts.display),
        headlineLarge = base.headlineLarge.copy(fontFamily = RedvaultFonts.display),
        headlineMedium = base.headlineMedium.copy(fontFamily = RedvaultFonts.display),
        headlineSmall = base.headlineSmall.copy(fontFamily = RedvaultFonts.display),
        titleLarge = base.titleLarge.sans(),
        titleMedium = base.titleMedium.sans(),
        titleSmall = base.titleSmall.sans(),
        bodyLarge = base.bodyLarge.copy(fontFamily = RedvaultFonts.sans, fontSize = 15.sp),
        bodyMedium = base.bodyMedium.sans(),
        bodySmall = base.bodySmall.sans(),
        labelLarge = base.labelLarge.sans(),
        labelMedium = base.labelMedium.sans(),
        labelSmall = base.labelSmall.sans(),
    )
}

private val LocalRedvaultColors = staticCompositionLocalOf { Tokens }

// Inputs 10dp, cards 14dp, dialogs and sheets 18dp (the desktop's radii).
private val RedvaultShapes = Shapes(
    extraSmall = RoundedCornerShape(8.dp),
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(18.dp),
    extraLarge = RoundedCornerShape(24.dp),
)

object RedvaultTheme {
    val colors: RedvaultColors
        @Composable get() = LocalRedvaultColors.current

    /** The avatar tint for a name or id: the same one every time, and the same as the web and desktop apps. */
    fun tintFor(key: String, colors: RedvaultColors = Tokens): Color {
        var h = 0L
        for (c in key) h = (h * 31 + c.code) and 0xFFFFFFFFL
        return colors.avatarTints[(h % colors.avatarTints.size).toInt()]
    }
}

/** The calls screens stay dark in every theme, like the desktop: video and faces read best on black. */
@Composable
fun DarkOnly(content: @Composable () -> Unit) {
    CompositionLocalProvider(LocalRedvaultColors provides Tokens) {
        MaterialTheme(colorScheme = DarkScheme, shapes = RedvaultShapes, typography = Type, content = content)
    }
}

@Composable
fun RedvaultTheme(dark: Boolean = true, content: @Composable () -> Unit) {
    CompositionLocalProvider(LocalRedvaultColors provides if (dark) Tokens else LightTokens) {
        MaterialTheme(colorScheme = if (dark) DarkScheme else LightScheme, shapes = RedvaultShapes, typography = Type, content = content)
    }
}
