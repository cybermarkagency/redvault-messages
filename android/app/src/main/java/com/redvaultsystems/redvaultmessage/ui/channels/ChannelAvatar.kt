// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.channels

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.ui.components.InitialsAvatar

/**
 * A channel's photo, or its initials. Fetched without the session cookie
 * (and with the private link's token when the channel is private), so
 * loading a photo never tells the server which account looked.
 */
@Composable
fun ChannelAvatar(channels: ChannelSession, name: String, avatarUrl: String?, access: String?, size: Dp = 40.dp, modifier: Modifier = Modifier) {
    val photo by produceState<ImageBitmap?>(initialValue = null, avatarUrl, access) {
        value = avatarUrl?.let { channels.avatar(it, access)?.asImageBitmap() }
    }
    val loaded = photo
    if (loaded == null) {
        InitialsAvatar(name, size, modifier = modifier)
    } else {
        Image(
            bitmap = loaded,
            contentDescription = "Photo of $name",
            contentScale = ContentScale.Crop,
            modifier = modifier.size(size).clip(CircleShape),
        )
    }
}
