// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.graphics.SurfaceTexture
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.view.Surface
import android.view.TextureView
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.BoxWithConstraintsScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryMeta
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import kotlinx.coroutines.delay
import java.io.File
import kotlin.math.roundToInt

/** The story tokens of the web app (tokens.css --story-*), so a story looks the same on every app. */
internal object StoryStyle {
    fun color(c: StoryColor): Color = when (c) {
        StoryColor.White -> Color(0xFFFFFFFF)
        StoryColor.Black -> Color(0xFF111111)
        StoryColor.Red -> Color(0xFFEF4444)
        StoryColor.Orange -> Color(0xFFF97316)
        StoryColor.Yellow -> Color(0xFFFACC15)
        StoryColor.Green -> Color(0xFF22C55E)
        StoryColor.Teal -> Color(0xFF14B8A6)
        StoryColor.Blue -> Color(0xFF3B82F6)
        StoryColor.Purple -> Color(0xFFA855F7)
        StoryColor.Pink -> Color(0xFFEC4899)
    }

    /** The ink on a boxed overlay: dark on the light colors, light on the rest. */
    fun boxedInk(c: StoryColor): Color = when (c) {
        StoryColor.White, StoryColor.Yellow, StoryColor.Orange, StoryColor.Green, StoryColor.Teal -> color(StoryColor.Black)
        else -> color(StoryColor.White)
    }

    fun gradient(b: StoryBackground): List<Color> = when (b) {
        StoryBackground.Sunset -> listOf(Color(0xFFF97316), Color(0xFFEC4899))
        StoryBackground.Ocean -> listOf(Color(0xFF0EA5E9), Color(0xFF312E81))
        StoryBackground.Forest -> listOf(Color(0xFF22C55E), Color(0xFF134E4A))
        StoryBackground.Berry -> listOf(Color(0xFFA855F7), Color(0xFFBE185D))
        StoryBackground.Night -> listOf(Color(0xFF1E293B), Color(0xFF020617))
        StoryBackground.Ember -> listOf(Color(0xFFFACC15), Color(0xFFDC2626))
        StoryBackground.Mint -> listOf(Color(0xFF99F6E4), Color(0xFF0D9488))
        StoryBackground.Slate -> listOf(Color(0xFF64748B), Color(0xFF1F2937))
    }

    fun brush(b: StoryBackground): Brush = Brush.verticalGradient(gradient(b))

    fun label(f: StoryFont) = when (f) {
        StoryFont.Classic -> "Classic"
        StoryFont.Modern -> "Modern"
        StoryFont.Typewriter -> "Typewriter"
        StoryFont.Strong -> "Strong"
    }

    fun label(b: StoryBackground) = b.wire.replaceFirstChar { it.uppercase() }

    fun family(f: StoryFont): FontFamily = when (f) {
        StoryFont.Classic -> RedvaultFonts.sans
        StoryFont.Modern, StoryFont.Strong -> RedvaultFonts.display
        StoryFont.Typewriter -> RedvaultFonts.mono
    }

    fun weight(f: StoryFont): FontWeight = when (f) {
        StoryFont.Classic -> FontWeight.SemiBold
        StoryFont.Modern, StoryFont.Strong -> FontWeight.Bold
        StoryFont.Typewriter -> FontWeight.Medium
    }

    /** The "Strong" look is upper case, as on the web. */
    fun shown(f: StoryFont, text: String) = if (f == StoryFont.Strong) text.uppercase() else text

    val ringGradient = listOf(Color(0xFFF2C38B), Color(0xFFF29B90), Color(0xFFD66BB7), Color(0xFF8F7CF2), Color(0xFFF2C38B))
    val ringClose = Color(0xFF4CD07D)
    val ringSeen = Color(0xFF3A433E)
    val scrim = Color(0x80000000)
    val track = Color(0x59FFFFFF)
    val chrome = Color(0xEBFFFFFF)
}

/**
 * One story drawn into its frame: a text story's words on its background, or a photo or video, with the text and emoji
 * that were placed on it and, over a video, the subtitle for now. Sizes are fractions of the frame's width (the web's
 * container units), so it looks the same at any size. Overlay animations (`anim`) aren't played: they show still, as in
 * apps from before they existed.
 */
@Composable
internal fun StoryFrame(
    meta: StoryMeta,
    photo: ImageBitmap?,
    video: File?,
    paused: Boolean,
    caption: String?,
    modifier: Modifier = Modifier,
    muted: Boolean = false,
    onVideoPosition: (positionMs: Long, durationMs: Long) -> Unit = { _, _ -> },
    onVideoEnded: () -> Unit = {},
    /** The stickers' surroundings; without it a story's stickers aren't drawn (the composer's preview has none). */
    stickers: StickerEnv? = null,
) {
    BoxWithConstraints(modifier.background(Color.Black).clipToBounds()) {
        val width = maxWidth
        val density = LocalDensity.current
        fun cq(percent: Float) = with(density) { (width * percent / 100f).toSp() }
        when (meta.kind) {
            StoryKind.Text -> {
                val font = meta.font ?: StoryFont.Classic
                Box(Modifier.fillMaxSize().background(StoryStyle.brush(meta.background ?: StoryBackground.Sunset)))
                Text(
                    StoryStyle.shown(font, meta.text.orEmpty()),
                    color = Color.White,
                    fontFamily = StoryStyle.family(font),
                    fontWeight = StoryStyle.weight(font),
                    fontSize = cq(7.5f),
                    lineHeight = cq(9.4f),
                    textAlign = TextAlign.Center,
                    modifier = Modifier.align(Alignment.Center).fillMaxWidth().padding(horizontal = width * 0.08f),
                )
            }
            StoryKind.Image -> if (photo != null) {
                Image(photo, contentDescription = "Story photo", contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize())
            }
            StoryKind.Video -> if (video != null) VideoSurface(video, meta.media?.width ?: 9, meta.media?.height ?: 16, paused, muted, onVideoPosition, onVideoEnded)
        }
        for (overlay in meta.overlays) OverlayView(overlay, width) { cq(it) }
        if (stickers != null) for (sticker in meta.stickers) StickerPlaced(sticker) { StickerView(sticker, width, stickers) }
        if (caption != null) {
            Text(
                caption,
                color = Color.White,
                fontSize = cq(4.4f),
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = 84.dp, start = 24.dp, end = 24.dp)
                    .background(StoryStyle.scrim, RoundedCornerShape(8.dp))
                    .padding(horizontal = 10.dp, vertical = 4.dp),
            )
        }
    }
}

@Composable
private fun BoxWithConstraintsScope.OverlayView(o: StoryOverlay, frameWidth: androidx.compose.ui.unit.Dp, cq: (Float) -> androidx.compose.ui.unit.TextUnit) {
    // The overlay is centred on (x, y) of the frame: measure it, then shift it by half its size.
    var size by remember { mutableStateOf(androidx.compose.ui.unit.IntSize.Zero) }
    val px = with(LocalDensity.current) { frameWidth.toPx() }
    val py = with(LocalDensity.current) { maxHeight.toPx() }
    OverlayText(
        o,
        frameWidth,
        cq,
        Modifier
            .align(Alignment.TopStart)
            .offset { IntOffset((o.x * px - size.width / 2f).roundToInt(), (o.y * py - size.height / 2f).roundToInt()) }
            .onSizeChanged { size = it }
            .graphicsLayer { rotationZ = o.rotation.toFloat(); scaleX = o.scale.toFloat(); scaleY = o.scale.toFloat() },
    )
}

/** The words or emoji of an overlay, drawn the way every app draws them (placement is up to the caller). */
@Composable
internal fun OverlayText(o: StoryOverlay, frameWidth: androidx.compose.ui.unit.Dp, cq: (Float) -> androidx.compose.ui.unit.TextUnit, modifier: Modifier = Modifier) {
    val boxed = !o.isEmoji && o.boxed
    val text = if (o.isEmoji) o.text else StoryStyle.shown(o.font, o.text)
    Text(
        text,
        color = when {
            o.isEmoji -> Color.White
            boxed -> StoryStyle.boxedInk(o.color)
            else -> StoryStyle.color(o.color)
        },
        fontFamily = if (o.isEmoji) null else StoryStyle.family(o.font),
        fontWeight = if (o.isEmoji) null else StoryStyle.weight(o.font),
        fontSize = if (o.isEmoji) cq(15f) else cq(6f),
        style = TextStyle(lineHeight = if (o.isEmoji) cq(15f) else cq(7.2f), textAlign = TextAlign.Center),
        modifier = modifier
            .widthIn(max = frameWidth * 0.86f)
            .then(if (boxed) Modifier.background(StoryStyle.color(o.color), RoundedCornerShape(6.dp)) else Modifier)
            .padding(horizontal = if (o.isEmoji) 0.dp else 8.dp, vertical = if (o.isEmoji) 0.dp else 3.dp),
    )
}

/**
 * The decrypted video, played from this app's private cache by the system's MediaPlayer onto a TextureView (a SurfaceView
 * never got its surface on some devices, and a TextureView draws like any other view). Fitted inside the frame by the
 * video's own proportions; paused while a finger is down.
 */
@Composable
private fun VideoSurface(file: File, videoWidth: Int, videoHeight: Int, paused: Boolean, muted: Boolean, onPosition: (Long, Long) -> Unit, onEnded: () -> Unit) {
    val player = remember(file) { MediaPlayer() }
    var ready by remember(file) { mutableStateOf(false) }
    val latestEnded by rememberUpdatedState(onEnded)
    DisposableEffect(player) {
        player.setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
        player.setOnPreparedListener { it.isLooping = false; ready = true; it.start() }
        player.setOnCompletionListener { latestEnded() }
        // A video this phone can't play is skipped, not stared at.
        player.setOnErrorListener { _, _, _ -> latestEnded(); true }
        onDispose { runCatching { player.release() } }
    }
    Box(Modifier.fillMaxSize().background(Color.Black), contentAlignment = Alignment.Center) {
        AndroidView(
            modifier = Modifier.aspectRatio(videoWidth.coerceAtLeast(1).toFloat() / videoHeight.coerceAtLeast(1)),
            factory = { context ->
                TextureView(context).apply {
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                            try {
                                player.setSurface(Surface(texture))
                                player.setDataSource(file.absolutePath)
                                player.prepareAsync()
                            } catch (e: Exception) {
                                latestEnded()
                            }
                        }
                        override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {}
                        override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean = true
                        override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
                    }
                }
            },
        )
    }
    LaunchedEffect(player, ready, muted) {
        if (ready) runCatching { player.setVolume(if (muted) 0f else 1f, if (muted) 0f else 1f) }
    }
    LaunchedEffect(player, ready, paused) {
        if (!ready) return@LaunchedEffect
        if (paused) {
            runCatching { player.pause() }
        } else {
            runCatching { player.start() }
            while (true) {
                runCatching { onPosition(player.currentPosition.toLong(), player.duration.toLong()) }
                delay(100)
            }
        }
    }
}
