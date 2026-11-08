// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.components

import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request

/**
 * Loads profile photos through the app's own authenticated HTTP client (the
 * server proxies them, so the storage provider never sees a viewer's IP).
 * Only same-server "/api/..." paths are fetched, never an arbitrary URL.
 * Photos are cached in memory only.
 */
class AvatarLoader(private val client: OkHttpClient, private val apiBase: String) {
    private val cache = LruCache<String, ImageBitmap>(64)

    suspend fun load(avatarUrl: String): ImageBitmap? {
        cache.get(avatarUrl)?.let { return it }
        if (!avatarUrl.startsWith("/api/")) return null
        return withContext(Dispatchers.IO) {
            runCatching {
                client.newCall(Request.Builder().url(apiBase + avatarUrl.removePrefix("/api/")).build()).execute().use { response ->
                    if (!response.isSuccessful) return@use null
                    val bytes = response.body?.bytes() ?: return@use null
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
                }
            }.getOrNull()?.also { cache.put(avatarUrl, it) }
        }
    }

    fun clear() = cache.evictAll()
}

val LocalAvatarLoader = staticCompositionLocalOf<AvatarLoader?> { null }

/** "maya.okafor" -> "MO", "quiet-harbor·3f2a" -> "QH", "Design Team" -> "DT" (the web's initials()). */
fun initialsOf(name: String): String {
    val words = name.replace(Regex("·.*$"), "")
        .split(Regex("[\\s._-]+"))
        .filter { w -> w.any { it.isLetterOrDigit() } }
    val letters = if (words.size >= 2) "${words[0][0]}${words[1][0]}" else (words.firstOrNull() ?: "?").take(2)
    return letters.uppercase()
}

/**
 * The profile photo if there is one, otherwise initials on a tint picked
 * from the name (the design's avatars). `online` adds the green dot.
 */
@Composable
fun Avatar(avatarUrl: String?, seed: String, size: Dp = 40.dp, modifier: Modifier = Modifier, online: Boolean = false) {
    val loader = LocalAvatarLoader.current
    val image by produceState<ImageBitmap?>(initialValue = null, avatarUrl, loader) {
        value = if (avatarUrl != null && loader != null) loader.load(avatarUrl) else null
    }
    Box(modifier.size(size)) {
        val loaded = image
        if (loaded == null) {
            InitialsAvatar(seed, size)
        } else {
            Image(
                bitmap = loaded,
                contentDescription = "Profile photo of $seed",
                contentScale = ContentScale.Crop,
                modifier = Modifier.size(size).clip(CircleShape),
            )
        }
        if (online) {
            // 14dp on list avatars, larger on big ones; kept on the circle's edge (at 45°) rather than the box's corner.
            val dot = maxOf(14.dp, size * 0.18f)
            val inset = maxOf(0.dp, size * 0.146f - dot / 2)
            OnlineDot(Modifier.align(Alignment.BottomEnd).offset(x = -inset, y = -inset), dot)
        }
    }
}

/** Initials on a tint: a circle for people, a rounded square for groups. */
@Composable
fun InitialsAvatar(name: String, size: Dp, square: Boolean = false, modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    val shape = if (square) RoundedCornerShape(size * 0.32f) else CircleShape
    Box(
        modifier
            .size(size)
            .clip(shape)
            .background(RedvaultTheme.tintFor(name, colors))
            .semantics { contentDescription = name },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            initialsOf(name),
            color = colors.avatarInk,
            fontFamily = if (size >= 80.dp) RedvaultFonts.display else RedvaultFonts.sans,
            fontWeight = if (size >= 80.dp) FontWeight.Bold else FontWeight.SemiBold,
            fontSize = (size.value * 0.34f).sp,
        )
    }
}

/** The design's online dot: 14px accent with a 3px background ring. */
@Composable
fun OnlineDot(modifier: Modifier = Modifier, size: Dp = 14.dp) {
    val colors = RedvaultTheme.colors
    Box(modifier.size(size).background(colors.bg, CircleShape).padding(size * 3 / 14).background(colors.accent, CircleShape).semantics { contentDescription = "Online" })
}
