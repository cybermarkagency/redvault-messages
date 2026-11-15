// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.StoriesRepository
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.StoryMediaPrep
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.studio.StoryCollage
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayInputStream

/** A photo in the collage, decoded (upright, at most 1920 pixels on its long side), with the name it is listed under. */
internal class CollageItem(val id: Int, val name: String, val bitmap: Bitmap)

private const val PREVIEW_WIDTH = 360
private const val PREVIEW_HEIGHT = 640

private fun displayName(context: Context, uri: Uri, number: Int): String =
    runCatching {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0)?.takeIf { it.isNotBlank() } else null
        }
    }.getOrNull() ?: "Photo $number"

/**
 * Two to four photos in one picture, laid out and drawn on this phone (the web's collage maker, [StoryCollage]): pick photos, put them in
 * order, choose a layout that takes that many, set the space between and the background, and the result is a fresh story photo (a JPEG with
 * none of the photos' metadata). Nothing is uploaded to make it. [startWith] is for tests, in place of picking.
 */
@Composable
internal fun CollageScreen(onDone: (PreparedStoryMedia) -> Unit, onCancel: () -> Unit, startWith: List<Pair<String, Bitmap>> = emptyList()) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var nextId by remember { mutableIntStateOf(startWith.size + 1) }
    var items by remember { mutableStateOf(startWith.mapIndexed { i, (name, bitmap) -> CollageItem(i + 1, name, bitmap) }) }
    var layoutId by remember { mutableStateOf<String?>(null) }
    var gap by remember { mutableStateOf(StoryCollage.DEFAULT_GAP.toFloat()) }
    var background by remember { mutableIntStateOf(0) }
    var error by remember { mutableStateOf<String?>(null) }
    var loading by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }

    val layouts = StoryCollage.layoutsFor(items.size)
    val layout = layouts.firstOrNull { it.id == layoutId } ?: layouts.firstOrNull()
    val ready = layout != null && items.size >= 2

    DisposableEffect(Unit) { onDispose { items.forEach { it.bitmap.recycle() } } }
    BackHandler(onBack = { if (!busy) onCancel() })

    fun add(uris: List<Uri>) {
        if (uris.isEmpty() || loading) return
        error = null
        val room = StoryCollage.MAX_PHOTOS - items.size
        scope.launch {
            loading = true
            val added = ArrayList<CollageItem>()
            for (uri in uris.take(room)) {
                try {
                    val prepared = StoryMediaPrep.prepareImage(context, uri)
                    val bitmap = withContext(Dispatchers.IO) { prepared.open().use { BitmapFactory.decodeStream(it) } } ?: throw StoryInputException("That photo couldn't be opened.")
                    added += CollageItem(nextId++, displayName(context, uri, items.size + added.size + 1), bitmap)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: StoryInputException) {
                    error = e.message
                } catch (e: Exception) {
                    error = "That photo couldn't be opened."
                } catch (e: OutOfMemoryError) {
                    error = "That photo is too large to open on this phone."
                }
            }
            if (uris.size > room) error = "A collage holds at most ${StoryCollage.MAX_PHOTOS} photos."
            items = items + added
            layoutId = null
            loading = false
        }
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.PickMultipleVisualMedia(StoryCollage.MAX_PHOTOS)) { add(it) }

    fun move(index: Int, by: Int) {
        val to = index + by
        if (to < 0 || to >= items.size) return
        items = items.toMutableList().also { list -> val x = list[index]; list[index] = list[to]; list[to] = x }
    }

    fun remove(index: Int) {
        items[index].bitmap.recycle()
        items = items.filterIndexed { i, _ -> i != index }
        layoutId = null
    }

    fun finish() {
        val chosen = layout ?: return
        if (items.size != chosen.cells.size) return
        busy = true
        error = null
        scope.launch {
            try {
                val (bytes, w, h) = withContext(Dispatchers.Default) {
                    StoryCollage.renderJpeg(items.map { it.bitmap }, chosen, gap.toDouble(), StoryCollage.BACKGROUNDS[background].argb)
                }
                if (bytes.size > StoriesRepository.MAX_PHOTO_CIPHERTEXT_BYTES - 1024 * 1024) throw StoryInputException("That collage is too large for a story.")
                onDone(PreparedStoryMedia(StoryKind.Image, "image/jpeg", w, h, null, bytes.size.toLong()) { ByteArrayInputStream(bytes) })
            } catch (e: CancellationException) {
                throw e
            } catch (e: StoryInputException) {
                error = e.message
                busy = false
            } catch (e: Throwable) {
                error = "That collage couldn't be made."
                busy = false
            }
        }
    }

    val preview = remember(items, layout, gap, background) {
        if (layout != null && items.size == layout.cells.size) {
            StoryCollage.draw(items.map { it.bitmap }, layout, PREVIEW_WIDTH, PREVIEW_HEIGHT, gap / 3.0, StoryCollage.BACKGROUNDS[background].argb).asImageBitmap()
        } else {
            null
        }
    }

    Column(Modifier.fillMaxSize().background(Color.Black).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = { if (!busy) onCancel() }) { Text("Cancel", color = Color.White) }
            Text("Collage", color = Color.White, fontSize = 16.sp, modifier = Modifier.weight(1f).padding(start = 8.dp))
            PillButton(if (busy) "Making…" else "Done", onClick = ::finish, enabled = ready && !busy, height = 40.dp, modifier = Modifier.width(96.dp))
        }

        BoxWithConstraints(Modifier.weight(1f).fillMaxWidth().padding(8.dp), contentAlignment = Alignment.Center) {
            val p = preview
            if (p == null) {
                Text(
                    if (loading) "Opening the photos…" else "Choose two to four photos. They stay on this phone.",
                    color = StoryStyle.track,
                    modifier = Modifier.padding(24.dp),
                )
            } else {
                val ratio = p.width.toFloat() / p.height
                val w = minOf(maxWidth.value, maxHeight.value * ratio)
                Image(p, contentDescription = "Collage preview", contentScale = ContentScale.FillBounds, modifier = Modifier.size(w.dp, (w / ratio).dp))
            }
        }

        error?.let { Text(it, color = colors.danger, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 16.dp)) }

        Column(
            Modifier.fillMaxWidth().heightIn(max = 300.dp).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items.forEachIndexed { i, item ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("${i + 1}. ${item.name}", color = Color.White, fontSize = 14.sp, maxLines = 1, modifier = Modifier.weight(1f))
                    TextButton(enabled = i > 0 && !busy, onClick = { move(i, -1) }, modifier = Modifier.semantics { contentDescription = "Move ${item.name} earlier" }) { Text("↑", color = Color.White) }
                    TextButton(enabled = i < items.size - 1 && !busy, onClick = { move(i, 1) }, modifier = Modifier.semantics { contentDescription = "Move ${item.name} later" }) { Text("↓", color = Color.White) }
                    TextButton(enabled = !busy, onClick = { remove(i) }, modifier = Modifier.semantics { contentDescription = "Remove ${item.name}" }) { Text("Remove", color = colors.danger) }
                }
            }
            if (items.size < StoryCollage.MAX_PHOTOS) {
                PillButton(
                    if (items.isEmpty()) "Choose photos" else "Add a photo",
                    onClick = { picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)) },
                    style = PillStyle.Secondary,
                    enabled = !busy && !loading,
                    modifier = Modifier.fillMaxWidth(),
                    height = 44.dp,
                )
            }
            if (layouts.isNotEmpty()) {
                Text("Layout", color = Color.White, fontSize = 13.sp)
                ChipRow(options = layouts, selected = layout ?: layouts.first(), label = { it.label }, onSelect = { layoutId = it.id }, edge = 0.dp)
            }
            if (items.size >= 2) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("Space between", color = Color.White, fontSize = 13.sp)
                    Slider(
                        value = gap,
                        onValueChange = { gap = it },
                        valueRange = 0f..StoryCollage.MAX_GAP.toFloat(),
                        colors = SliderDefaults.colors(thumbColor = colors.accent, activeTrackColor = colors.accent, inactiveTrackColor = colors.border),
                        modifier = Modifier.weight(1f).semantics { contentDescription = "Space between photos" },
                    )
                }
                Text("Background", color = Color.White, fontSize = 13.sp)
                ChipRow(
                    options = StoryCollage.BACKGROUNDS.indices.toList(),
                    selected = background,
                    label = { StoryCollage.BACKGROUNDS[it].label },
                    onSelect = { background = it },
                    edge = 0.dp,
                )
            }
        }
    }
}
