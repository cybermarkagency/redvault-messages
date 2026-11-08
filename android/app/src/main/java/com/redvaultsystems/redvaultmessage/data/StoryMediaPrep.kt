// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.media.MediaMetadataRetriever
import android.net.Uri
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import kotlin.math.max
import kotlin.math.min

/**
 * Getting a picked photo or video ready to post (port of prepareImage and prepareVideo in ui/src/lib/stories.ts).
 *
 * A photo is decoded, turned upright, cut down to at most 1920 pixels on its long side and saved as a fresh JPEG, so
 * whatever the original carried (location, camera, time) stays behind. A video is posted as it is, within 60 seconds
 * and 60 MB: its own container metadata (a phone's location tag, for one) is NOT removed, and the composer says so.
 */
object StoryMediaPrep {
    private val VIDEO_TYPES = Regex("^video/(mp4|webm|quicktime)$")

    /** "video/webm;codecs=vp9" is "video/webm". */
    fun normalizeMime(type: String?): String = type?.substringBefore(';')?.trim()?.lowercase().orEmpty()

    suspend fun prepareImage(context: Context, uri: Uri): PreparedStoryMedia = withContext(Dispatchers.IO) {
        val resolver = context.contentResolver
        if (!normalizeMime(resolver.getType(uri)).startsWith("image/")) throw StoryInputException("That isn't a photo.")
        try {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) throw StoryInputException("That photo couldn't be opened.")
            var sample = 1
            while (max(bounds.outWidth, bounds.outHeight) / (sample * 2) >= StoryLimits.MAX_IMAGE_SIDE) sample *= 2
            val decoded = resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample }) }
                ?: throw StoryInputException("That photo couldn't be opened.")
            val orientation = resolver.openInputStream(uri)?.use { runCatching { ExifInterface(it).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL) }.getOrNull() }
                ?: ExifInterface.ORIENTATION_NORMAL
            val matrix = Matrix().apply { applyOrientation(orientation) }
            val turned = orientation != ExifInterface.ORIENTATION_NORMAL && orientation != ExifInterface.ORIENTATION_UNDEFINED
            val longSide = max(decoded.width, decoded.height).toFloat()
            val scale = min(1f, StoryLimits.MAX_IMAGE_SIDE / longSide)
            if (scale < 1f) matrix.postScale(scale, scale)
            val ready = if (turned || scale < 1f) Bitmap.createBitmap(decoded, 0, 0, decoded.width, decoded.height, matrix, true) else decoded
            val out = ByteArrayOutputStream()
            // Bitmap.compress writes no metadata at all: the JPEG is made from the pixels alone.
            if (!ready.compress(Bitmap.CompressFormat.JPEG, 90, out)) throw StoryInputException("That photo couldn't be prepared.")
            val width = ready.width
            val height = ready.height
            if (ready !== decoded) decoded.recycle()
            ready.recycle()
            val bytes = out.toByteArray()
            if (bytes.size > StoriesRepository.MAX_PHOTO_CIPHERTEXT_BYTES - 1024 * 1024) throw StoryInputException("That photo is too large for a story.")
            PreparedStoryMedia(StoryKind.Image, "image/jpeg", width, height, null, bytes.size.toLong()) { ByteArrayInputStream(bytes) }
        } catch (e: IOException) {
            throw StoryInputException("That photo couldn't be opened.")
        } catch (e: OutOfMemoryError) {
            throw StoryInputException("That photo is too large to open on this phone.")
        }
    }

    suspend fun prepareVideo(context: Context, uri: Uri): PreparedStoryMedia = withContext(Dispatchers.IO) {
        val resolver = context.contentResolver
        val mime = normalizeMime(resolver.getType(uri))
        if (!VIDEO_TYPES.matches(mime)) throw StoryInputException("Use an MP4, WebM or MOV video.")
        val size = resolver.openAssetFileDescriptor(uri, "r")?.use { it.length }?.takeIf { it >= 0 }
            ?: resolver.openInputStream(uri)?.use { stream ->
                var total = 0L
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val n = stream.read(buffer)
                    if (n < 0) break
                    total += n
                    if (total > StoryLimits.MAX_VIDEO_BYTES) break
                }
                total
            }
            ?: throw StoryInputException("That video couldn't be opened.")
        if (size > StoryLimits.MAX_VIDEO_BYTES) throw StoryInputException("That video is too large for a story (at most 60 MB).")
        val retriever = MediaMetadataRetriever()
        try {
            retriever.setDataSource(context, uri)
            val durationMs = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull()
            if (durationMs == null || durationMs <= 0) throw StoryInputException("That video couldn't be opened.")
            if (durationMs > StoryLimits.MAX_VIDEO_MS + 500) throw StoryInputException("Stories can be up to 60 seconds long. Trim the video first.")
            var width = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
            var height = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
            val rotation = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
            if (rotation == 90 || rotation == 270) width = height.also { height = width }
            PreparedStoryMedia(
                StoryKind.Video, mime, width.takeIf { it > 0 } ?: 1080, height.takeIf { it > 0 } ?: 1920,
                min(durationMs, StoryLimits.MAX_VIDEO_MS), size,
            ) { resolver.openInputStream(uri) ?: throw StoryInputException("That video couldn't be read.") }
        } catch (e: RuntimeException) {
            // MediaMetadataRetriever reports a file it can't read as a RuntimeException.
            throw StoryInputException("That video couldn't be opened.")
        } finally {
            retriever.release()
        }
    }

    /** The turn and mirror an EXIF orientation asks for (the same eight cases every image library handles). */
    private fun Matrix.applyOrientation(orientation: Int) {
        when (orientation) {
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> postScale(-1f, 1f)
            ExifInterface.ORIENTATION_ROTATE_180 -> postRotate(180f)
            ExifInterface.ORIENTATION_FLIP_VERTICAL -> postScale(1f, -1f)
            ExifInterface.ORIENTATION_TRANSPOSE -> { postRotate(90f); postScale(-1f, 1f) }
            ExifInterface.ORIENTATION_ROTATE_90 -> postRotate(90f)
            ExifInterface.ORIENTATION_TRANSVERSE -> { postRotate(-90f); postScale(-1f, 1f) }
            ExifInterface.ORIENTATION_ROTATE_270 -> postRotate(270f)
        }
    }
}
