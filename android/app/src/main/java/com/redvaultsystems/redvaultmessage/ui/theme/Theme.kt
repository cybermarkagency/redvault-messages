package com.redvaultsystems.redvaultmessage.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
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

// Design tokens from the mobile UI handoff (the same values as the desktop
// app and the web redesign). The design is dark only.

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
    val bubbleOut: Color,
    val bubbleOutTime: Color,
    val warn: Color,
    val warnSoft: Color,
    /** Text: missed calls, Block, Report. */
    val danger: Color,
    /** Fills: end / decline call. */
    val dangerStrong: Color,
    val dangerSoft: Color,
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
    bg = Color(0xFF0F1311),
    bar = Color(0xFF131816),
    surface = Color(0xFF171C1A),
    surface2 = Color(0xFF1B211E),
    surface3 = Color(0xFF1F2623),
    selected = Color(0xFF1D2421),
    text = Color(0xFFECEAE3),
    text2 = Color(0xFFC9CFCA),
    text3 = Color(0xFF9AA39D),
    placeholder = Color(0xFF8A938D),
    border = Color(0xFF232A27),
    borderStrong = Color(0xFF26302B),
    accent = Color(0xFF86D9AE),
    accentSoft = Color(0xFF16271F),
    accentBorder = Color(0xFF244536),
    accentContrast = Color(0xFF0B2A1C),
    bubbleOut = Color(0xFF1E4434),
    bubbleOutTime = Color(0xFFB5D9C5),
    warn = Color(0xFFF2C38B),
    warnSoft = Color(0xFF2A2217),
    danger = Color(0xFFF29B90),
    dangerStrong = Color(0xFFC8433B),
    dangerSoft = Color(0xFF2A1714),
    toggleOnBg = Color(0xFFECEAE3),
    toggleOnFg = Color(0xFF0F1311),
    lockBg = Color(0xFF1B2236),
    lockText = Color(0xFFC6D4FF),
    lockText2 = Color(0xFF9FB4FF),
    lockBorder = Color(0xFF2A3556),
    avatarInk = Color(0xFF141413),
    avatarTints = listOf(
        Color(0xFFC9B6E4), Color(0xFF9CC9E8), Color(0xFFF2C38B),
        Color(0xFFB9D98C), Color(0xFFE8A7A0), Color(0xFFE6D48A),
    ),
)

/** Bricolage Grotesque for titles, IBM Plex Sans for text, IBM Plex Mono for anything exact. */
object RedvaultFonts {
    val display = FontFamily(Font(R.font.bricolage_grotesque_bold, FontWeight.Bold))
    val sans = FontFamily(
        Font(R.font.ibm_plex_sans_regular, FontWeight.Normal),
        Font(R.font.ibm_plex_sans_medium, FontWeight.Medium),
        Font(R.font.ibm_plex_sans_semibold, FontWeight.SemiBold),
        Font(R.font.ibm_plex_sans_semibold, FontWeight.Bold),
    )
    val mono = FontFamily(
        Font(R.font.ibm_plex_mono_regular, FontWeight.Normal),
        Font(R.font.ibm_plex_mono_medium, FontWeight.Medium),
    )

    /** Tab titles ("Chats", "Files"): 32px, tight. */
    val title = TextStyle(fontFamily = display, fontWeight = FontWeight.Bold, fontSize = 32.sp, letterSpacing = (-0.02).em)
}

private val Colors = Tokens.let {
    darkColorScheme(
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
}

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

// Inputs 12px, cards 16px, dialogs and sheets 22px.
private val RedvaultShapes = Shapes(
    extraSmall = RoundedCornerShape(10.dp),
    small = RoundedCornerShape(12.dp),
    medium = RoundedCornerShape(16.dp),
    large = RoundedCornerShape(22.dp),
    extraLarge = RoundedCornerShape(26.dp),
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

@Composable
@Suppress("UNUSED_PARAMETER")
fun RedvaultTheme(dark: Boolean = true, content: @Composable () -> Unit) {
    CompositionLocalProvider(LocalRedvaultColors provides Tokens) {
        MaterialTheme(colorScheme = Colors, shapes = RedvaultShapes, typography = Type, content = content)
    }
}
