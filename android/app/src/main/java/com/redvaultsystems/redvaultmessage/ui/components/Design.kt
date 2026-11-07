package com.redvaultsystems.redvaultmessage.ui.components

import androidx.annotation.DrawableRes
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

// The mobile design's building blocks: icons, the tab title, the search box,
// filter chips, pill and round buttons, switches and the bottom tab bar.

@Composable
fun DsIcon(@DrawableRes icon: Int, tint: Color, size: Dp = 20.dp, description: String? = null, modifier: Modifier = Modifier) {
    Icon(painterResource(icon), contentDescription = description, tint = tint, modifier = modifier.size(size))
}

/** A tab's big title ("Chats") with its trailing buttons. */
@Composable
fun TabHeader(title: String, modifier: Modifier = Modifier, actions: @Composable RowScope.() -> Unit = {}) {
    Row(
        modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 14.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(title, style = RedvaultFonts.title, color = RedvaultTheme.colors.text, modifier = Modifier.weight(1f))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically, content = actions)
    }
}

/** The mono accent line under a tab title ("End-to-end encrypted · ..."). */
@Composable
fun SecurityLine(text: String, @DrawableRes icon: Int, modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    Row(modifier.padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        DsIcon(icon, colors.accent, size = 13.dp)
        Text(text, color = colors.accent, fontFamily = RedvaultFonts.mono, fontSize = 11.sp, maxLines = 1, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
    }
}

/** A 44px round raised button with an icon. */
@Composable
fun RoundButton(
    @DrawableRes icon: Int,
    description: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    size: Dp = 44.dp,
    iconSize: Dp = 20.dp,
    container: Color = RedvaultTheme.colors.surface2,
    tint: Color = RedvaultTheme.colors.text,
    enabled: Boolean = true,
) {
    Box(
        modifier
            .size(size)
            .clip(CircleShape)
            .background(container)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .alpha(if (enabled) 1f else 0.45f)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center,
    ) { DsIcon(icon, tint, iconSize) }
}

/** The search field: a raised rounded box with a magnifier. */
@Composable
fun SearchBox(value: String, onValueChange: (String) -> Unit, placeholder: String, @DrawableRes icon: Int, modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 46.dp)
            .background(colors.surface2, RoundedCornerShape(14.dp))
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        DsIcon(icon, colors.text3, 18.dp)
        Box(Modifier.weight(1f)) {
            if (value.isEmpty()) Text(placeholder, color = colors.placeholder, fontSize = 15.sp, fontFamily = RedvaultFonts.sans)
            BasicTextField(
                value = value,
                onValueChange = onValueChange,
                singleLine = true,
                textStyle = TextStyle(color = colors.text, fontSize = 15.sp, fontFamily = RedvaultFonts.sans),
                cursorBrush = SolidColor(colors.accent),
                modifier = Modifier.fillMaxWidth().semantics { contentDescription = placeholder },
            )
        }
    }
}

/** One filter chip: selected is light with dark text. */
@Composable
fun FilterChip(text: String, selected: Boolean, onClick: () -> Unit, @DrawableRes icon: Int? = null, lock: Boolean = false) {
    val colors = RedvaultTheme.colors
    val container = when {
        selected -> colors.toggleOnBg
        lock -> colors.lockBg
        else -> colors.surface2
    }
    val ink = when {
        selected -> colors.toggleOnFg
        lock -> colors.lockText
        else -> colors.text2
    }
    Row(
        Modifier
            .height(36.dp)
            .clip(RoundedCornerShape(18.dp))
            .background(container)
            .clickable(role = Role.Tab, onClick = onClick)
            .semantics { this.selected = selected }
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        if (icon != null) DsIcon(icon, ink, 13.dp)
        Text(text, color = ink, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, fontFamily = RedvaultFonts.sans)
    }
}

@Composable
fun <T> ChipRow(
    options: List<T>,
    selected: T,
    label: (T) -> String,
    onSelect: (T) -> Unit,
    lockOption: T? = null,
    @DrawableRes lockIcon: Int? = null,
    /** The screen-edge inset; 0 inside something already padded. */
    edge: Dp = 20.dp,
) {
    LazyRow(contentPadding = PaddingValues(horizontal = edge), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        items(options) { option ->
            FilterChip(
                label(option),
                selected = option == selected,
                onClick = { onSelect(option) },
                icon = if (option == lockOption) lockIcon else null,
                lock = option == lockOption,
            )
        }
    }
}

enum class PillStyle { Primary, Secondary, Danger, Quiet }

/** The design's pill button (52px for a sheet's primary action, 44px elsewhere). */
@Composable
fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    style: PillStyle = PillStyle.Primary,
    enabled: Boolean = true,
    busy: Boolean = false,
    height: Dp = 52.dp,
    @DrawableRes icon: Int? = null,
) {
    val colors = RedvaultTheme.colors
    val container = when (style) {
        PillStyle.Primary -> colors.accent
        PillStyle.Secondary -> colors.surface2
        PillStyle.Danger -> colors.dangerSoft
        PillStyle.Quiet -> Color.Transparent
    }
    val ink = when (style) {
        PillStyle.Primary -> colors.accentContrast
        PillStyle.Secondary -> colors.text
        PillStyle.Danger -> colors.danger
        PillStyle.Quiet -> colors.accent
    }
    val active = enabled && !busy
    // Disabled: a quiet surface with muted text, so it never reads as tappable. Busy keeps its colours, dimmed.
    val fill = if (!enabled && style != PillStyle.Quiet) colors.surface2 else container
    val textInk = if (!enabled) colors.text3 else ink
    Row(
        modifier
            .height(height)
            .alpha(if (busy) 0.7f else 1f)
            .clip(RoundedCornerShape(height / 2))
            .background(fill)
            .clickable(enabled = active, role = Role.Button, onClick = onClick)
            .semantics { if (!enabled) stateDescription = "Unavailable" }
            .padding(horizontal = 20.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
    ) {
        if (busy) CircularProgressIndicator(color = textInk, strokeWidth = 2.dp, modifier = Modifier.size(16.dp))
        else if (icon != null) DsIcon(icon, textInk, 18.dp)
        Text(text, color = textInk, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, fontFamily = RedvaultFonts.sans)
    }
}

/** The design's switch: 50x30 on mobile, accent when on. */
@Composable
fun DsSwitch(checked: Boolean, onCheckedChange: (Boolean) -> Unit, description: String, modifier: Modifier = Modifier, enabled: Boolean = true) {
    val colors = RedvaultTheme.colors
    Box(
        modifier
            .size(width = 50.dp, height = 30.dp)
            .alpha(if (enabled) 1f else 0.4f)
            .clip(RoundedCornerShape(15.dp))
            .background(if (checked) colors.accent else colors.surface3)
            .clickable(enabled = enabled, role = Role.Switch) { onCheckedChange(!checked) }
            .semantics {
                contentDescription = description
                stateDescription = if (checked) "On" else "Off"
            },
    ) {
        Box(
            Modifier
                .offset(x = if (checked) 23.dp else 3.dp, y = 3.dp)
                .size(24.dp)
                .background(if (checked) colors.accentContrast else colors.text3, CircleShape),
        )
    }
}

/** A labelled setting with a switch, on a card. */
@Composable
fun SwitchCard(title: String, description: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit, modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    Row(
        modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(16.dp)).padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, color = colors.text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
            Text(description, color = colors.text2, fontSize = 13.sp)
        }
        DsSwitch(checked, onCheckedChange, title)
    }
}

/** A section label in small caps ("RECENT"). */
@Composable
fun SectionCaps(text: String, modifier: Modifier = Modifier, edge: Dp = 20.dp) {
    Text(
        text.uppercase(),
        color = RedvaultTheme.colors.text3,
        fontSize = 12.sp,
        fontWeight = FontWeight.SemiBold,
        letterSpacing = 0.8.sp,
        modifier = modifier.padding(horizontal = edge, vertical = 8.dp),
    )
}

enum class CalloutTone { Ok, Error, Warn, Neutral, Lock }

/** A callout with an icon (valid key, errors, notes). */
@Composable
fun Callout(text: String, tone: CalloutTone, @DrawableRes icon: Int, modifier: Modifier = Modifier, detail: String? = null, extra: @Composable () -> Unit = {}) {
    val colors = RedvaultTheme.colors
    val (container, border, ink) = when (tone) {
        CalloutTone.Ok -> Triple(colors.accentSoft, colors.accentBorder, colors.accent)
        CalloutTone.Error -> Triple(colors.dangerSoft, Color(0xFF5A2D28), colors.danger)
        CalloutTone.Warn -> Triple(colors.warnSoft, Color(0xFF4A3F28), colors.warn)
        CalloutTone.Neutral -> Triple(colors.surface, colors.border, colors.text3)
        CalloutTone.Lock -> Triple(colors.lockBg, colors.lockBorder, colors.lockText)
    }
    Row(
        modifier.fillMaxWidth().background(container, RoundedCornerShape(14.dp)).border(1.dp, border, RoundedCornerShape(14.dp)).padding(14.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        DsIcon(icon, ink, 18.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (text.isNotEmpty()) {
                Text(
                    text,
                    color = if (tone == CalloutTone.Neutral) colors.text2 else ink,
                    fontSize = 13.sp,
                    fontWeight = if (tone == CalloutTone.Ok && detail != null) FontWeight.SemiBold else FontWeight.Normal,
                )
            }
            if (detail != null) Text(detail, color = colors.text, fontSize = 12.sp)
            extra()
        }
    }
}

/** One destination in the bottom tab bar. */
data class TabItem(val key: String, val label: String, @DrawableRes val icon: Int, val badge: Int = 0)

/** The bottom tab bar: equal tabs, accent when selected, 1px top border. */
@Composable
fun BottomTabBar(tabs: List<TabItem>, selected: String, onSelect: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxWidth().background(colors.bg)) {
        Box(Modifier.fillMaxWidth().height(1.dp).background(colors.border))
        Row(Modifier.fillMaxWidth().navigationBarsPadding().padding(horizontal = 8.dp, vertical = 6.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
            tabs.forEach { tab ->
                val on = tab.key == selected
                val ink = if (on) colors.accent else colors.text3
                Column(
                    Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(12.dp))
                        .clickable(role = Role.Tab) { onSelect(tab.key) }
                        .semantics { this.selected = on }
                        .padding(vertical = 6.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Box {
                        DsIcon(tab.icon, ink, 24.dp)
                        if (tab.badge > 0) {
                            Box(
                                Modifier.align(Alignment.TopEnd).offset(x = 8.dp, y = (-4).dp).size(16.dp).background(colors.accent, CircleShape),
                                contentAlignment = Alignment.Center,
                            ) {
                                Text(if (tab.badge > 9) "9+" else tab.badge.toString(), color = colors.accentContrast, fontSize = 9.sp, fontWeight = FontWeight.Bold)
                            }
                        }
                    }
                    Spacer(Modifier.height(4.dp))
                    // Six tabs fit a narrow phone at 10sp on one line.
                    Text(
                        tab.label,
                        color = ink,
                        fontSize = if (tabs.size > 5) 10.sp else 11.sp,
                        fontWeight = if (on) FontWeight.SemiBold else FontWeight.Medium,
                        fontFamily = RedvaultFonts.sans,
                        maxLines = 1,
                        softWrap = false,
                    )
                }
            }
        }
    }
}

/** A sheet's top bar: Cancel on the left, the title centred. */
@Composable
fun SheetBar(title: String, onCancel: () -> Unit, cancelText: String = "Cancel") {
    val colors = RedvaultTheme.colors
    Box(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp)) {
        Text(
            cancelText,
            color = colors.accent,
            fontSize = 16.sp,
            fontFamily = RedvaultFonts.sans,
            modifier = Modifier.align(Alignment.CenterStart).clip(RoundedCornerShape(10.dp)).clickable(role = Role.Button, onClick = onCancel).padding(horizontal = 8.dp, vertical = 12.dp),
        )
        Text(title, color = colors.text, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, fontFamily = RedvaultFonts.sans, modifier = Modifier.align(Alignment.Center))
    }
}

/** Width helper for fixed-size spacers in rows. */
@Composable
fun HSpace(width: Dp) = Spacer(Modifier.width(width))
