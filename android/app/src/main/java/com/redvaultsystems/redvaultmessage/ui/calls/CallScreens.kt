package com.redvaultsystems.redvaultmessage.ui.calls

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.calls.CallManager
import com.redvaultsystems.redvaultmessage.calls.CallPhase
import com.redvaultsystems.redvaultmessage.calls.CallView
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import org.webrtc.RendererCommon
import org.webrtc.SurfaceViewRenderer

internal val DangerRed = Color(0xFFC8433B)
internal val ControlBg = Color(0xFF1F2623)

/**
 * Runs [action] once the microphone (and, for video, the camera) are
 * granted, asking if they aren't yet. Video without the camera falls back
 * to voice; no microphone means no call.
 */
@Composable
fun rememberCallPermissions(onDenied: (String) -> Unit): (video: Boolean, action: (cameraGranted: Boolean) -> Unit) -> Unit {
    val context = LocalContext.current
    var pending by remember { mutableStateOf<((Boolean) -> Unit)?>(null) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { results ->
        val action = pending ?: return@rememberLauncherForActivityResult
        pending = null
        val mic = results[Manifest.permission.RECORD_AUDIO] ?: granted(context, Manifest.permission.RECORD_AUDIO)
        if (!mic) return@rememberLauncherForActivityResult onDenied("Calls need the microphone. Allow it in Settings to call.")
        action(results[Manifest.permission.CAMERA] ?: granted(context, Manifest.permission.CAMERA))
    }
    return { video, action ->
        val needed = buildList {
            if (!granted(context, Manifest.permission.RECORD_AUDIO)) add(Manifest.permission.RECORD_AUDIO)
            if (video && !granted(context, Manifest.permission.CAMERA)) add(Manifest.permission.CAMERA)
        }
        if (needed.isEmpty()) action(!video || granted(context, Manifest.permission.CAMERA))
        else {
            pending = action
            launcher.launch(needed.toTypedArray())
        }
    }
}

private fun granted(context: android.content.Context, permission: String) =
    ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

/** The call, over everything else, while there is one. [onMinimize] tucks it away; [onOpenChat] goes to the chat. */
@Composable
fun CallScreen(calls: CallManager, call: CallView, onMinimize: () -> Unit, onOpenChat: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    val askPermissions = rememberCallPermissions { calls.clearNotice() }
    Box(Modifier.fillMaxSize().background(colors.bg)) {
        when {
            call.phase == CallPhase.Incoming -> IncomingCall(
                call,
                onDecline = calls::decline,
                onAudioOnly = { askPermissions(false) { calls.accept(audioOnly = true) } },
                onAccept = { askPermissions(call.kind == "video") { camera -> calls.accept(audioOnly = !camera) } },
                onMessage = { call.conversationId?.let { calls.decline(); onOpenChat(it) } },
            )
            call.kind == "video" && call.phase != CallPhase.Ended || call.remoteVideo || !call.cameraOff -> VideoCall(calls, call, onMinimize, onOpenChat)
            else -> VoiceCall(calls, call, onMinimize, onOpenChat)
        }
    }
}

@Composable
private fun IncomingCall(call: CallView, onDecline: () -> Unit, onAudioOnly: () -> Unit, onAccept: () -> Unit, onMessage: () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 24.dp, vertical = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        EncryptedPill("End-to-end encrypted")
        Spacer(Modifier.weight(1f))
        Avatar(call.peerAvatarUrl, call.peerHandle, size = 132.dp)
        Spacer(Modifier.height(22.dp))
        Text(call.peerHandle, fontFamily = RedvaultFonts.display, fontSize = 34.sp, fontWeight = FontWeight.Bold, color = colors.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(
            when {
                call.linkName != null -> "Wants to join “${call.linkName}”…"
                call.kind == "video" -> "Incoming video call…"
                else -> "Incoming voice call…"
            },
            color = colors.text2,
            fontSize = 16.sp,
            modifier = Modifier.padding(top = 6.dp),
        )
        Spacer(Modifier.weight(1.2f))
        if (call.conversationId != null) {
            Row(Modifier.padding(bottom = 28.dp)) {
                SmallAction(R.drawable.ui_chat, "Message", onMessage)
            }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.Top) {
            BigButton(R.drawable.ui_phone, "Decline", DangerRed, Color.White, rotate = 135f, onClick = onDecline)
            if (call.kind == "video") BigButton(R.drawable.ui_mic, "Audio only", ControlBg, colors.text, onClick = onAudioOnly)
            BigButton(if (call.kind == "video") R.drawable.ui_video else R.drawable.ui_phone, "Accept", colors.accent, colors.accentContrast, onClick = onAccept)
        }
        Spacer(Modifier.height(20.dp))
    }
}

@Composable
private fun VoiceCall(calls: CallManager, call: CallView, onMinimize: () -> Unit, onOpenChat: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    val askPermissions = rememberCallPermissions { }
    Column(
        Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 20.dp, vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            RoundIcon(R.drawable.ui_chevron_down, "Minimize call", Color.Transparent, colors.text, 44.dp, onMinimize)
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) { EncryptedPill("Encrypted call") }
            Spacer(Modifier.width(44.dp))
        }
        Spacer(Modifier.weight(0.6f))
        Avatar(call.peerAvatarUrl, call.peerHandle, size = 140.dp)
        Spacer(Modifier.height(20.dp))
        Text(call.peerHandle, fontFamily = RedvaultFonts.display, fontSize = 32.sp, fontWeight = FontWeight.Bold, color = colors.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
        CallStatus(call)
        Spacer(Modifier.weight(0.5f))
        SafetyWords(call)
        Spacer(Modifier.weight(0.5f))
        call.notice?.let { Text(it, color = colors.warn, fontSize = 13.sp, textAlign = TextAlign.Center, modifier = Modifier.padding(bottom = 12.dp)) }
        if (call.phase != CallPhase.Ended) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                ToggleButton(if (call.muted) R.drawable.ui_mic_off else R.drawable.ui_mic, if (call.muted) "Unmute" else "Mute", call.muted, 64.dp, calls::toggleMute)
                ToggleButton(R.drawable.ui_speaker, "Speaker", call.speakerOn, 64.dp, calls::toggleSpeaker)
                ToggleButton(R.drawable.ui_video, "Video", false, 64.dp, onClick = { askPermissions(true) { camera -> if (camera) calls.toggleCamera() } })
                if (call.conversationId != null) ToggleButton(R.drawable.ui_chat, "Chat", false, 64.dp, onClick = { onMinimize(); onOpenChat(call.conversationId) })
            }
            Spacer(Modifier.height(26.dp))
            BigButton(R.drawable.ui_phone, null, DangerRed, Color.White, rotate = 135f, description = "End call", onClick = calls::hangUp)
        } else {
            EndedBanner(call, calls::dismiss)
        }
        Spacer(Modifier.height(12.dp))
    }
}

@Composable
private fun VideoCall(calls: CallManager, call: CallView, onMinimize: () -> Unit, onOpenChat: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val projection = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val data = result.data
        if (result.resultCode == Activity.RESULT_OK && data != null) calls.startScreenShare(data)
    }
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        if (call.remoteVideo) {
            VideoSurface(calls, local = false, modifier = Modifier.fillMaxSize(), fit = call.remoteSharing)
        } else {
            Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
                Avatar(call.peerAvatarUrl, call.peerHandle, size = 120.dp)
                Text(if (call.phase == CallPhase.Active) "Camera off" else "", color = colors.text2, fontSize = 14.sp, modifier = Modifier.padding(top = 12.dp))
            }
        }
        // Top: minimize, who and how long.
        Row(
            Modifier.fillMaxWidth().statusBarsPadding().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            RoundIcon(R.drawable.ui_chevron_down, "Minimize call", Color.Black.copy(alpha = 0.35f), Color.White, 44.dp, onMinimize)
            Column(Modifier.weight(1f).padding(horizontal = 10.dp)) {
                Text(call.peerHandle, color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    DsIcon(R.drawable.ui_lock, colors.accent, 12.dp)
                    Text("Encrypted · ${statusText(call)}", color = Color.White.copy(alpha = 0.85f), fontFamily = MonoFamily, fontSize = 12.sp)
                }
                if (call.remoteSharing) Text("Sharing their screen", color = colors.accent, fontSize = 12.sp)
            }
            if (call.conversationId != null && call.phase != CallPhase.Ended) {
                RoundIcon(R.drawable.ui_chat, "Open chat", Color.Black.copy(alpha = 0.35f), Color.White, 44.dp) { onMinimize(); onOpenChat(call.conversationId) }
            }
        }
        // Your camera in a corner.
        if (!call.cameraOff && !call.sharingScreen) {
            Box(
                Modifier.align(Alignment.TopEnd).statusBarsPadding().padding(top = 72.dp, end = 14.dp)
                    .size(width = 104.dp, height = 148.dp).clip(RoundedCornerShape(16.dp)).border(1.dp, Color.White.copy(alpha = 0.25f), RoundedCornerShape(16.dp)),
            ) { VideoSurface(calls, local = true, modifier = Modifier.fillMaxSize(), mirror = true, overlay = true) }
        }
        Column(
            Modifier.align(Alignment.BottomCenter).fillMaxWidth().navigationBarsPadding().padding(bottom = 18.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            if (call.sas != null && call.phase == CallPhase.Active) {
                Text(
                    call.sas.joinToString("  "),
                    color = Color.White,
                    fontFamily = MonoFamily,
                    fontSize = 14.sp,
                    modifier = Modifier.clip(RoundedCornerShape(12.dp)).background(Color.Black.copy(alpha = 0.45f)).padding(horizontal = 12.dp, vertical = 6.dp)
                        .semantics { contentDescription = "Safety words: ${call.sas.joinToString(" ")}. Say them aloud; if the other person sees the same, the call is secure." },
                )
                Spacer(Modifier.height(12.dp))
            }
            if (call.sharingScreen) Text("You're sharing your screen", color = colors.accent, fontSize = 13.sp, modifier = Modifier.padding(bottom = 8.dp))
            call.notice?.let { Text(it, color = colors.warn, fontSize = 13.sp, textAlign = TextAlign.Center, modifier = Modifier.padding(horizontal = 24.dp, vertical = 6.dp)) }
            if (call.phase == CallPhase.Ended) {
                EndedBanner(call, calls::dismiss)
            } else {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.CenterVertically) {
                    ToggleButton(if (call.muted) R.drawable.ui_mic_off else R.drawable.ui_mic, if (call.muted) "Unmute microphone" else "Mute microphone", call.muted, 56.dp, calls::toggleMute, label = false)
                    ToggleButton(if (call.cameraOff) R.drawable.ui_video_off else R.drawable.ui_video, if (call.cameraOff) "Turn camera on" else "Turn camera off", call.cameraOff, 56.dp, calls::toggleCamera, label = false)
                    ToggleButton(R.drawable.ui_refresh, "Flip camera", false, 56.dp, calls::flipCamera, label = false)
                    ToggleButton(R.drawable.ui_speaker, "Speaker", call.speakerOn, 56.dp, calls::toggleSpeaker, label = false)
                    if (calls.canShareScreen) {
                        ToggleButton(R.drawable.ui_monitor_up, if (call.sharingScreen) "Stop sharing your screen" else "Share your screen", call.sharingScreen, 56.dp, {
                            if (call.sharingScreen) calls.stopScreenShare()
                            else {
                                val manager = context.getSystemService(MediaProjectionManager::class.java)
                                projection.launch(manager.createScreenCaptureIntent())
                            }
                        }, label = false)
                    }
                }
                Spacer(Modifier.height(18.dp))
                BigButton(R.drawable.ui_phone, null, DangerRed, Color.White, rotate = 135f, description = "End call", onClick = calls::hangUp)
            }
        }
    }
}

/** A WebRTC video surface: the other side's video, or your camera (mirrored, drawn above the other surface). */
@Composable
private fun VideoSurface(calls: CallManager, local: Boolean, modifier: Modifier, mirror: Boolean = false, overlay: Boolean = false, fit: Boolean = false) {
    var renderer by remember { mutableStateOf<SurfaceViewRenderer?>(null) }
    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            SurfaceViewRenderer(ctx).apply {
                init(calls.eglContext, null)
                setScalingType(if (fit) RendererCommon.ScalingType.SCALE_ASPECT_FIT else RendererCommon.ScalingType.SCALE_ASPECT_FILL)
                setMirror(mirror)
                setEnableHardwareScaler(true)
                if (overlay) setZOrderMediaOverlay(true)
                if (local) calls.attachLocal(this) else calls.attachRemote(this)
                renderer = this
            }
        },
        update = { it.setScalingType(if (fit) RendererCommon.ScalingType.SCALE_ASPECT_FIT else RendererCommon.ScalingType.SCALE_ASPECT_FILL) },
    )
    DisposableEffect(Unit) {
        onDispose {
            renderer?.let {
                if (local) calls.detachLocal(it) else calls.detachRemote(it)
                it.release()
            }
        }
    }
}

@Composable
private fun CallStatus(call: CallView) {
    val colors = RedvaultTheme.colors
    Text(statusText(call), color = colors.text2, fontFamily = MonoFamily, fontSize = 17.sp, modifier = Modifier.padding(top = 6.dp))
    if (call.phase == CallPhase.Active) {
        Text("Relayed · end-to-end encrypted", color = colors.text3, fontSize = 13.sp, modifier = Modifier.padding(top = 2.dp))
    }
}

/** "Calling…", "Ringing…", "Connecting…", the timer, or why it ended. */
@Composable
private fun statusText(call: CallView): String {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(call.phase) {
        while (call.phase == CallPhase.Active) {
            now = System.currentTimeMillis()
            delay(1000)
        }
    }
    return when (call.phase) {
        CallPhase.Outgoing -> "Ringing…"
        CallPhase.Incoming -> "Incoming…"
        CallPhase.Connecting -> "Connecting…"
        CallPhase.Reconnecting -> "Reconnecting…"
        CallPhase.Ended -> call.endedMessage ?: "Call ended"
        CallPhase.Active -> {
            val seconds = ((now - (call.connectedAt ?: now)) / 1000).coerceAtLeast(0)
            if (seconds >= 3600) "%d:%02d:%02d".format(seconds / 3600, (seconds % 3600) / 60, seconds % 60) else "%02d:%02d".format(seconds / 60, seconds % 60)
        }
    }
}

/** The four words from both DTLS fingerprints, to read aloud. */
@Composable
private fun SafetyWords(call: CallView) {
    val colors = RedvaultTheme.colors
    val words = call.sas ?: return
    if (call.phase != CallPhase.Active && call.phase != CallPhase.Reconnecting) return
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Color(0xFF16271F)).border(1.dp, Color(0xFF244536), RoundedCornerShape(16.dp)).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            DsIcon(R.drawable.ui_shield_check, colors.accent, 16.dp)
            Text("Say these words aloud. If ${call.peerHandle} sees the same, the call is secure.", color = colors.text2, fontSize = 13.sp)
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
            words.forEach { Text(it, color = colors.accent, fontFamily = MonoFamily, fontSize = 16.sp, fontWeight = FontWeight.Medium) }
        }
    }
}

@Composable
internal fun EndedBanner(call: CallView, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    Text(
        call.endedMessage ?: "Call ended",
        color = colors.text,
        fontSize = 15.sp,
        textAlign = TextAlign.Center,
        modifier = Modifier.padding(horizontal = 24.dp).clip(RoundedCornerShape(14.dp)).background(colors.surface2).clickable(onClick = onDismiss).padding(horizontal = 18.dp, vertical = 12.dp),
    )
}

@Composable
internal fun EncryptedPill(text: String) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier.clip(RoundedCornerShape(16.dp)).background(Color(0xFF16271F)).padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        DsIcon(R.drawable.ui_lock, colors.accent, 13.dp)
        Text(text, color = colors.accent, fontSize = 13.sp, fontWeight = FontWeight.Medium)
    }
}

@Composable
internal fun BigButton(icon: Int, label: String?, bg: Color, fg: Color, rotate: Float = 0f, description: String? = label, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Box(
            Modifier.size(76.dp).clip(CircleShape).background(bg).clickable(role = Role.Button, onClick = onClick).semantics { contentDescription = description ?: "" },
            contentAlignment = Alignment.Center,
        ) { DsIcon(icon, fg, 30.dp, modifier = Modifier.rotate(rotate)) }
        if (label != null) Text(label, color = RedvaultTheme.colors.text2, fontSize = 13.sp)
    }
}

@Composable
internal fun ToggleButton(icon: Int, text: String, on: Boolean, size: Dp, onClick: () -> Unit, label: Boolean = true) {
    val colors = RedvaultTheme.colors
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(
            Modifier.size(size).clip(CircleShape).background(if (on) colors.text else ControlBg).clickable(role = Role.Button, onClick = onClick)
                .semantics { contentDescription = text },
            contentAlignment = Alignment.Center,
        ) { DsIcon(icon, if (on) colors.bg else colors.text, 24.dp) }
        if (label) Text(text, color = colors.text2, fontSize = 12.sp)
    }
}

@Composable
internal fun RoundIcon(icon: Int, description: String, bg: Color, fg: Color, size: Dp, onClick: () -> Unit) {
    Box(
        Modifier.size(size).clip(CircleShape).background(bg).clickable(role = Role.Button, onClick = onClick).semantics { contentDescription = description },
        contentAlignment = Alignment.Center,
    ) { DsIcon(icon, fg, 22.dp) }
}

@Composable
internal fun SmallAction(icon: Int, label: String, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier.clip(RoundedCornerShape(20.dp)).background(ControlBg).clickable(role = Role.Button, onClick = onClick).padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        DsIcon(icon, colors.text, 18.dp)
        Text(label, color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.Medium)
    }
}

/** A bar over the app while a minimized call is still on: tap to return. */
@Composable
fun CallBar(call: CallView, onReturn: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier.fillMaxWidth().background(colors.accent).statusBarsPadding().clickable(onClick = onReturn).padding(horizontal = 20.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        DsIcon(if (call.kind == "video") R.drawable.ui_video else R.drawable.ui_phone, colors.accentContrast, 18.dp)
        Text("${call.peerHandle} · ${statusText(call)}", color = colors.accentContrast, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f), maxLines = 1)
        Text("Return", color = colors.accentContrast, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
    }
}
