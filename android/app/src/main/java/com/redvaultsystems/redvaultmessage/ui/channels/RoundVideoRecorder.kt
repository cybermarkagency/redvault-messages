// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.channels

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Size
import android.view.Surface
import android.view.TextureView
import android.view.WindowManager
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import java.io.File
import java.util.UUID

private const val ROUND_MAX_MS = 60_000

/**
 * Records a round video message with the front camera (up to a minute):
 * H.264 and AAC in an MP4 in the app's private cache, never anywhere else;
 * nothing but the picture and sound goes in (no location). The preview is
 * mirrored like a mirror; the recording isn't, as others see you.
 * Needs the camera and microphone permissions already granted.
 */
@Composable
fun RoundVideoRecorder(onCancel: () -> Unit, onDone: (file: File, durationMs: Long) -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val camera = remember { RoundCamera(context) }
    var phase by remember { mutableStateOf(camera.phase) }
    var elapsed by remember { mutableLongStateOf(0L) }
    camera.onChange = { phase = camera.phase }
    DisposableEffect(camera) { onDispose { camera.close() } }
    LaunchedEffect(phase) {
        while (phase == RoundCamera.Phase.Recording) {
            elapsed = camera.elapsedMs()
            if (elapsed >= ROUND_MAX_MS) {
                camera.stop()?.let { (file, ms) -> onDone(file, ms) }
                break
            }
            delay(200)
        }
    }
    Dialog(onDismissRequest = { camera.close(); onCancel() }, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(
            Modifier.fillMaxSize().background(Color.Black).padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(24.dp, Alignment.CenterVertically),
        ) {
            Text("Round video", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
            Box(Modifier.size(280.dp).clip(CircleShape).background(colors.surface2), contentAlignment = Alignment.Center) {
                AndroidView(
                    factory = { ctx -> TextureView(ctx).also(camera::attach) },
                    modifier = Modifier.fillMaxSize(),
                )
                if (phase == RoundCamera.Phase.Failed) {
                    Text(camera.problem ?: "Couldn't use the camera.", color = Color.White, fontSize = 14.sp, modifier = Modifier.padding(24.dp))
                }
            }
            Text(
                when (phase) {
                    RoundCamera.Phase.Recording -> "● ${ChannelPosts.formatDuration(elapsed / 1000.0)} / ${ChannelPosts.formatDuration(ROUND_MAX_MS / 1000.0)}"
                    RoundCamera.Phase.Ready -> "Up to a minute. Tap Record when you're ready."
                    RoundCamera.Phase.Failed -> ""
                    else -> "Starting the camera…"
                },
                color = if (phase == RoundCamera.Phase.Recording) colors.danger else Color.White,
                fontSize = 15.sp,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                PillButton("Cancel", {
                    camera.close()
                    onCancel()
                }, style = PillStyle.Secondary, height = 48.dp)
                when (phase) {
                    RoundCamera.Phase.Recording -> PillButton("Stop and add", {
                        camera.stop()?.let { (file, ms) -> onDone(file, ms) } ?: onCancel()
                    }, height = 48.dp)
                    else -> PillButton("Record", { camera.record() }, enabled = phase == RoundCamera.Phase.Ready, height = 48.dp)
                }
            }
        }
    }
}

/** The front camera into a preview and a MediaRecorder, with Camera2. All of it on one background thread. */
private class RoundCamera(private val context: Context) {
    enum class Phase { Opening, Ready, Recording, Failed }

    var phase = Phase.Opening
        private set
    var problem: String? = null
        private set
    var onChange: () -> Unit = {}

    private val thread = HandlerThread("round-camera").apply { start() }
    private val handler = Handler(thread.looper)
    private val main = Handler(context.mainLooper)
    private val manager = context.getSystemService(CameraManager::class.java)
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var recorder: MediaRecorder? = null
    private var previewSurface: Surface? = null
    private var output: File? = null
    private var startedAt = 0L
    private var cameraId: String? = null
    private var sensorOrientation = 270
    private var size = Size(640, 480)
    private var view: TextureView? = null
    private var closed = false

    fun elapsedMs() = if (phase == Phase.Recording) SystemClock.elapsedRealtime() - startedAt else 0L

    fun attach(texture: TextureView) {
        view = texture
        texture.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) = open(st)
            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) = fitPreview()
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                close()
                return true
            }
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }
        texture.surfaceTexture?.let(::open)
    }

    @SuppressLint("MissingPermission")
    private fun open(texture: SurfaceTexture) {
        if (closed || device != null) return
        try {
            val id = manager.cameraIdList.firstOrNull { manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT }
                ?: manager.cameraIdList.firstOrNull()
                ?: return fail("This phone has no camera.")
            val characteristics = manager.getCameraCharacteristics(id)
            sensorOrientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 270
            val sizes = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)?.getOutputSizes(MediaRecorder::class.java).orEmpty()
            // Small and square-ish is all a round video needs: the smallest size at least 480 tall, 4:3 or squarer.
            size = sizes.filter { minOf(it.width, it.height) >= 480 && maxOf(it.width, it.height) * 3 <= minOf(it.width, it.height) * 4 }
                .minByOrNull { it.width * it.height }
                ?: sizes.filter { minOf(it.width, it.height) >= 480 }.minByOrNull { it.width * it.height }
                ?: sizes.maxByOrNull { it.width * it.height }
                ?: Size(640, 480)
            texture.setDefaultBufferSize(size.width, size.height)
            previewSurface = Surface(texture)
            cameraId = id
            main.post(::fitPreview)
            manager.openCamera(id, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    if (closed) {
                        camera.close()
                        return
                    }
                    device = camera
                    startPreview()
                }
                override fun onDisconnected(camera: CameraDevice) {
                    camera.close()
                    device = null
                }
                override fun onError(camera: CameraDevice, error: Int) {
                    camera.close()
                    device = null
                    fail("Couldn't use the camera.")
                }
            }, handler)
        } catch (e: SecurityException) {
            fail("Allow camera access to record a round video.")
        } catch (e: Exception) {
            fail("Couldn't use the camera.")
        }
    }

    private fun startPreview() {
        val camera = device ?: return
        val preview = previewSurface ?: return
        session(camera, listOf(preview)) { s ->
            val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply { addTarget(preview) }.build()
            s.setRepeatingRequest(request, null, handler)
            set(Phase.Ready)
        }
    }

    fun record() {
        if (phase != Phase.Ready) return
        handler.post {
            val camera = device ?: return@post
            val preview = previewSurface ?: return@post
            try {
                session?.close()
                val dir = File(context.cacheDir, "outgoing").apply { mkdirs() }
                val target = File(dir, "round-${UUID.randomUUID()}.mp4")
                @Suppress("DEPRECATION")
                val r = if (Build.VERSION.SDK_INT >= 31) MediaRecorder(context) else MediaRecorder()
                r.setAudioSource(MediaRecorder.AudioSource.MIC)
                r.setVideoSource(MediaRecorder.VideoSource.SURFACE)
                r.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
                r.setVideoEncoder(MediaRecorder.VideoEncoder.H264)
                r.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
                r.setVideoSize(size.width, size.height)
                r.setVideoFrameRate(30)
                r.setVideoEncodingBitRate(1_500_000)
                r.setAudioEncodingBitRate(64_000)
                r.setAudioSamplingRate(44_100)
                r.setAudioChannels(1)
                r.setOrientationHint(recordingRotation())
                r.setMaxDuration(ROUND_MAX_MS + 1000)
                r.setOutputFile(target.absolutePath)
                r.prepare()
                recorder = r
                output = target
                session(camera, listOf(preview, r.surface)) { s ->
                    val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                        addTarget(preview)
                        addTarget(r.surface)
                    }.build()
                    s.setRepeatingRequest(request, null, handler)
                    r.start()
                    startedAt = SystemClock.elapsedRealtime()
                    set(Phase.Recording)
                }
            } catch (e: Exception) {
                recorder?.release()
                recorder = null
                output?.delete()
                output = null
                fail("Couldn't start recording.")
            }
        }
    }

    /** Stops and returns the recording and its length, or null if there's nothing usable. Blocks briefly. */
    fun stop(): Pair<File, Long>? {
        if (phase != Phase.Recording) return null
        val length = elapsedMs()
        val r = recorder ?: return null
        val file = output
        recorder = null
        output = null
        val ok = runCatching {
            session?.stopRepeating()
            r.stop()
        }.isSuccess
        r.release()
        close()
        if (!ok || file == null || length < 700) {
            file?.delete()
            return null
        }
        return file to length
    }

    fun close() {
        if (closed) return
        closed = true
        runCatching { recorder?.release() }
        recorder = null
        output?.delete()
        output = null
        runCatching { session?.close() }
        session = null
        runCatching { device?.close() }
        device = null
        previewSurface?.release()
        previewSurface = null
        thread.quitSafely()
    }

    private fun session(camera: CameraDevice, targets: List<Surface>, onReady: (CameraCaptureSession) -> Unit) {
        @Suppress("DEPRECATION")
        camera.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                if (closed) {
                    s.close()
                    return
                }
                session = s
                try {
                    onReady(s)
                } catch (e: Exception) {
                    fail("Couldn't use the camera.")
                }
            }
            override fun onConfigureFailed(s: CameraCaptureSession) = fail("Couldn't use the camera.")
        }, handler)
    }

    /** How the recorded picture is turned, for the screen's current rotation (Camera2's documented formula). */
    private fun recordingRotation(): Int {
        val degrees = when (displayRotation()) {
            Surface.ROTATION_90 -> 90
            Surface.ROTATION_180 -> 180
            Surface.ROTATION_270 -> 270
            else -> 0
        }
        val front = cameraId?.let { manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) } == CameraCharacteristics.LENS_FACING_FRONT
        return if (front) (sensorOrientation + degrees) % 360 else (sensorOrientation - degrees + 360) % 360
    }

    @Suppress("DEPRECATION")
    private fun displayRotation(): Int = runCatching {
        if (Build.VERSION.SDK_INT >= 30) context.display?.rotation ?: Surface.ROTATION_0
        else context.getSystemService(WindowManager::class.java).defaultDisplay.rotation
    }.getOrDefault(Surface.ROTATION_0)

    /** The preview fills its circle without stretching (cropped), and is mirrored for the front camera. */
    private fun fitPreview() {
        val v = view ?: return
        if (v.width == 0 || v.height == 0) return
        val rotation = displayRotation()
        // The camera's buffer is landscape; upright on a portrait screen it's the other way round.
        val sideways = rotation == Surface.ROTATION_0 || rotation == Surface.ROTATION_180
        val contentW = if (sideways) size.height.toFloat() else size.width.toFloat()
        val contentH = if (sideways) size.width.toFloat() else size.height.toFloat()
        val viewW = v.width.toFloat()
        val viewH = v.height.toFloat()
        val cover = maxOf(viewW / contentW, viewH / contentH)
        val matrix = Matrix()
        matrix.setScale(contentW * cover / viewW, contentH * cover / viewH, viewW / 2, viewH / 2)
        matrix.postScale(-1f, 1f, viewW / 2, viewH / 2)
        v.setTransform(matrix)
    }

    private fun fail(message: String) {
        problem = message
        set(Phase.Failed)
    }

    private fun set(next: Phase) {
        main.post {
            phase = next
            onChange()
        }
    }
}
