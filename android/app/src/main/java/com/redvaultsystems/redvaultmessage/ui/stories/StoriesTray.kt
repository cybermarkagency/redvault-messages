package com.redvaultsystems.redvaultmessage.ui.stories

import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.background
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.AuthorStories
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/**
 * The row of rings above the chat list: yours first, then friends with something to see (new first). Shows nothing at all
 * unless the server has Stories on (see [StoriesGate]).
 */
@Composable
fun StoriesTray(
    state: StoriesState,
    selfHandle: String,
    selfAvatarUrl: String?,
    onOpenOwn: () -> Unit,
    onCompose: () -> Unit,
    onOpenAuthor: (userId: String) -> Unit,
) {
    if (!state.on) return
    val colors = RedvaultTheme.colors
    LazyRow(contentPadding = PaddingValues(horizontal = 20.dp, vertical = 10.dp), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        item(key = "me") {
            val hasOwn = state.own.isNotEmpty()
            TrayItem(
                label = "Your story",
                avatarUrl = selfAvatarUrl,
                seed = selfHandle,
                ring = if (hasOwn) Brush.sweepGradient(StoryStyle.ringGradient) else null,
                plus = true,
                description = if (hasOwn) "Your story. Opens it" else "Add to your story",
                onClick = if (hasOwn) onOpenOwn else onCompose,
                onPlus = onCompose,
            )
        }
        items(state.authors, key = { it.author.userId }) { author ->
            TrayItem(
                label = author.author.handle,
                avatarUrl = author.author.avatarUrl,
                seed = author.author.handle,
                ring = ringOf(author),
                plus = false,
                onPlus = {},
                description = "${author.author.handle}'s story" + if (author.allSeen) "" else ", new",
                onClick = { onOpenAuthor(author.author.userId) },
            )
        }
    }
}

private fun ringOf(a: AuthorStories): Brush = when {
    a.allSeen -> SolidColor(StoryStyle.ringSeen)
    a.stories.any { !it.seen && it.meta.closeFriends } -> SolidColor(StoryStyle.ringClose)
    else -> Brush.sweepGradient(StoryStyle.ringGradient)
}

@Composable
private fun TrayItem(label: String, avatarUrl: String?, seed: String, ring: Brush?, plus: Boolean, onPlus: () -> Unit, description: String, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.clip(RoundedCornerShape(14.dp)).clickable(role = Role.Button, onClick = onClick).semantics { contentDescription = description }.padding(2.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        Box(Modifier.size(68.dp), contentAlignment = Alignment.Center) {
            Box(Modifier.size(68.dp).then(if (ring != null) Modifier.border(2.5.dp, ring, CircleShape) else Modifier.border(1.dp, colors.border, CircleShape)))
            Avatar(avatarUrl = avatarUrl, seed = seed, size = 58.dp)
            if (plus) {
                Box(
                    Modifier
                        .align(Alignment.BottomEnd)
                        .size(24.dp)
                        .background(colors.accent, CircleShape)
                        .border(2.dp, colors.bg, CircleShape)
                        .clip(CircleShape)
                        .clickable(role = Role.Button, onClick = onPlus)
                        .semantics { contentDescription = "Add to your story" },
                    contentAlignment = Alignment.Center,
                ) { DsIcon(R.drawable.ui_plus, colors.accentContrast, 12.dp) }
            }
        }
        Text(label, color = colors.text2, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center, modifier = Modifier.width(72.dp))
    }
}
