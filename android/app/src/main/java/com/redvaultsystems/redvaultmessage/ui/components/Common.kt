package com.redvaultsystems.redvaultmessage.ui.components

import androidx.annotation.DrawableRes
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/** A secondary screen in the design's style: a round back button, a display title, then a scrolling column. */
@Composable
fun SubScreen(title: String, onBack: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding().imePadding()) {
        Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 12.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back", onClick = onBack, container = Color.Transparent, iconSize = 24.dp)
        }
        Column(
            modifier = Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(title, fontFamily = RedvaultFonts.display, fontSize = 30.sp, fontWeight = FontWeight.Bold, color = colors.text, letterSpacing = (-0.5).sp)
            content()
            Spacer(Modifier.height(20.dp))
        }
    }
}

/** A section heading inside a screen's padded column. */
@Composable
fun SectionLabel(text: String) {
    SectionCaps(text, Modifier.padding(top = 12.dp), edge = 0.dp)
}

/** A row on a surface card: optional icon, title and description, then a control or a chevron. */
@Composable
fun SettingsRow(
    title: String,
    description: String? = null,
    onClick: (() -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
    @DrawableRes icon: Int? = null,
    danger: Boolean = false,
) {
    val colors = RedvaultTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(colors.surface)
            .then(if (onClick != null) Modifier.clickable(role = Role.Button, onClick = onClick) else Modifier)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (icon != null) {
            Box(Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(colors.surface2), contentAlignment = Alignment.Center) {
                DsIcon(icon, if (danger) colors.danger else colors.text2, 18.dp)
            }
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, color = if (danger) colors.danger else colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
            if (description != null) Text(description, color = colors.text3, fontSize = 13.sp)
        }
        when {
            trailing != null -> trailing()
            onClick != null -> DsIcon(R.drawable.ui_chevron_right, colors.text3, 18.dp)
        }
    }
}

/** The design's switch for settings rows (Material's Switch signature, so it drops in). */
@Composable
fun RvSwitch(checked: Boolean, onCheckedChange: ((Boolean) -> Unit)?, enabled: Boolean = true, description: String = "") {
    DsSwitch(checked, { onCheckedChange?.invoke(it) }, description, enabled = enabled && onCheckedChange != null)
}

enum class NoticeTone { Warn, Success }

@Composable
fun Notice(text: String, tone: NoticeTone = NoticeTone.Warn) {
    if (tone == NoticeTone.Warn) Callout(text, CalloutTone.Warn, R.drawable.ui_alert_circle)
    else Callout(text, CalloutTone.Ok, R.drawable.ui_check)
}

@Composable
fun PasswordInput(value: String, onValueChange: (String) -> Unit, label: String) {
    Column {
        Text(label, color = RedvaultTheme.colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(bottom = 6.dp))
        DsTextField(value, onValueChange, label, password = true)
    }
}

@Composable
fun PrimaryButton(text: String, enabled: Boolean, busy: Boolean = false, onClick: () -> Unit) {
    PillButton(text, onClick, Modifier.fillMaxWidth(), enabled = enabled, busy = busy)
}

/** The message to show for a failed action: the server's own wording where there is one. */
fun describeError(e: Throwable, fallback: String): String = when (e) {
    is UserFacingException -> e.message ?: fallback
    is com.redvaultsystems.redvaultmessage.crypto.UnsafeKdfException -> e.message ?: fallback
    is ApiException -> e.message?.takeIf { it.isNotBlank() } ?: fallback
    else -> fallback
}
