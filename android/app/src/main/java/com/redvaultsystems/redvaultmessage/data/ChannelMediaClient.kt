package com.redvaultsystems.redvaultmessage.data

import android.content.ContentResolver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.media.MediaDataSource
import android.net.Uri
import android.provider.OpenableColumns
import android.util.LruCache
import com.redvaultsystems.redvaultmessage.data.api.ACCESS_HEADER
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okio.BufferedSink
import okio.source
import java.io.File
import java.io.IOException
import java.util.UUID
import java.util.concurrent.TimeUnit
import kotlin.coroutines.coroutineContext

/**
 * The bytes of channel media: uploads (signed in, streamed from the phone
 * with progress), and photos, files, videos and recordings to show or play.
 * Channels are plain text by design, so media is stored as uploaded; reading
 * it goes out on the cookie-less public client (signed, expiring same-server
 * links), so the server can't tell which account looks at what, and video
 * and audio play through our own client by byte ranges ([RangeSource]), so
 * no other HTTP stack (with its own User-Agent) ever fetches them.
 *
 * Only same-server "/api/…" paths are ever fetched. Everything downloaded
 * lives in the app's private cache and goes when the session ends.
 */
class ChannelMediaClient(
    private val publicClient: OkHttpClient,
    signedInClient: OkHttpClient,
    private val apiBase: String,
    private val contentResolver: ContentResolver,
    cacheDir: File,
) {
    // Uploads can be gigabytes: the server answers only once the last byte is stored.
    private val uploadClient = signedInClient.newBuilder().writeTimeout(2, TimeUnit.MINUTES).readTimeout(2, TimeUnit.MINUTES).build()
    private val root = File(cacheDir, "channel-media")
    private val outgoing = File(root, "outgoing")
    private val downloads = File(root, "files")
    private val photos = object : LruCache<String, Bitmap>(PHOTO_CACHE_BYTES) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val avatars = LruCache<String, Bitmap>(48)

    /** A same-server API path as a full address on our own server, or null for anything else. */
    fun resolve(path: String): String? = if (path.startsWith("/api/")) apiBase + path.removePrefix("/api/") else null

    // --- Uploads ---

    /** What a picked file will be sent as: its name, exact size and type, read from the phone. */
    class Picked(val uri: Uri, val name: String, val size: Long?, val mime: String)

    fun describe(uri: Uri): Picked {
        var name: String? = null
        var size: Long? = null
        runCatching {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    name = c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 }?.let(c::getString)
                    size = c.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 && !c.isNull(it) }?.let(c::getLong)
                }
            }
        }
        val mime = (contentResolver.getType(uri) ?: "application/octet-stream").lowercase()
        return Picked(uri, name?.takeIf { it.isNotBlank() } ?: "file", size?.takeIf { it > 0 }, mime)
    }

    /**
     * A local copy to upload when the file can't go straight from the
     * picker: photos (JPEG, PNG and WebP) lose their location tags first,
     * keeping their full resolution, and anything whose size the phone won't
     * say is measured by copying. Null when the picked file can go as it is.
     */
    suspend fun prepare(picked: Picked, kind: String): File? = withContext(Dispatchers.IO) {
        val photo = kind == "photo" && picked.mime in EXIF_WRITABLE
        if (!photo && picked.size != null) return@withContext null
        outgoing.mkdirs()
        val copy = File(outgoing, UUID.randomUUID().toString())
        try {
            contentResolver.openInputStream(picked.uri)?.use { input -> copy.outputStream().use { input.copyTo(it, 256 * 1024) } }
                ?: throw UserFacingException("Couldn't read that file.")
            if (photo) stripLocation(copy)
            copy
        } catch (e: Exception) {
            copy.delete()
            if (e is UserFacingException) throw e
            throw UserFacingException("Couldn't read that file.")
        }
    }

    /** Sends an upload's bytes (from [file], or else straight from the picked [uri]) to [uploadUrl], reporting 0..1. */
    suspend fun upload(uploadUrl: String, file: File?, uri: Uri?, size: Long, onProgress: (Float) -> Unit) = withContext(Dispatchers.IO) {
        val url = resolve(uploadUrl) ?: throw UserFacingException("The server sent an upload address this app won't use.")
        val body = object : RequestBody() {
            override fun contentType() = OCTET_STREAM
            override fun contentLength() = size
            override fun writeTo(sink: BufferedSink) {
                val input = file?.inputStream() ?: uri?.let(contentResolver::openInputStream) ?: throw IOException("The file can't be read")
                input.source().use { sink.writeAll(it) }
            }
        }
        val counted = CountingRequestBody(body) { written -> onProgress(if (size > 0) (written.toFloat() / size).coerceIn(0f, 1f) else 0f) }
        try {
            uploadClient.newCall(Request.Builder().url(url).put(counted).build()).execute().use { response ->
                if (response.isSuccessful) return@use
                val error = runCatching { ApiClient.json.parseToJsonElement(response.body?.string().orEmpty()).jsonObject }.getOrNull()
                val code = runCatching { error?.get("error")?.jsonPrimitive?.content }.getOrNull()
                val message = runCatching { error?.get("message")?.jsonPrimitive?.content }.getOrNull()
                throw ApiException(
                    response.code,
                    code ?: "upload_failed",
                    if (response.code == 413) "That file is too big for this server." else message ?: "The upload failed.",
                )
            }
        } catch (e: IOException) {
            throw UserFacingException("The upload was interrupted. Check your connection and try again.")
        }
    }

    fun discard(file: File?) {
        file?.delete()
    }

    // --- Reading ---

    /** A channel's photo, fetched without the cookie (with the private link's token when it's private). */
    suspend fun avatar(avatarUrl: String, access: String?): Bitmap? {
        val key = "$avatarUrl|${access.orEmpty()}"
        avatars.get(key)?.let { return it }
        val url = resolve(avatarUrl) ?: return null
        return withContext(Dispatchers.IO) {
            runCatching {
                val request = Request.Builder().url(url).apply { if (access != null) header(ACCESS_HEADER, access) }.build()
                publicClient.newCall(request).execute().use { response ->
                    if (!response.isSuccessful) return@use null
                    val bytes = response.body?.bytes() ?: return@use null
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                }
            }.getOrNull()?.also { avatars.put(key, it) }
        }
    }

    /**
     * A media item downloaded once into the private cache (by its id, so a
     * fresh signed link reuses it). Throws [UserFacingException] on failure.
     */
    suspend fun download(mediaId: String, url: String, onProgress: (Float) -> Unit = {}): File = withContext(Dispatchers.IO) {
        val target = File(downloads, safeName(mediaId))
        if (target.exists() && target.length() > 0) return@withContext target
        val address = resolve(url) ?: throw UserFacingException("This file's link isn't one this app opens.")
        downloads.mkdirs()
        val partial = File(downloads, "${safeName(mediaId)}.part")
        try {
            publicClient.newBuilder().readTimeout(60, TimeUnit.SECONDS).build()
                .newCall(Request.Builder().url(address).build()).execute().use { response ->
                    if (!response.isSuccessful) {
                        throw UserFacingException(if (response.code == 403 || response.code == 410) "This link expired. Reopen the channel." else "Couldn't download it. Try again.")
                    }
                    val body = response.body ?: throw UserFacingException("Couldn't download it. Try again.")
                    val total = body.contentLength()
                    body.byteStream().use { input ->
                        partial.outputStream().use { out ->
                            val buffer = ByteArray(256 * 1024)
                            var done = 0L
                            while (true) {
                                coroutineContext.ensureActive()
                                val read = input.read(buffer)
                                if (read < 0) break
                                out.write(buffer, 0, read)
                                done += read
                                if (total > 0) onProgress((done.toFloat() / total).coerceIn(0f, 1f))
                            }
                        }
                    }
                }
            if (!partial.renameTo(target)) throw UserFacingException("Couldn't save it. Try again.")
            target
        } catch (e: IOException) {
            throw UserFacingException("Couldn't download it. Check your connection and try again.")
        } finally {
            partial.delete()
        }
    }

    /** A photo to show at about [maxSide] pixels on its longer side, upright; cached in memory. */
    suspend fun photo(mediaId: String, url: String, maxSide: Int): Bitmap? {
        val key = "$mediaId@$maxSide"
        photos.get(key)?.let { return it }
        val file = runCatching { download(mediaId, url) }.getOrNull() ?: return null
        return withContext(Dispatchers.Default) { decodeSampled(file, maxSide) }?.also { photos.put(key, it) }
    }

    /** Reads [url] by byte ranges as it's played, for MediaPlayer: nothing but our own client touches the network. */
    fun rangeSource(url: String, size: Long): MediaDataSource? {
        val address = resolve(url) ?: return null
        return RangeSource(publicClient, address, size)
    }

    fun clear() {
        photos.evictAll()
        avatars.evictAll()
        root.deleteRecursively()
    }

    private fun decodeSampled(file: File, maxSide: Int): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.absolutePath, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= maxSide) sample *= 2
        val decoded = BitmapFactory.decodeFile(file.absolutePath, BitmapFactory.Options().apply { inSampleSize = sample }) ?: return null
        val orientation = runCatching {
            ExifInterface(file.absolutePath).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
        }.getOrDefault(ExifInterface.ORIENTATION_NORMAL)
        val degrees = when (orientation) {
            ExifInterface.ORIENTATION_ROTATE_90 -> 90f
            ExifInterface.ORIENTATION_ROTATE_180 -> 180f
            ExifInterface.ORIENTATION_ROTATE_270 -> 270f
            else -> return decoded
        }
        return Bitmap.createBitmap(decoded, 0, 0, decoded.width, decoded.height, Matrix().apply { postRotate(degrees) }, true)
    }

    private fun stripLocation(file: File) {
        runCatching {
            val exif = ExifInterface(file.absolutePath)
            var changed = false
            for (tag in LOCATION_TAGS) {
                if (exif.getAttribute(tag) != null) {
                    exif.setAttribute(tag, null)
                    changed = true
                }
            }
            if (changed) exif.saveAttributes()
        }
    }

    private fun safeName(id: String) = id.filter { it.isLetterOrDigit() || it == '-' }.ifEmpty { "media" }

    companion object {
        private val OCTET_STREAM = "application/octet-stream".toMediaType()
        /** Decoded photos kept in memory, in bytes. */
        private const val PHOTO_CACHE_BYTES = 48 * 1024 * 1024
        /** The types android.media.ExifInterface can rewrite in place. */
        private val EXIF_WRITABLE = setOf("image/jpeg", "image/png", "image/webp")
        private val LOCATION_TAGS = listOf(
            ExifInterface.TAG_GPS_LATITUDE, ExifInterface.TAG_GPS_LATITUDE_REF, ExifInterface.TAG_GPS_LONGITUDE,
            ExifInterface.TAG_GPS_LONGITUDE_REF, ExifInterface.TAG_GPS_ALTITUDE, ExifInterface.TAG_GPS_ALTITUDE_REF,
            ExifInterface.TAG_GPS_TIMESTAMP, ExifInterface.TAG_GPS_DATESTAMP, ExifInterface.TAG_GPS_PROCESSING_METHOD,
            ExifInterface.TAG_GPS_AREA_INFORMATION, ExifInterface.TAG_GPS_SPEED, ExifInterface.TAG_GPS_SPEED_REF,
            ExifInterface.TAG_GPS_TRACK, ExifInterface.TAG_GPS_TRACK_REF, ExifInterface.TAG_GPS_IMG_DIRECTION,
            ExifInterface.TAG_GPS_IMG_DIRECTION_REF, ExifInterface.TAG_GPS_DEST_LATITUDE, ExifInterface.TAG_GPS_DEST_LATITUDE_REF,
            ExifInterface.TAG_GPS_DEST_LONGITUDE, ExifInterface.TAG_GPS_DEST_LONGITUDE_REF, ExifInterface.TAG_GPS_DEST_BEARING,
            ExifInterface.TAG_GPS_DEST_BEARING_REF, ExifInterface.TAG_GPS_DEST_DISTANCE, ExifInterface.TAG_GPS_DEST_DISTANCE_REF,
            ExifInterface.TAG_GPS_MAP_DATUM, ExifInterface.TAG_GPS_SATELLITES, ExifInterface.TAG_GPS_STATUS,
            ExifInterface.TAG_GPS_MEASURE_MODE, ExifInterface.TAG_GPS_DOP, ExifInterface.TAG_GPS_DIFFERENTIAL,
            ExifInterface.TAG_GPS_VERSION_ID,
        )
    }
}

/**
 * Media read as it plays: blocks of [BLOCK] bytes fetched by Range on our
 * own client (no cookie, no User-Agent), the last few kept in memory. Called
 * by MediaPlayer on its own thread, where blocking is expected.
 */
private class RangeSource(private val client: OkHttpClient, private val url: String, private val total: Long) : MediaDataSource() {
    private val blocks = object : LinkedHashMap<Long, ByteArray>(KEEP, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Long, ByteArray>?) = this.size > KEEP
    }

    override fun getSize(): Long = total

    @Synchronized
    override fun readAt(position: Long, buffer: ByteArray, offset: Int, length: Int): Int {
        if (position >= total) return -1
        var done = 0
        while (done < length && position + done < total) {
            val at = position + done
            val index = at / BLOCK
            val block = blocks[index] ?: fetch(index).also { blocks[index] = it }
            val within = (at - index * BLOCK).toInt()
            if (within >= block.size) break
            val n = minOf(length - done, block.size - within)
            System.arraycopy(block, within, buffer, offset + done, n)
            done += n
        }
        return if (done == 0) -1 else done
    }

    private fun fetch(index: Long): ByteArray {
        val start = index * BLOCK
        val end = minOf(total, start + BLOCK) - 1
        val request = Request.Builder().url(url).header("Range", "bytes=$start-$end").build()
        client.newCall(request).execute().use { response ->
            val body = response.body ?: throw IOException("No body")
            return when (response.code) {
                206 -> body.bytes()
                // A server that ignores Range: take this block out of the whole.
                200 -> body.byteStream().use { input ->
                    input.skipNBytesCompat(start)
                    val out = ByteArray((end - start + 1).toInt())
                    var read = 0
                    while (read < out.size) {
                        val n = input.read(out, read, out.size - read)
                        if (n < 0) break
                        read += n
                    }
                    out.copyOf(read)
                }
                else -> throw IOException("HTTP ${response.code}")
            }
        }
    }

    override fun close() {
        synchronized(this) { blocks.clear() }
    }

    private fun java.io.InputStream.skipNBytesCompat(n: Long) {
        var left = n
        val scratch = ByteArray(64 * 1024)
        while (left > 0) {
            val read = read(scratch, 0, minOf(scratch.size.toLong(), left).toInt())
            if (read < 0) throw IOException("Ended early")
            left -= read
        }
    }

    private companion object {
        const val BLOCK = 512L * 1024
        const val KEEP = 8
    }
}
