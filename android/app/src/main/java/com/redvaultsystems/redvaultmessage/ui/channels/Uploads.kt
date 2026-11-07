package com.redvaultsystems.redvaultmessage.ui.channels

import android.content.Context
import android.graphics.BitmapFactory
import android.media.MediaMetadataRetriever
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.data.ChannelMediaClient
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaStartRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelUploadDto
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** One file on its way into a post or a suggestion. */
data class Upload(
    val key: Int,
    val name: String,
    val kind: String,
    val size: Long,
    val progress: Float = 0f,
    val mediaId: String? = null,
    val error: String? = null,
)

/**
 * Uploads picked or recorded files one by one as they're added, with
 * progress, like the web's useUploads; [start] asks the server for each
 * upload. A recording's file is deleted once it's sent or removed.
 */
@Stable
class UploadQueue(
    private val context: Context,
    private val scope: CoroutineScope,
    private val media: ChannelMediaClient,
    val max: Int,
    private val start: suspend (ChannelMediaStartRequest) -> ChannelUploadDto,
) {
    var uploads by mutableStateOf<List<Upload>>(emptyList())
        private set
    private val jobs = HashMap<Int, Job>()
    private var nextKey = 1

    val busy: Boolean get() = uploads.any { it.mediaId == null && it.error == null }
    val ready: List<String> get() = uploads.mapNotNull { it.mediaId }
    val full: Boolean get() = uploads.size >= max
    val isEmpty: Boolean get() = uploads.isEmpty()

    /** A picked file; [kind] null means by its type (photo, video, audio or file). */
    fun add(uri: Uri, kind: String? = null) {
        if (full) return
        val picked = media.describe(uri)
        val asKind = kind ?: ChannelPosts.mediaKindFor(picked.mime)
        val key = nextKey++
        uploads = uploads + Upload(key, picked.name, asKind, picked.size ?: 0)
        jobs[key] = scope.launch {
            var copy: File? = null
            try {
                copy = media.prepare(picked, asKind)
                val size = copy?.length() ?: picked.size ?: 0
                patch(key) { it.copy(size = size) }
                val probed = withContext(Dispatchers.IO) { probe(uri, asKind) }
                val started = start(
                    ChannelMediaStartRequest(
                        kind = asKind,
                        mime = picked.mime,
                        size = size,
                        filename = picked.name,
                        durationMs = probed.durationMs,
                        width = probed.width,
                        height = probed.height,
                    ),
                )
                media.upload(started.uploadUrl, copy, uri, size, progressTo(key))
                patch(key) { it.copy(mediaId = started.id, progress = 1f) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                patch(key) { it.copy(error = describeError(e, "The upload failed.")) }
            } finally {
                media.discard(copy)
            }
        }
    }

    /** A voice clip or round video just recorded: its file goes once it's uploaded (or removed). */
    fun addRecording(file: File, kind: String, mime: String, durationMs: Long) {
        if (full) {
            file.delete()
            return
        }
        val key = nextKey++
        val name = if (kind == "voice") "voice.m4a" else "round.mp4"
        uploads = uploads + Upload(key, name, kind, file.length())
        jobs[key] = scope.launch {
            try {
                val probed = if (kind == "round") withContext(Dispatchers.IO) { probe(Uri.fromFile(file), kind) } else Probed()
                val started = start(
                    ChannelMediaStartRequest(
                        kind = kind,
                        mime = mime,
                        size = file.length(),
                        filename = name,
                        durationMs = durationMs,
                        width = probed.width,
                        height = probed.height,
                    ),
                )
                media.upload(started.uploadUrl, file, null, file.length(), progressTo(key))
                patch(key) { it.copy(mediaId = started.id, progress = 1f) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                patch(key) { it.copy(error = describeError(e, "The upload failed.")) }
            } finally {
                file.delete()
            }
        }
    }

    fun remove(key: Int) {
        jobs.remove(key)?.cancel()
        uploads = uploads.filterNot { it.key == key }
    }

    fun clear() {
        jobs.values.forEach { it.cancel() }
        jobs.clear()
        uploads = emptyList()
    }

    /** Progress from the upload's thread, passed on in steps of a percent so the screen isn't flooded. */
    private fun progressTo(key: Int): (Float) -> Unit {
        var shown = -1f
        return { p ->
            if (p - shown >= 0.01f) {
                shown = p
                scope.launch { patch(key) { it.copy(progress = p) } }
            }
        }
    }

    private fun patch(key: Int, change: (Upload) -> Upload) {
        uploads = uploads.map { if (it.key == key) change(it) else it }
    }

    private class Probed(val width: Int? = null, val height: Int? = null, val durationMs: Long? = null)

    /** A picture's size or a recording's length, read on the phone (best effort, as the web's probeMedia). */
    private fun probe(uri: Uri, kind: String): Probed = runCatching {
        when (kind) {
            "photo" -> context.contentResolver.openInputStream(uri)?.use { input ->
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeStream(input, null, bounds)
                Probed(bounds.outWidth.takeIf { it > 0 }, bounds.outHeight.takeIf { it > 0 })
            } ?: Probed()
            "file" -> Probed()
            else -> {
                val retriever = MediaMetadataRetriever()
                try {
                    retriever.setDataSource(context, uri)
                    val duration = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull()
                    var width = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull()
                    var height = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull()
                    val rotation = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
                    if (rotation == 90 || rotation == 270) width = height.also { height = width }
                    Probed(width, height, duration)
                } finally {
                    retriever.release()
                }
            }
        }
    }.getOrDefault(Probed())
}

/** The uploads under a composer: each with its progress, Ready, or what went wrong, and Remove. */
@Composable
fun UploadList(queue: UploadQueue) {
    val colors = RedvaultTheme.colors
    if (queue.uploads.isEmpty()) return
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        queue.uploads.forEach { u ->
            Column(
                Modifier.fillMaxWidth().background(colors.surface2, RoundedCornerShape(12.dp)).padding(start = 12.dp, top = 8.dp, bottom = 8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "${ChannelPosts.kindLabel(u.kind)}: ${u.name} · ${FileMessage.formatBytes(u.size)}",
                        color = colors.text,
                        fontSize = 13.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(onClick = { queue.remove(u.key) }) { Text("Remove", color = colors.text2, fontSize = 13.sp) }
                }
                when {
                    u.error != null -> Text(u.error, color = colors.danger, fontSize = 12.sp, modifier = Modifier.padding(end = 12.dp))
                    u.mediaId != null -> Text("Ready", color = colors.accent, fontSize = 12.sp)
                    else -> LinearProgressIndicator(
                        progress = { u.progress },
                        color = colors.accent,
                        trackColor = colors.surface3,
                        modifier = Modifier.fillMaxWidth().padding(end = 12.dp, bottom = 4.dp),
                    )
                }
            }
        }
    }
}
