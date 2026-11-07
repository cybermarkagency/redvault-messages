package com.redvaultsystems.redvaultmessage.ui.channels

import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.data.model.RememberedVote
import com.redvaultsystems.redvaultmessage.ui.TimeFormat
import com.redvaultsystems.redvaultmessage.ui.chats.FormattedText
import com.redvaultsystems.redvaultmessage.ui.chats.openFile
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.io.File

/**
 * A post as a chat's incoming bubble (the same colour and shape as in a chat): the channel's small photo
 * and name, never who posted it, what it carries, and its time and views. No lock, security panel or delivery
 * marks, which a channel has no right to show. [onActions] opens what can be done to it, from a long press or
 * the overflow button (needed too because photos, players and poll options take their own taps); null when
 * there's nothing to do.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ChannelPostBubble(
    channels: ChannelSession,
    channel: ChannelDto,
    post: ChannelPostDto,
    access: String?,
    isOwner: Boolean,
    onOpenLink: (String) -> Unit,
    onChange: (ChannelPostDto) -> Unit,
    onActions: (() -> Unit)?,
) {
    val colors = RedvaultTheme.colors
    val shape = RoundedCornerShape(15.dp, 15.dp, 15.dp, 4.dp)
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        ChannelAvatar(channels, channel.name, channel.avatarUrl, access, size = 28.dp, modifier = Modifier.clearAndSetSemantics {})
        Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
            Column(
                Modifier
                    .widthIn(max = 300.dp)
                    .clip(shape)
                    .background(colors.surface)
                    .then(if (onActions != null) Modifier.combinedClickable(onClick = {}, onLongClickLabel = "Post actions", onLongClick = onActions) else Modifier)
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Text(
                    channel.name,
                    color = colors.accent,
                    fontWeight = FontWeight.SemiBold,
                    style = MaterialTheme.typography.labelMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                PostContent(channels, channel.id, post, access, isOwner, onOpenLink, onChange)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        ChannelThread.footer(post, TimeFormat::time),
                        color = colors.text3,
                        fontFamily = FontFamily.Monospace,
                        style = MaterialTheme.typography.labelSmall,
                        modifier = Modifier.weight(1f, fill = false),
                    )
                    // 28dp drawn, so the footer stays a chat's height; Compose still gives it a 48dp touch target.
                    if (onActions != null) {
                        RoundButton(R.drawable.ui_dots, "Post actions", onActions, size = 28.dp, iconSize = 18.dp, container = Color.Transparent, tint = colors.text3)
                    }
                }
            }
        }
    }
}

/**
 * What a channel post carries besides its header and footer: formatted text,
 * photos, videos, files and recordings, a poll or quiz, or a checklist.
 * A port of the web's components/channel/PostContent.tsx.
 */
@Composable
fun PostContent(
    channels: ChannelSession,
    channelId: String,
    post: ChannelPostDto,
    access: String?,
    isOwner: Boolean,
    onOpenLink: (String) -> Unit,
    /** The post changed (a poll closed): replace it. */
    onChange: (ChannelPostDto) -> Unit = {},
) {
    val colors = RedvaultTheme.colors
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (post.suggested) {
            Text(
                "Suggested by a follower",
                color = colors.accent,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.background(colors.accentSoft, RoundedCornerShape(8.dp)).padding(horizontal = 8.dp, vertical = 3.dp),
            )
        }
        if (post.media.isNotEmpty()) MediaGallery(channels, post.media)
        if (post.body.isNotBlank()) {
            FormattedText(post.body, color = colors.text, onOpenLink = onOpenLink, linkColor = colors.accent, style = androidx.compose.ui.text.TextStyle(fontSize = 15.sp))
        }
        // Votes, closing and ticks go back into the post, so they're still there after scrolling away and back.
        post.poll?.let { poll ->
            PollCard(channels, channelId, post.id, poll, access, isOwner, onChanged = { changed -> onChange(post.copy(poll = changed)) })
        }
        post.checklist?.let { list ->
            ChecklistCard(channels, channelId, post.id, list, access, canTick = isOwner || list.collaborative, onChanged = { changed -> onChange(post.copy(checklist = changed)) })
        }
    }
}

/** Photos at full resolution (tap to see the whole one), videos, round videos, podcasts, voice clips and files. */
@Composable
fun MediaGallery(channels: ChannelSession, media: List<ChannelMediaDto>) {
    val photos = media.filter { it.kind == "photo" }
    val others = media.filter { it.kind != "photo" }
    var viewing by remember { mutableStateOf<ChannelMediaDto?>(null) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        when (photos.size) {
            0 -> {}
            1 -> PhotoTile(channels, photos[0], single = true) { viewing = photos[0] }
            else -> photos.chunked(2).forEach { row ->
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    row.forEach { m -> Box(Modifier.weight(1f)) { PhotoTile(channels, m, single = false) { viewing = m } } }
                    if (row.size == 1) Box(Modifier.weight(1f))
                }
            }
        }
        others.forEach { m ->
            when (m.kind) {
                "video" -> VideoPlayer(channels, m)
                "round" -> RoundVideo(channels, m)
                "audio" -> AudioPlayer(channels, m, podcast = true)
                "voice" -> AudioPlayer(channels, m, podcast = false)
                else -> FileRow(channels, m)
            }
        }
    }
    viewing?.let { PhotoViewer(channels, it) { viewing = null } }
}

@Composable
private fun PhotoTile(channels: ChannelSession, media: ChannelMediaDto, single: Boolean, onOpen: () -> Unit) {
    val colors = RedvaultTheme.colors
    val image by produceState<ImageBitmap?>(initialValue = null, media.id) {
        value = channels.photo(media.id, media.url, if (single) 1280 else 640)?.asImageBitmap()
    }
    val ratio = if (single && (media.width ?: 0) > 0 && (media.height ?: 0) > 0) {
        (media.width!!.toFloat() / media.height!!).coerceIn(0.6f, 2.2f)
    } else {
        1f
    }
    Box(
        Modifier
            .fillMaxWidth()
            .aspectRatio(ratio)
            .clip(RoundedCornerShape(12.dp))
            .background(colors.surface2)
            .clickable(role = Role.Image, onClick = onOpen)
            .semantics { contentDescription = media.filename ?: "Photo" },
        contentAlignment = Alignment.Center,
    ) {
        val loaded = image
        if (loaded == null) {
            CircularProgressIndicator(color = colors.text3, strokeWidth = 2.dp, modifier = Modifier.size(20.dp))
        } else {
            Image(loaded, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
        }
    }
}

/** The whole photo, pinch to zoom; Save puts the original (as posted) where you choose. */
@Composable
private fun PhotoViewer(channels: ChannelSession, media: ChannelMediaDto, onClose: () -> Unit) {
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    val scope = rememberCoroutineScope()
    var error by remember { mutableStateOf<String?>(null) }
    val image by produceState<ImageBitmap?>(initialValue = null, media.id) {
        value = channels.photo(media.id, media.url, 2560)?.asImageBitmap()
    }
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(androidx.compose.ui.geometry.Offset.Zero) }
    val transform = rememberTransformableState { zoom, pan, _ ->
        scale = (scale * zoom).coerceIn(1f, 6f)
        offset = if (scale == 1f) androidx.compose.ui.geometry.Offset.Zero else offset + pan
    }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument(media.mime)) { target ->
        if (target == null) return@rememberLauncherForActivityResult
        scope.launch {
            try {
                val file = channels.download(media.id, media.url)
                context.contentResolver.openOutputStream(target)?.use { out -> file.inputStream().use { it.copyTo(out) } }
            } catch (e: Exception) {
                error = describeError(e, "Couldn't save the photo.")
            }
        }
    }
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            val loaded = image
            if (loaded == null) {
                CircularProgressIndicator(color = Color.White, modifier = Modifier.align(Alignment.Center))
            } else {
                Image(
                    loaded,
                    contentDescription = media.filename ?: "Photo",
                    contentScale = ContentScale.Fit,
                    modifier = Modifier
                        .fillMaxSize()
                        .transformable(transform)
                        .graphicsLayer(scaleX = scale, scaleY = scale, translationX = offset.x, translationY = offset.y),
                )
            }
            Row(Modifier.align(Alignment.TopEnd).padding(12.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                TextButton(onClick = {
                    appLock?.expectExternalReturn()
                    save.launch(media.filename ?: "photo")
                }) { Text("Save", color = Color.White, fontWeight = FontWeight.SemiBold) }
                TextButton(onClick = onClose) { Text("Close", color = Color.White, fontWeight = FontWeight.SemiBold) }
            }
            error?.let { Text(it, color = Color.White, modifier = Modifier.align(Alignment.BottomCenter).padding(24.dp)) }
        }
    }
}

/** Pauses [player] when the app leaves the screen, and lets it go with the composable. */
@Composable
private fun rememberPlayer(channels: ChannelSession, media: ChannelMediaDto, podcast: Boolean = false): ChannelPlayer {
    val player = remember(media.id) {
        ChannelPlayer(
            source = { channels.rangeSource(media.url, media.size) },
            resumeAtSeconds = if (podcast) channels.position(media.id) else 0,
            onPosition = if (podcast) ({ seconds -> channels.savePosition(media.id, seconds) }) else null,
        )
    }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(player, lifecycle) {
        val observer = LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_STOP) player.pause() }
        lifecycle.addObserver(observer)
        onDispose {
            lifecycle.removeObserver(observer)
            player.release()
        }
    }
    LaunchedEffect(player, player.playing) {
        while (player.playing) {
            player.tick()
            delay(250)
        }
    }
    return player
}

/** A video: nothing is fetched until Play; then it streams by byte ranges through our own client. */
@Composable
private fun VideoPlayer(channels: ChannelSession, media: ChannelMediaDto) {
    val colors = RedvaultTheme.colors
    val player = rememberPlayer(channels, media)
    val ratio = when {
        player.videoWidth > 0 && player.videoHeight > 0 -> player.videoWidth.toFloat() / player.videoHeight
        (media.width ?: 0) > 0 && (media.height ?: 0) > 0 -> media.width!!.toFloat() / media.height!!
        else -> 16f / 9f
    }.coerceIn(0.5f, 2.4f)
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Box(
            Modifier.fillMaxWidth().heightIn(max = 420.dp).aspectRatio(ratio).clip(RoundedCornerShape(12.dp)).background(Color.Black)
                .clickable(role = Role.Button) { player.toggle() }
                .semantics { contentDescription = if (player.playing) "Pause video" else "Play video" },
            contentAlignment = Alignment.Center,
        ) {
            if (player.phase != ChannelPlayer.Phase.Idle) VideoSurface(player)
            when {
                player.phase == ChannelPlayer.Phase.Preparing -> CircularProgressIndicator(color = Color.White, strokeWidth = 2.dp, modifier = Modifier.size(28.dp))
                player.phase == ChannelPlayer.Phase.Failed -> Text("Couldn't play this video.", color = Color.White, fontSize = 13.sp)
                !player.playing -> PlayBadge(media.durationMs?.let { ChannelPosts.formatDuration(it / 1000.0) })
            }
        }
        if (player.phase == ChannelPlayer.Phase.Ready) SeekRow(player)
        else Text(listOfNotNull("Video", media.filename, FileMessage.formatBytes(media.size)).joinToString(" · "), color = colors.text3, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** A round video message: tap to play with sound, tap again to pause. */
@Composable
private fun RoundVideo(channels: ChannelSession, media: ChannelMediaDto) {
    val player = rememberPlayer(channels, media)
    Box(
        Modifier.size(220.dp).clip(CircleShape).background(Color.Black)
            .clickable(role = Role.Button) { player.toggle() }
            .semantics { contentDescription = if (player.playing) "Pause video message" else "Play video message" },
        contentAlignment = Alignment.Center,
    ) {
        if (player.phase != ChannelPlayer.Phase.Idle) VideoSurface(player, crop = true)
        when {
            player.phase == ChannelPlayer.Phase.Preparing -> CircularProgressIndicator(color = Color.White, strokeWidth = 2.dp, modifier = Modifier.size(28.dp))
            player.phase == ChannelPlayer.Phase.Failed -> Text("Couldn't play it.", color = Color.White, fontSize = 13.sp)
            !player.playing -> PlayBadge(media.durationMs?.let { ChannelPosts.formatDuration(it / 1000.0) })
        }
    }
}

@Composable
private fun PlayBadge(duration: String?) {
    Row(
        Modifier.background(Color.Black.copy(alpha = 0.55f), RoundedCornerShape(16.dp)).padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        DsIcon(R.drawable.ui_play, Color.White, 16.dp)
        if (duration != null) Text(duration, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
    }
}

/** The video's picture: a TextureView whose surface goes to the player (center-cropped for round videos). */
@Composable
private fun VideoSurface(player: ChannelPlayer, crop: Boolean = false) {
    AndroidView(
        factory = { context ->
            TextureView(context).apply {
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    var surface: Surface? = null
                    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                        surface = Surface(texture).also(player::setSurface)
                    }
                    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {}
                    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                        player.setSurface(null)
                        surface?.release()
                        return true
                    }
                    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
                }
            }
        },
        update = { view ->
            // A round video fills its circle: scale the picture up to cover the square, keeping its shape.
            if (crop && player.videoWidth > 0 && player.videoHeight > 0 && view.width > 0) {
                val w = player.videoWidth.toFloat()
                val h = player.videoHeight.toFloat()
                val matrix = android.graphics.Matrix()
                val sx = if (w > h) w / h else 1f
                val sy = if (h > w) h / w else 1f
                matrix.setScale(sx, sy, view.width / 2f, view.height / 2f)
                view.setTransform(matrix)
            }
        },
        modifier = Modifier.fillMaxSize(),
    )
}

@Composable
private fun SeekRow(player: ChannelPlayer) {
    val colors = RedvaultTheme.colors
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Slider(
            value = if (player.durationMs > 0) player.positionMs.toFloat() / player.durationMs else 0f,
            onValueChange = { player.seekTo((it * player.durationMs).toLong()) },
            colors = SliderDefaults.colors(thumbColor = colors.accent, activeTrackColor = colors.accent, inactiveTrackColor = colors.surface3),
            modifier = Modifier.weight(1f).semantics { contentDescription = "Position" },
        )
        Text(
            "${ChannelPosts.formatDuration(player.positionMs / 1000.0)} / ${ChannelPosts.formatDuration(player.durationMs / 1000.0)}",
            color = colors.text3,
            fontSize = 12.sp,
        )
    }
}

/**
 * Podcasts and long audio (resume where you stopped, skip 15 s,
 * 1×/1.5×/2×), and voice clips (the same, compact). Positions stay on this phone.
 */
@Composable
private fun AudioPlayer(channels: ChannelSession, media: ChannelMediaDto, podcast: Boolean) {
    val colors = RedvaultTheme.colors
    val player = rememberPlayer(channels, media, podcast)
    val saved = remember(media.id) { if (podcast) channels.position(media.id) else 0 }
    val duration = if (player.durationMs > 0) player.durationMs else media.durationMs ?: 0
    Column(
        Modifier.fillMaxWidth().background(colors.surface2, RoundedCornerShape(14.dp)).padding(horizontal = 10.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Box(
                Modifier.size(40.dp).clip(CircleShape).background(colors.accent).clickable(role = Role.Button) { player.toggle() }
                    .semantics { contentDescription = if (player.playing) "Pause" else "Play" },
                contentAlignment = Alignment.Center,
            ) {
                if (player.phase == ChannelPlayer.Phase.Preparing) CircularProgressIndicator(color = colors.accentContrast, strokeWidth = 2.dp, modifier = Modifier.size(18.dp))
                else DsIcon(if (player.playing) R.drawable.ui_pause else R.drawable.ui_play, colors.accentContrast, 18.dp)
            }
            Column(Modifier.weight(1f)) {
                if (podcast && media.filename != null) Text(media.filename, color = colors.text, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (!podcast) Text("Voice message", color = colors.text, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Slider(
                    value = if (duration > 0) (player.positionMs.toFloat() / duration).coerceIn(0f, 1f) else 0f,
                    onValueChange = { if (player.phase == ChannelPlayer.Phase.Ready) player.seekTo((it * duration).toLong()) },
                    enabled = player.phase == ChannelPlayer.Phase.Ready,
                    colors = SliderDefaults.colors(thumbColor = colors.accent, activeTrackColor = colors.accent, inactiveTrackColor = colors.surface3),
                    modifier = Modifier.fillMaxWidth().heightIn(max = 28.dp).semantics { contentDescription = "Position" },
                )
                val resumes = podcast && player.positionMs == 0L && saved > 5
                Text(
                    when {
                        player.phase == ChannelPlayer.Phase.Failed -> "Couldn't play it."
                        else -> "${ChannelPosts.formatDuration(player.positionMs / 1000.0)} / ${ChannelPosts.formatDuration(duration / 1000.0)}" +
                            if (resumes) " · resumes at ${ChannelPosts.formatDuration(saved.toDouble())}" else ""
                    },
                    color = colors.text3,
                    fontSize = 11.sp,
                )
            }
            if (!podcast) SmallAudioButton(ChannelPosts.speedLabel(player.speed), "Playback speed ${ChannelPosts.speedLabel(player.speed)}") { player.cycleSpeed() }
        }
        // A bubble is too narrow for these beside the slider.
        if (podcast) {
            Row(Modifier.align(Alignment.End), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                SmallAudioButton("−15", "Back 15 seconds", enabled = player.phase == ChannelPlayer.Phase.Ready) { player.skip(-15) }
                SmallAudioButton("+15", "Forward 15 seconds", enabled = player.phase == ChannelPlayer.Phase.Ready) { player.skip(15) }
                SmallAudioButton(ChannelPosts.speedLabel(player.speed), "Playback speed ${ChannelPosts.speedLabel(player.speed)}") { player.cycleSpeed() }
            }
        }
    }
}

@Composable
private fun SmallAudioButton(label: String, description: String, enabled: Boolean = true, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Text(
        label,
        color = if (enabled) colors.text else colors.text3,
        fontSize = 12.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description }
            .padding(horizontal = 6.dp, vertical = 8.dp),
    )
}

/** A file: Download (through our own client), then Open with another app or Save where you choose. */
@Composable
private fun FileRow(channels: ChannelSession, media: ChannelMediaDto) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    val scope = rememberCoroutineScope()
    var file by remember(media.id) { mutableStateOf<File?>(null) }
    var progress by remember(media.id) { mutableStateOf<Float?>(null) }
    var error by remember(media.id) { mutableStateOf<String?>(null) }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { target ->
        val ready = file ?: return@rememberLauncherForActivityResult
        if (target == null) return@rememberLauncherForActivityResult
        runCatching { context.contentResolver.openOutputStream(target)?.use { out -> ready.inputStream().use { it.copyTo(out) } } }
            .onFailure { error = "Couldn't save the file." }
    }
    Row(
        Modifier.fillMaxWidth().background(colors.surface2, RoundedCornerShape(14.dp)).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        DsIcon(R.drawable.ui_file, colors.text2, 22.dp)
        Column(Modifier.weight(1f)) {
            Text(media.filename ?: "File", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text(
                error ?: progress?.let { "Downloading… ${(it * 100).toInt()}%" } ?: FileMessage.formatBytes(media.size),
                color = if (error != null) colors.danger else colors.text3,
                fontSize = 12.sp,
            )
        }
        val ready = file
        if (ready == null) {
            TextButton(enabled = progress == null, onClick = {
                error = null
                progress = 0f
                scope.launch {
                    try {
                        file = channels.download(media.id, media.url) { p -> progress = p }
                    } catch (e: Exception) {
                        error = describeError(e, "Couldn't download it. Try again.")
                    } finally {
                        progress = null
                    }
                }
            }) { Text("Download", color = colors.accent, fontWeight = FontWeight.SemiBold) }
        } else {
            TextButton(onClick = {
                appLock?.expectExternalReturn()
                openFile(context, ready, if (media.mime == "application/octet-stream") "*/*" else media.mime) { error = it }
            }) { Text("Open", color = colors.accent, fontWeight = FontWeight.SemiBold) }
            TextButton(onClick = {
                appLock?.expectExternalReturn()
                save.launch(media.filename ?: "file")
            }) { Text("Save", color = colors.accent, fontWeight = FontWeight.SemiBold) }
        }
    }
}

/** A poll or quiz. You see the results once you've voted (or it's closed); a quiz shows whether you were right. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PollCard(
    channels: ChannelSession,
    channelId: String,
    postId: String,
    initial: ChannelPollDto,
    access: String?,
    isOwner: Boolean,
    onChanged: (ChannelPollDto) -> Unit,
) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var poll by remember(postId, initial) { mutableStateOf(initial) }
    var mine by remember(postId) { mutableStateOf<RememberedVote?>(channels.rememberedVote(postId)) }
    var picked by remember(postId) { mutableStateOf<List<Int>>(emptyList()) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val showResults = mine != null || poll.closed
    val correct = poll.correct ?: mine?.correct
    val explanation = poll.explanation ?: mine?.explanation

    fun vote(options: List<Int>) {
        busy = true
        error = null
        scope.launch {
            try {
                val result = channels.vote(channelId, postId, options, access)
                poll = result.poll
                mine = channels.rememberedVote(postId)
                picked = emptyList()
                onChanged(result.poll)
            } catch (e: Exception) {
                error = describeError(e, "Couldn't send your vote. Try again.")
            } finally {
                busy = false
            }
        }
    }

    val kind = if (poll.quiz) "Quiz" else if (poll.multiple) "Poll · pick any" else "Poll"
    Column(
        Modifier.fillMaxWidth().background(colors.surface2, RoundedCornerShape(14.dp)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(kind + if (poll.closed) " · closed" else "", color = colors.text3, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
        Text(poll.question, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
        val percents = ChannelPosts.percents(poll)
        poll.options.forEachIndexed { i, option ->
            if (!showResults) {
                val on = i in picked
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(10.dp))
                        .border(1.dp, if (on) colors.accent else colors.border, RoundedCornerShape(10.dp))
                        .background(if (on) colors.accentSoft else colors.surface)
                        .clickable(enabled = !busy, role = if (poll.multiple) Role.Checkbox else Role.Button) {
                            if (poll.multiple) picked = if (on) picked - i else picked + i else vote(listOf(i))
                        }
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    if (poll.multiple) Text(if (on) "☑" else "☐", color = if (on) colors.accent else colors.text3, fontSize = 15.sp)
                    Text(option, color = colors.text, fontSize = 14.sp)
                }
            } else {
                val chosen = mine?.mine?.contains(i) == true
                val right = poll.quiz && correct == i
                val wrong = poll.quiz && chosen && correct != null && correct != i
                val tint = when {
                    right -> colors.accent
                    wrong -> colors.danger
                    else -> colors.text3
                }
                val share = percents[i] / 100f
                Box(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(colors.surface)
                        .drawBehind { drawRect(tint.copy(alpha = 0.18f), size = Size(size.width * share, size.height)) }
                        .then(if (right || wrong) Modifier.border(1.dp, tint, RoundedCornerShape(10.dp)) else Modifier),
                ) {
                    Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 10.dp)) {
                        Text(
                            (if (chosen) "✓ " else "") + option + if (right) " (right answer)" else "",
                            color = colors.text,
                            fontSize = 14.sp,
                            fontWeight = if (chosen) FontWeight.SemiBold else FontWeight.Normal,
                            modifier = Modifier.weight(1f),
                        )
                        Text("${percents[i]}%", color = colors.text2, fontSize = 13.sp)
                    }
                }
            }
        }
        if (!showResults && poll.multiple) {
            TextButton(enabled = !busy && picked.isNotEmpty(), onClick = { vote(picked.sorted()) }) {
                Text("Vote", color = if (picked.isNotEmpty()) colors.accent else colors.text3, fontWeight = FontWeight.SemiBold)
            }
        }
        if (poll.quiz && showResults && !explanation.isNullOrBlank()) Text(explanation, color = colors.text2, fontSize = 13.sp)
        error?.let { Text(it, color = colors.danger, fontSize = 12.sp) }
        Text(
            "${poll.voters} ${if (poll.voters == 1) "vote" else "votes"}" + if (!showResults) " · results show after you vote" else "",
            color = colors.text3,
            fontSize = 12.sp,
        )
        // On their own line: a bubble is too narrow to share it with the count.
        FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            if (mine != null && !poll.quiz && !poll.closed) {
                PollLink("Take back my vote", enabled = !busy) { vote(emptyList()) }
            }
            if (isOwner && !poll.closed) {
                PollLink(if (poll.quiz) "Close and show the answer" else "Close poll", enabled = !busy) {
                    busy = true
                    scope.launch {
                        try {
                            channels.closePoll(channelId, postId).poll?.let {
                                poll = it
                                onChanged(it)
                            }
                        } catch (e: Exception) {
                            error = describeError(e, "Couldn't close the poll.")
                        } finally {
                            busy = false
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PollLink(text: String, enabled: Boolean, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Text(
        text,
        color = if (enabled) colors.accent else colors.text3,
        fontSize = 12.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.clip(RoundedCornerShape(6.dp)).clickable(enabled = enabled, role = Role.Button, onClick = onClick).padding(6.dp),
    )
}

/** A checklist: the owner ticks items, and so can followers when it's collaborative. */
@Composable
private fun ChecklistCard(
    channels: ChannelSession,
    channelId: String,
    postId: String,
    checklist: ChannelChecklistDto,
    access: String?,
    canTick: Boolean,
    onChanged: (ChannelChecklistDto) -> Unit,
) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var items by remember(postId, checklist) { mutableStateOf(checklist.items) }
    var error by remember { mutableStateOf<String?>(null) }
    val done = items.count { it.done }
    Column(
        Modifier.fillMaxWidth().background(colors.surface2, RoundedCornerShape(14.dp)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(checklist.title, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
        Text(
            "$done of ${items.size} done" + if (checklist.collaborative) " · anyone following can tick items" else "",
            color = colors.text3,
            fontSize = 12.sp,
        )
        items.forEachIndexed { index, item ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(8.dp))
                    .clickable(enabled = canTick, role = Role.Checkbox) {
                        val before = items
                        val next = items.mapIndexed { i, it -> if (i == index) it.copy(done = !it.done) else it }
                        items = next
                        error = null
                        scope.launch {
                            try {
                                val saved = channels.setChecklistItem(channelId, postId, index, next[index].done, access)
                                items = saved.items
                                onChanged(saved)
                            } catch (e: Exception) {
                                items = before
                                error = describeError(e, "Couldn't update the checklist.")
                            }
                        }
                    }
                    .padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Box(
                    Modifier.size(20.dp).clip(RoundedCornerShape(5.dp))
                        .background(if (item.done) colors.accent else Color.Transparent)
                        .border(1.5.dp, if (item.done) colors.accent else colors.text3, RoundedCornerShape(5.dp)),
                    contentAlignment = Alignment.Center,
                ) {
                    if (item.done) DsIcon(R.drawable.ui_check, colors.accentContrast, 14.dp)
                }
                Text(
                    item.text,
                    color = if (item.done) colors.text3 else colors.text,
                    fontSize = 14.sp,
                    textDecoration = if (item.done) TextDecoration.LineThrough else null,
                    modifier = Modifier.weight(1f),
                )
            }
        }
        error?.let { Text(it, color = colors.danger, fontSize = 12.sp) }
    }
}
