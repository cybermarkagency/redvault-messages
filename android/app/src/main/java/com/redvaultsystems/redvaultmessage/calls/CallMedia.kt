package com.redvaultsystems.redvaultmessage.calls

import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjection
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraVideoCapturer
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.MediaConstraints
import org.webrtc.PeerConnectionFactory
import org.webrtc.ScreenCapturerAndroid
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSink
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import org.webrtc.audio.JavaAudioDeviceModule

/**
 * The phone's side of a call's media: one WebRTC factory for the session,
 * the microphone, the camera (front by default) and screen capture. Tracks
 * live only while a call does; [stop] releases the camera and microphone.
 */
class CallMedia(
    private val context: Context,
    /** Settings › Echo cancellation. Read when a call starts. */
    private val echoCancellation: () -> Boolean,
) {
    val eglBase: EglBase = EglBase.create()

    private var built: PeerConnectionFactory? = null
    private var builtWithEcho = true
    private var initialized = false

    /** The WebRTC factory, built with the echo-cancellation setting in force when it was made. */
    val factory: PeerConnectionFactory
        get() = built ?: build(echoCancellation())

    private fun build(echo: Boolean): PeerConnectionFactory {
        if (!initialized) {
            PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(context.applicationContext).createInitializationOptions(),
            )
            initialized = true
        }
        // The phone's own echo canceller is chosen when the audio module is made, so a changed
        // setting rebuilds the factory before the next call ([start]).
        val audioModule = JavaAudioDeviceModule.builder(context.applicationContext)
            .setUseHardwareAcousticEchoCanceler(echo)
            .setUseHardwareNoiseSuppressor(true)
            .createAudioDeviceModule()
        val factory = PeerConnectionFactory.builder()
            .setAudioDeviceModule(audioModule)
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(eglBase.eglBaseContext, true, true))
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(eglBase.eglBaseContext))
            .createPeerConnectionFactory()
        audioModule.release() // the factory holds its own reference
        built = factory
        builtWithEcho = echo
        return factory
    }

    private var audioSource: AudioSource? = null
    var audioTrack: AudioTrack? = null
        private set

    private var camera: CameraVideoCapturer? = null
    private var cameraHelper: SurfaceTextureHelper? = null
    private var cameraSource: VideoSource? = null
    var videoTrack: VideoTrack? = null
        private set
    private val localSinks = mutableSetOf<VideoSink>()

    private var screen: ScreenCapturerAndroid? = null
    private var screenHelper: SurfaceTextureHelper? = null
    private var screenSource: VideoSource? = null
    private var screenTrack: VideoTrack? = null

    /** Microphone, and the camera when [video]. Returns whether the camera came on (no camera: voice only). */
    fun start(video: Boolean): Boolean {
        val echo = echoCancellation()
        if (audioTrack == null && built != null && builtWithEcho != echo) {
            // Between calls only: nothing is using the old factory now.
            built?.dispose()
            built = null
        }
        if (audioTrack == null) {
            // WebRTC's own echo canceller follows the setting too, per call.
            val constraints = MediaConstraints().apply {
                mandatory.add(MediaConstraints.KeyValuePair("googEchoCancellation", echo.toString()))
                mandatory.add(MediaConstraints.KeyValuePair("googNoiseSuppression", "true"))
                mandatory.add(MediaConstraints.KeyValuePair("googAutoGainControl", "true"))
            }
            val source = factory.createAudioSource(constraints)
            audioSource = source
            audioTrack = factory.createAudioTrack("mic", source).apply { setEnabled(true) }
        }
        if (!video) return false
        return runCatching { startCamera() }.isSuccess
    }

    /** Starts the camera (front if there is one) and returns its track. */
    fun startCamera(): VideoTrack {
        videoTrack?.let { track ->
            track.setEnabled(true)
            return track
        }
        val enumerator = Camera2Enumerator(context)
        val names = enumerator.deviceNames
        val name = names.firstOrNull { enumerator.isFrontFacing(it) } ?: names.firstOrNull() ?: throw IllegalStateException("No camera")
        val capturer = enumerator.createCapturer(name, null) ?: throw IllegalStateException("No camera")
        val helper = SurfaceTextureHelper.create("camera", eglBase.eglBaseContext)
        val source = factory.createVideoSource(false)
        capturer.initialize(helper, context, source.capturerObserver)
        capturer.startCapture(1280, 720, 30)
        val track = factory.createVideoTrack("camera", source)
        localSinks.forEach { track.addSink(it) }
        camera = capturer
        cameraHelper = helper
        cameraSource = source
        videoTrack = track
        return track
    }

    /** Turns the camera off completely (its light goes off too). */
    fun stopCamera() {
        videoTrack?.let { track -> localSinks.forEach { track.removeSink(it) } }
        runCatching { camera?.stopCapture() }
        camera?.dispose()
        videoTrack?.dispose()
        cameraSource?.dispose()
        cameraHelper?.dispose()
        camera = null
        videoTrack = null
        cameraSource = null
        cameraHelper = null
    }

    fun switchCamera() {
        camera?.switchCamera(null)
    }

    /** Screen capture with the system's permission [projectionData]; [onStopped] runs if the system ends it. */
    fun startScreen(projectionData: Intent, onStopped: () -> Unit): VideoTrack {
        stopScreen()
        val capturer = ScreenCapturerAndroid(projectionData, object : MediaProjection.Callback() {
            override fun onStop() = onStopped()
        })
        val helper = SurfaceTextureHelper.create("screen", eglBase.eglBaseContext)
        val source = factory.createVideoSource(true)
        capturer.initialize(helper, context, source.capturerObserver)
        val metrics = context.resources.displayMetrics
        // Half resolution keeps text readable at a bitrate a relay handles well.
        capturer.startCapture(metrics.widthPixels / 2, metrics.heightPixels / 2, 15)
        val track = factory.createVideoTrack("screen", source)
        screen = capturer
        screenHelper = helper
        screenSource = source
        screenTrack = track
        return track
    }

    fun stopScreen() {
        runCatching { screen?.stopCapture() }
        screen?.dispose()
        screenTrack?.dispose()
        screenSource?.dispose()
        screenHelper?.dispose()
        screen = null
        screenTrack = null
        screenSource = null
        screenHelper = null
    }

    fun addLocalSink(sink: VideoSink) {
        localSinks += sink
        videoTrack?.addSink(sink)
    }

    fun removeLocalSink(sink: VideoSink) {
        localSinks -= sink
        videoTrack?.removeSink(sink)
    }

    /** Ends the call's media: camera, screen and microphone. */
    fun stop() {
        stopScreen()
        stopCamera()
        audioTrack?.dispose()
        audioSource?.dispose()
        audioTrack = null
        audioSource = null
    }

    fun release() {
        stop()
        built?.dispose()
        built = null
        eglBase.release()
    }
}
