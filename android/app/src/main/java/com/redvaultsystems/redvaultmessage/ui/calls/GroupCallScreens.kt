package com.redvaultsystems.redvaultmessage.ui.calls

import android.app.Activity
import android.media.projection.MediaProjectionManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.calls.GroupCallManager
import com.redvaultsystems.redvaultmessage.calls.GroupCallView
import com.redvaultsystems.redvaultmessage.calls.GroupPhase
import com.redvaultsystems.redvaultmessage.calls.GroupTile
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.groups.GroupAvatar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import io.livekit.android.renderer.TextureViewRenderer
import kotlinx.coroutines.delay
import livekit.org.webrtc.RendererCommon

/**
 * A group call, over everything else, while there is one: the ring, then a
 * grid of people (or a shared screen with everyone beside it). The web's
 * components/GroupCallOverlay.tsx. [onMinimize] tucks it away; [onOpenChat]
 * goes to the group's chat.
 */
@Composable
fun GroupCallScreen(calls: GroupCallManager, call: GroupCallView, onMinimize: () -> Unit, onOpenChat: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val askPermissions = rememberCallPermissions { message -> android.widget.Toast.makeText(context, message, android.widget.Toast.LENGTH_LONG).show() }
    Box(Modifier.fillMaxSize().background(colors.bg)) {
        if (call.phase == GroupPhase.Incoming) {
            IncomingGroupCall(
                call,
                onDecline = calls::decline,
                onJoin = { askPermissions(call.kind == "video") { calls.accept() } },
            )
        } else {
            InGroupCall(calls, call, askPermissions, onMinimize, onOpenChat)
        }
    }
}

@Composable
private fun IncomingGroupCall(call: GroupCallView, onDecline: () -> Unit, onJoin: () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 24.dp, vertical = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        EncryptedPill("End-to-end encrypted")
        Spacer(Modifier.weight(1f))
        GroupAvatar(call.groupLabel, size = 132.dp)
        Spacer(Modifier.height(22.dp))
        Text(
            call.groupLabel,
            fontFamily = RedvaultFonts.display,
            fontSize = 34.sp,
            fontWeight = FontWeight.Bold,
            color = colors.text,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.Center,
        )
        val what = if (call.kind == "video") "group video call" else "group voice call"
        Text(
            if (call.hostHandle != null) "${call.hostHandle} started a $what" else "Incoming $what…",
            color = colors.text2,
            fontSize = 16.sp,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 6.dp),
        )
        Spacer(Modifier.weight(1.4f))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.Top) {
            BigButton(R.drawable.ui_phone, "Decline", DangerRed, Color.White, rotate = 135f, onClick = onDecline)
            BigButton(if (call.kind == "video") R.drawable.ui_video else R.drawable.ui_phone, "Join", colors.accent, colors.accentContrast, onClick = onJoin)
        }
        Spacer(Modifier.height(20.dp))
    }
}

@Composable
private fun InGroupCall(
    calls: GroupCallManager,
    call: GroupCallView,
    askPermissions: (video: Boolean, action: (cameraGranted: Boolean) -> Unit) -> Unit,
    onMinimize: () -> Unit,
    onOpenChat: (String) -> Unit,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val projection = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val data = result.data
        if (result.resultCode == Activity.RESULT_OK && data != null) calls.startScreenShare(data)
    }
    val people = call.tiles.count { !it.screen }
    val status = when (call.phase) {
        GroupPhase.Connecting -> "Connecting…"
        GroupPhase.Reconnecting -> "Reconnecting…"
        GroupPhase.Active -> "$people ${if (people == 1) "person" else "people"} · ${elapsedText(call)}"
        else -> ""
    }
    Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 12.dp, vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            RoundIcon(R.drawable.ui_chevron_down, "Minimize call", Color.Transparent, colors.text, 44.dp, onMinimize)
            Column(Modifier.weight(1f).padding(horizontal = 8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    DsIcon(R.drawable.ui_users, colors.text, 15.dp)
                    Text(call.groupLabel, color = colors.text, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    DsIcon(R.drawable.ui_lock, colors.accent, 12.dp)
                    Text("Encrypted · $status", color = colors.text2, fontFamily = MonoFamily, fontSize = 12.sp)
                }
            }
            if (call.phase != GroupPhase.Ended) {
                RoundIcon(R.drawable.ui_chat, "Open chat", Color.Transparent, colors.text, 44.dp) { onMinimize(); onOpenChat(call.conversationId) }
            }
        }
        Spacer(Modifier.height(8.dp))
        if (call.sharingScreen) Banner("You're sharing your screen", colors.accent, R.drawable.ui_monitor_up)
        if (call.phase == GroupPhase.Active && !call.keyReady) Banner("Waiting for the call key… nothing is sent until it arrives", colors.text2, R.drawable.ui_key)
        call.notice?.let { Banner(it, colors.warn, R.drawable.ui_alert_circle) }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            if (call.phase == GroupPhase.Ended) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { GroupEndedBanner(call.endedMessage ?: "Call ended", calls::dismiss) }
            } else {
                Tiles(calls, call.tiles)
            }
        }
        if (call.sas != null && call.phase != GroupPhase.Ended) {
            Spacer(Modifier.height(8.dp))
            Column(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Color(0xFF16271F)).border(1.dp, Color(0xFF244536), RoundedCornerShape(14.dp)).padding(10.dp)
                    .semantics { contentDescription = "Safety words: ${call.sas.joinToString(" ")}. Everyone in the call should see these same words." },
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    DsIcon(R.drawable.ui_shield_check, colors.accent, 16.dp)
                    Text("Everyone in the call should see these same words", color = colors.text2, fontSize = 12.sp)
                }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                    call.sas.forEach { Text(it, color = colors.accent, fontFamily = MonoFamily, fontSize = 15.sp, fontWeight = FontWeight.Medium) }
                }
            }
        }
        if (call.phase != GroupPhase.Ended) {
            Spacer(Modifier.height(14.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.CenterVertically) {
                ToggleButton(
                    if (call.muted) R.drawable.ui_mic_off else R.drawable.ui_mic,
                    if (call.muted) "Unmute microphone" else "Mute microphone",
                    call.muted, 54.dp, calls::toggleMute, label = false,
                )
                if (call.kind == "video") {
                    ToggleButton(
                        if (call.cameraOff) R.drawable.ui_video_off else R.drawable.ui_video,
                        if (call.cameraOff) "Turn camera on" else "Turn camera off",
                        call.cameraOff, 54.dp,
                        { if (call.cameraOff) askPermissions(true) { camera -> if (camera) calls.toggleCamera() } else calls.toggleCamera() },
                        label = false,
                    )
                    if (!call.cameraOff) ToggleButton(R.drawable.ui_refresh, "Flip camera", false, 54.dp, calls::flipCamera, label = false)
                }
                ToggleButton(R.drawable.ui_speaker, "Speaker", call.speakerOn, 54.dp, calls::toggleSpeaker, label = false)
                if (calls.canShareScreen) {
                    ToggleButton(
                        R.drawable.ui_monitor_up,
                        if (call.sharingScreen) "Stop sharing your screen" else "Share your screen",
                        call.sharingScreen, 54.dp,
                        {
                            if (call.sharingScreen) calls.stopScreenShare()
                            else projection.launch(context.getSystemService(MediaProjectionManager::class.java).createScreenCaptureIntent())
                        },
                        label = false,
                    )
                }
            }
            Spacer(Modifier.height(14.dp))
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                BigButton(R.drawable.ui_phone, null, DangerRed, Color.White, rotate = 135f, description = "Leave call", onClick = calls::leave)
            }
        }
        Spacer(Modifier.height(8.dp))
    }
}

/** Everyone's tiles: a grid, or a shared screen big with everyone else in a strip. */
@Composable
private fun Tiles(calls: GroupCallManager, tiles: List<GroupTile>) {
    if (tiles.isEmpty()) return
    // Someone else's shared screen first: yours you can already see.
    val screens = tiles.filter { it.screen }
    val spotlight = screens.firstOrNull { !it.isSelf } ?: screens.firstOrNull()
    if (spotlight != null) {
        val others = tiles.filter { it !== spotlight }
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            TileView(calls, spotlight, Modifier.weight(1f).fillMaxWidth())
            LazyRow(Modifier.fillMaxWidth().height(110.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                items(others, key = { it.key }) { TileView(calls, it, Modifier.width(120.dp).fillMaxSize()) }
            }
        }
        return
    }
    val rows = tiles.chunked(2)
    if (rows.size <= 3) {
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            rows.forEach { row ->
                Row(Modifier.weight(1f).fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    row.forEach { TileView(calls, it, Modifier.weight(1f).fillMaxSize()) }
                    if (row.size == 1 && rows.size > 1) Spacer(Modifier.weight(1f))
                }
            }
        }
    } else {
        LazyColumn(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(rows, key = { row -> row.first().key }) { row ->
                Row(Modifier.fillMaxWidth().height(180.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    row.forEach { TileView(calls, it, Modifier.weight(1f).fillMaxSize()) }
                    if (row.size == 1) Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
private fun TileView(calls: GroupCallManager, tile: GroupTile, modifier: Modifier) {
    val colors = RedvaultTheme.colors
    val shape = RoundedCornerShape(16.dp)
    val name = if (tile.screen) "${tile.handle}${if (tile.handle == "You") "r" else "'s"} screen" else tile.handle
    Box(
        modifier.clip(shape).background(Color(0xFF16271F)).border(2.dp, if (tile.speaking) colors.accent else Color.Transparent, shape)
            .semantics { contentDescription = if (tile.micMuted && !tile.screen) "$name, microphone off" else name },
    ) {
        val video = tile.video
        if (video != null) {
            key(video) { TrackSurface(calls, video, mirror = tile.mirrored && !tile.screen, fit = tile.screen, modifier = Modifier.fillMaxSize()) }
        } else {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { Avatar(tile.avatarUrl, tile.userId ?: tile.key, size = 72.dp) }
        }
        Row(
            Modifier.align(Alignment.BottomStart).padding(8.dp).clip(RoundedCornerShape(10.dp)).background(Color.Black.copy(alpha = 0.5f)).padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            if (tile.screen) DsIcon(R.drawable.ui_monitor_up, Color.White, 13.dp) else if (tile.micMuted) DsIcon(R.drawable.ui_mic_off, Color.White, 13.dp)
            Text(name, color = Color.White, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

/** A person's camera or a shared screen, drawn by LiveKit's renderer. */
@Composable
private fun TrackSurface(calls: GroupCallManager, track: io.livekit.android.room.track.VideoTrack, mirror: Boolean, fit: Boolean, modifier: Modifier) {
    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            TextureViewRenderer(ctx).apply {
                calls.attach(this, track)
                setScalingType(if (fit) RendererCommon.ScalingType.SCALE_ASPECT_FIT else RendererCommon.ScalingType.SCALE_ASPECT_FILL)
                setMirror(mirror)
                setEnableHardwareScaler(true)
            }
        },
        update = { it.setMirror(mirror) },
        onRelease = { calls.detach(it, track) },
    )
}

@Composable
private fun Banner(text: String, color: Color, icon: Int) {
    Row(
        Modifier.fillMaxWidth().padding(bottom = 6.dp).clip(RoundedCornerShape(12.dp)).background(Color(0xFF16271F)).padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        DsIcon(icon, color, 15.dp)
        Text(text, color = color, fontSize = 13.sp)
    }
}

@Composable
private fun GroupEndedBanner(text: String, onDismiss: () -> Unit) {
    Text(
        text,
        color = RedvaultTheme.colors.text,
        fontSize = 15.sp,
        textAlign = TextAlign.Center,
        modifier = Modifier.padding(horizontal = 24.dp).clip(RoundedCornerShape(14.dp)).background(RedvaultTheme.colors.surface2).clickable(onClick = onDismiss)
            .padding(horizontal = 18.dp, vertical = 12.dp),
    )
}

/** "00:12" / "1:02:03" since the call connected. */
@Composable
private fun elapsedText(call: GroupCallView): String {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(call.phase) {
        while (call.phase == GroupPhase.Active) {
            now = System.currentTimeMillis()
            delay(1000)
        }
    }
    val seconds = ((now - (call.connectedAt ?: now)) / 1000).coerceAtLeast(0)
    return if (seconds >= 3600) "%d:%02d:%02d".format(seconds / 3600, (seconds % 3600) / 60, seconds % 60) else "%02d:%02d".format(seconds / 60, seconds % 60)
}

/** A bar over the app while a minimized group call is still on: tap to return. */
@Composable
fun GroupCallBar(call: GroupCallView, onReturn: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier.fillMaxWidth().background(colors.accent).statusBarsPadding().clickable(onClick = onReturn).padding(horizontal = 20.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        DsIcon(R.drawable.ui_users, colors.accentContrast, 18.dp)
        val status = when (call.phase) {
            GroupPhase.Connecting -> "Connecting…"
            GroupPhase.Reconnecting -> "Reconnecting…"
            GroupPhase.Ended -> call.endedMessage ?: "Call ended"
            else -> elapsedText(call)
        }
        Text("${call.groupLabel} · $status", color = colors.accentContrast, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f), maxLines = 1)
        Text("Return", color = colors.accentContrast, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
    }
}
