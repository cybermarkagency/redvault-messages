package com.redvaultsystems.redvaultmessage.data

import android.content.ContentResolver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.net.Uri
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import java.io.ByteArrayOutputStream

/**
 * Like readImageAsAvatar on the web: center-crop to a square, scale to
 * 128 px, JPEG at 85%. Re-encoding also drops the photo's EXIF data
 * (location, camera, time), so none of it is uploaded. Profile photos and
 * channel photos both go through here.
 */
object AvatarJpeg {
    private const val AVATAR_SIZE = 128
    private const val MAX_SOURCE_BYTES = 10 * 1024 * 1024

    /** Throws [UserFacingException] when [uri] can't be read or isn't an image. */
    fun base64(contentResolver: ContentResolver, uri: Uri): String {
        val bytes = contentResolver.openInputStream(uri)?.use { input ->
            val buffer = ByteArrayOutputStream()
            val chunk = ByteArray(64 * 1024)
            while (true) {
                val read = input.read(chunk)
                if (read < 0) break
                buffer.write(chunk, 0, read)
                if (buffer.size() > MAX_SOURCE_BYTES) throw UserFacingException("That image is too large (max 10 MB).")
            }
            buffer.toByteArray()
        } ?: throw UserFacingException("Couldn't read that image.")

        // Decode at a reduced size: a phone photo can be 50+ megapixels.
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) throw UserFacingException("That's not an image file.")
        var sampleSize = 1
        while (minOf(bounds.outWidth, bounds.outHeight) / (sampleSize * 2) >= AVATAR_SIZE) sampleSize *= 2
        val decoded = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sampleSize })
            ?: throw UserFacingException("Couldn't load that as an image.")

        val upright = rotateForExif(decoded, bytes)
        val side = minOf(upright.width, upright.height)
        val square = Bitmap.createBitmap(upright, (upright.width - side) / 2, (upright.height - side) / 2, side, side)
        val scaled = Bitmap.createScaledBitmap(square, AVATAR_SIZE, AVATAR_SIZE, true)
        val out = ByteArrayOutputStream()
        scaled.compress(Bitmap.CompressFormat.JPEG, 85, out)
        return Base64Codec.encode(out.toByteArray())
    }

    private fun rotateForExif(bitmap: Bitmap, bytes: ByteArray): Bitmap {
        val orientation = runCatching {
            ExifInterface(bytes.inputStream()).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
        }.getOrDefault(ExifInterface.ORIENTATION_NORMAL)
        val degrees = when (orientation) {
            ExifInterface.ORIENTATION_ROTATE_90 -> 90f
            ExifInterface.ORIENTATION_ROTATE_180 -> 180f
            ExifInterface.ORIENTATION_ROTATE_270 -> 270f
            else -> return bitmap
        }
        return Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, Matrix().apply { postRotate(degrees) }, true)
    }
}
