package com.redvaultsystems.redvaultmessage.ui.chats

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.util.LruCache
import com.redvaultsystems.redvaultmessage.data.FileDownloadStage
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/** Which files show as pictures in a chat, like the desktop's isInlineImage (desktop/app/src/chat_models.cpp). */
internal object PictureRules {
    /** Bigger pictures wait for a tap before they're downloaded (the desktop's limit). */
    const val AUTO_LOAD_BYTES = 20L * 1024 * 1024

    /** Past this a picture stays an ordinary file (Download, Open, Save): it's held in memory to be shown. */
    const val MAX_INLINE_BYTES = 64L * 1024 * 1024

    /** The decoder checks the bytes themselves; this only picks what to try. */
    private val TYPES = setOf("image/jpeg", "image/png", "image/gif", "image/webp", "image/bmp")

    fun isInlineImage(m: Message): Boolean {
        val file = m.file ?: return false
        if (m.kind != MessageKind.File || m.queued) return false
        if ((m.fileStatus ?: "available") != "available") return false
        if (file.originalSize > MAX_INLINE_BYTES) return false
        return file.mimeType.substringBefore(';').trim().lowercase() in TYPES
    }

    /** The power-of-two step that brings the longer side down to about [maxSide], never below it. */
    fun sampleSize(width: Int, height: Int, maxSide: Int): Int {
        var sample = 1
        if (width <= 0 || height <= 0 || maxSide <= 0) return sample
        while (maxOf(width, height) / (sample * 2) >= maxSide) sample *= 2
        return sample
    }
}

/**
 * Pictures in chats, like the desktop's ChatImageCache: downloaded and decrypted when their bubble
 * shows (or when tapped, past [PictureRules.AUTO_LOAD_BYTES]), then kept in memory only. The decrypted
 * copy on disk goes as soon as it's read; [fileFor] writes one again only for Open. [clear] when the
 * session ends.
 */
class ChatPictures(
    private val download: suspend (FileDescriptor, (FileDownloadStage) -> Unit) -> File,
    /** A decrypted copy in the app's private cache, for handing to another app (Open). */
    private val writeDecrypted: suspend (filename: String, bytes: ByteArray) -> File,
) {
    private val thumbs = object : LruCache<String, Bitmap>(THUMB_CACHE_BYTES) {
        override fun sizeOf(key: String, value: Bitmap) = value.allocationByteCount
    }
    private val originals = object : LruCache<String, ByteArray>(ORIGINAL_CACHE_BYTES) {
        override fun sizeOf(key: String, value: ByteArray) = value.size
    }

    fun cachedThumb(descriptor: FileDescriptor): Bitmap? = thumbs.get(descriptor.objectId)

    /** The picture for its bubble, about [THUMB_SIDE] px on its longer side. Throws a [UserFacingException]. */
    suspend fun thumb(descriptor: FileDescriptor): Bitmap {
        thumbs.get(descriptor.objectId)?.let { return it }
        val bytes = bytes(descriptor)
        val thumb = withContext(Dispatchers.Default) { decode(bytes, THUMB_SIDE) } ?: throw UserFacingException("Couldn't show this picture.")
        thumbs.put(descriptor.objectId, thumb)
        return thumb
    }

    /** The whole picture as sent (for the viewer and Save). Throws a [UserFacingException]. */
    suspend fun bytes(descriptor: FileDescriptor): ByteArray {
        originals.get(descriptor.objectId)?.let { return it }
        val file = download(descriptor) {}
        val bytes = try {
            withContext(Dispatchers.IO) { file.readBytes() }
        } finally {
            withContext(Dispatchers.IO) { file.parentFile?.deleteRecursively() ?: file.delete() }
        }
        originals.put(descriptor.objectId, bytes)
        return bytes
    }

    /** The picture decoded at about [maxSide] px, for the full-size viewer. */
    suspend fun full(bytes: ByteArray, maxSide: Int = FULL_SIDE): Bitmap? = withContext(Dispatchers.Default) { decode(bytes, maxSide) }

    suspend fun fileFor(descriptor: FileDescriptor, bytes: ByteArray): File = writeDecrypted(descriptor.filename, bytes)

    fun clear() {
        thumbs.evictAll()
        originals.evictAll()
    }

    private fun decode(bytes: ByteArray, maxSide: Int): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        val sample = PictureRules.sampleSize(bounds.outWidth, bounds.outHeight, maxSide)
        val decoded = runCatching {
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
        }.getOrNull() ?: return null
        val orientation = runCatching {
            ExifInterface(bytes.inputStream()).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
        }.getOrDefault(ExifInterface.ORIENTATION_NORMAL)
        val matrix = Matrix()
        when (orientation) {
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.postScale(-1f, 1f)
            ExifInterface.ORIENTATION_ROTATE_180 -> matrix.postRotate(180f)
            ExifInterface.ORIENTATION_FLIP_VERTICAL -> matrix.postScale(1f, -1f)
            ExifInterface.ORIENTATION_TRANSPOSE -> { matrix.postRotate(90f); matrix.postScale(-1f, 1f) }
            ExifInterface.ORIENTATION_ROTATE_90 -> matrix.postRotate(90f)
            ExifInterface.ORIENTATION_TRANSVERSE -> { matrix.postRotate(-90f); matrix.postScale(-1f, 1f) }
            ExifInterface.ORIENTATION_ROTATE_270 -> matrix.postRotate(270f)
        }
        val longer = maxOf(decoded.width, decoded.height)
        if (longer > maxSide) matrix.postScale(maxSide.toFloat() / longer, maxSide.toFloat() / longer)
        if (matrix.isIdentity) return decoded
        return Bitmap.createBitmap(decoded, 0, 0, decoded.width, decoded.height, matrix, true)
    }

    private companion object {
        const val THUMB_SIDE = 720
        const val FULL_SIDE = 2560
        const val THUMB_CACHE_BYTES = 32 * 1024 * 1024
        const val ORIGINAL_CACHE_BYTES = 40 * 1024 * 1024
    }
}
