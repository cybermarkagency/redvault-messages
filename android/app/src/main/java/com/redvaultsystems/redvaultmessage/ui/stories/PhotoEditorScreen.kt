// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.SystemClock
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectDragGestures
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
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke as DrawStroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.studio.Adjust
import com.redvaultsystems.redvaultmessage.studio.EditHistory
import com.redvaultsystems.redvaultmessage.studio.EditState
import com.redvaultsystems.redvaultmessage.studio.Handle
import com.redvaultsystems.redvaultmessage.studio.ImageEdit
import com.redvaultsystems.redvaultmessage.studio.ImageRender
import com.redvaultsystems.redvaultmessage.studio.PhotoFilter
import com.redvaultsystems.redvaultmessage.studio.Point
import com.redvaultsystems.redvaultmessage.studio.Rect
import com.redvaultsystems.redvaultmessage.studio.Stroke
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayInputStream
import kotlin.math.abs
import kotlin.math.min

private const val PREVIEW_SIDE = 900

internal enum class PhotoTab(val label: String) { Crop("Crop"), Adjust("Adjust"), Filters("Filters"), Draw("Draw") }

/** The crop shapes offered (the web's list); "Free" is the whole picture, which is then dragged by its corners. */
private val ASPECTS = listOf("Free" to null, "1:1" to 1.0, "4:5" to 4.0 / 5, "9:16" to 9.0 / 16, "16:9" to 16.0 / 9)

/**
 * Edits a photo on this phone: crop, turn, flip, brightness, contrast, saturation, blur, filters and drawing, with undo and redo. Nothing is
 * uploaded to edit a photo, and the result is a fresh JPEG with none of the original's metadata (the same as the web's studio, whose maths
 * lives in [ImageEdit]).
 */
@Composable
internal fun PhotoEditorScreen(source: PreparedStoryMedia, onDone: (PreparedStoryMedia) -> Unit, onCancel: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    val loaded by produceState<Bitmap?>(null, source) {
        value = withContext(Dispatchers.IO) { runCatching { source.open().use { BitmapFactory.decodeStream(it) } }.getOrNull() }
    }
    val bitmap = loaded
    var history by remember { mutableStateOf(EditHistory.start(EditState.NEUTRAL)) }
    var tab by remember { mutableStateOf(PhotoTab.Crop) }
    var penColor by remember { mutableStateOf(StoryColor.White) }
    var penSize by remember { mutableStateOf(0.012) }
    var eraser by remember { mutableStateOf(false) }
    var stroke by remember { mutableStateOf<Stroke?>(null) }
    var cropLive by remember { mutableStateOf<Rect?>(null) }
    var preview by remember { mutableStateOf<ImageBitmap?>(null) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val state = history.present
    val latest by rememberUpdatedState(state)
    val latestCrop by rememberUpdatedState(cropLive)
    val grouping = remember { object { var id = ""; var at = 0L } }

    /** A change. Steps of the same kind close together (dragging a slider) count as one undo. */
    fun commit(next: EditState, group: String = "") {
        val now = SystemClock.elapsedRealtime()
        val same = group.isNotEmpty() && grouping.id == group && now - grouping.at < 700
        grouping.id = group
        grouping.at = now
        history = if (same) history.replace(next) else history.push(next)
    }

    // In the crop tab the picture is shown whole (turned, with colour) and the crop is an outline on it.
    val shown = if (tab == PhotoTab.Crop) state.copy(crop = null, strokes = emptyList()) else state
    LaunchedEffect(bitmap, shown) {
        val b = bitmap ?: return@LaunchedEffect
        delay(16)
        preview = withContext(Dispatchers.Default) { ImageRender.draw(b, shown, PREVIEW_SIDE).asImageBitmap() }
    }

    BackHandler(onBack = { if (!busy) onCancel() })

    Column(Modifier.fillMaxSize().background(Color.Black).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = { if (!busy) onCancel() }) { Text("Cancel", color = Color.White) }
            Box(Modifier.weight(1f))
            TextButton(enabled = history.canUndo && !busy, onClick = { history = history.undo() }) { Text("Undo", color = if (history.canUndo) Color.White else StoryStyle.track) }
            TextButton(enabled = history.canRedo && !busy, onClick = { history = history.redo() }) { Text("Redo", color = if (history.canRedo) Color.White else StoryStyle.track) }
            TextButton(enabled = ImageEdit.hasEdits(state) && !busy, onClick = { commit(EditState.NEUTRAL) }) { Text("Reset", color = if (ImageEdit.hasEdits(state)) Color.White else StoryStyle.track) }
            PillButton(
                if (busy) "Saving…" else "Done",
                onClick = {
                    val b = bitmap ?: return@PillButton
                    if (!ImageEdit.hasEdits(state)) return@PillButton onDone(source)
                    busy = true
                    scope.launch {
                        try {
                            val (bytes, w, h) = withContext(Dispatchers.Default) { ImageRender.renderJpeg(b, state, StoryLimits.MAX_IMAGE_SIDE) }
                            onDone(PreparedStoryMedia(StoryKind.Image, "image/jpeg", w, h, null, bytes.size.toLong()) { ByteArrayInputStream(bytes) })
                        } catch (e: kotlinx.coroutines.CancellationException) {
                            throw e
                        } catch (e: Throwable) {
                            error = "That photo couldn't be prepared."
                            busy = false
                        }
                    }
                },
                enabled = bitmap != null && !busy,
                height = 40.dp,
                modifier = Modifier.width(96.dp),
            )
        }

        BoxWithConstraints(Modifier.weight(1f).fillMaxWidth().padding(8.dp), contentAlignment = Alignment.Center) {
            val p = preview
            if (p == null) {
                Text(if (bitmap == null && loaded == null) "Opening the photo…" else "…", color = StoryStyle.track)
            } else {
                val ratio = p.width.toFloat() / p.height
                val w = min(maxWidth.value, maxHeight.value * ratio)
                Box(Modifier.size(w.dp, (w / ratio).dp)) {
                    Image(p, contentDescription = "Photo preview", contentScale = ContentScale.FillBounds, modifier = Modifier.fillMaxSize())
                    if (tab == PhotoTab.Crop) {
                        CropOverlay(
                            rect = cropLive ?: state.crop ?: Rect(0.0, 0.0, 1.0, 1.0),
                            onStart = { latest.crop ?: Rect(0.0, 0.0, 1.0, 1.0) },
                            onLive = { cropLive = it },
                            onEnd = { final -> if (final != null) commit(ImageEdit.withGeometry(latest, crop = final)); cropLive = null },
                        )
                    }
                    if (tab == PhotoTab.Draw) {
                        DrawOverlay(
                            inProgress = stroke,
                            onDown = { at -> stroke = ImageEdit.newStroke(penColor, penSize, eraser, at) },
                            onMove = { at -> stroke = stroke?.let { ImageEdit.extendStroke(it, at) } },
                            onUp = { stroke?.let { commit(ImageEdit.addStroke(latest, it)) }; stroke = null },
                        )
                    }
                }
            }
        }

        error?.let { Text(it, color = colors.danger, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 16.dp)) }

        ChipRow(options = PhotoTab.entries.toList(), selected = tab, label = { it.label }, onSelect = { tab = it; stroke = null })

        // The tool panel never takes more than its share of a short screen.
        Column(Modifier.fillMaxWidth().heightIn(max = 240.dp).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            when (tab) {
                PhotoTab.Crop -> if (bitmap != null) {
                    ChipRow(
                        options = listOf("⟲ Rotate", "Rotate ⟳", if (state.flipX) "Flip ✓" else "Flip"),
                        selected = "",
                        label = { it },
                        onSelect = {
                            when {
                                it.startsWith("⟲") -> commit(ImageEdit.withGeometry(state, rotation = ImageEdit.rotateBy(state.rotation, -1), crop = null))
                                it.startsWith("Rotate") -> commit(ImageEdit.withGeometry(state, rotation = ImageEdit.rotateBy(state.rotation, 1), crop = null))
                                else -> commit(ImageEdit.withGeometry(state, flipX = !state.flipX, crop = null))
                            }
                        },
                        edge = 0.dp,
                    )
                    val (rw, rh) = ImageEdit.rotatedSize(bitmap.width, bitmap.height, state.rotation)
                    ChipRow(
                        options = ASPECTS,
                        selected = ASPECTS[0],
                        label = { it.first },
                        onSelect = { choice -> commit(ImageEdit.withGeometry(state, crop = choice.second?.let { ImageEdit.aspectCrop(it, rw, rh) })) },
                        edge = 0.dp,
                    )
                    Text("Drag the corners, or the middle to move it.", color = StoryStyle.track, fontSize = 12.sp)
                    if (state.strokes.isNotEmpty()) Text("Changing the crop, turn or flip clears your drawing.", color = StoryStyle.track, fontSize = 12.sp)
                }
                PhotoTab.Adjust -> {
                    AdjustSlider("Brightness", state.adjust.brightness, -100.0, 100.0) { commit(state.copy(adjust = state.adjust.copy(brightness = it)), "brightness") }
                    AdjustSlider("Contrast", state.adjust.contrast, -100.0, 100.0) { commit(state.copy(adjust = state.adjust.copy(contrast = it)), "contrast") }
                    AdjustSlider("Saturation", state.adjust.saturation, -100.0, 100.0) { commit(state.copy(adjust = state.adjust.copy(saturation = it)), "saturation") }
                    AdjustSlider("Blur", state.adjust.blur, 0.0, 100.0) { commit(state.copy(adjust = state.adjust.copy(blur = it)), "blur") }
                }
                PhotoTab.Filters -> ChipRow(
                    options = PhotoFilter.entries.toList(),
                    selected = state.filter,
                    label = { it.label },
                    onSelect = { commit(state.copy(filter = it)) },
                    edge = 0.dp,
                )
                PhotoTab.Draw -> {
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.horizontalScroll(rememberScrollState()).fillMaxWidth()) {
                        for (c in StoryColor.entries) {
                            val on = penColor == c && !eraser
                            Box(
                                Modifier
                                    .size(30.dp)
                                    .clip(CircleShape)
                                    .background(Color(ImageRender.penColor(c)))
                                    .border(if (on) 3.dp else 1.dp, if (on) colors.accent else colors.border, CircleShape)
                                    .clickable { penColor = c; eraser = false }
                                    .semantics { contentDescription = "Pen colour " + c.wire },
                            )
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        PillButton(if (eraser) "Eraser on" else "Eraser", { eraser = !eraser }, style = if (eraser) PillStyle.Primary else PillStyle.Secondary, height = 40.dp, modifier = Modifier.width(120.dp))
                        Text("Size", color = Color.White, fontSize = 13.sp)
                        Slider(
                            value = penSize.toFloat(),
                            onValueChange = { penSize = ImageEdit.clamp(it.toDouble(), ImageEdit.MIN_PEN, ImageEdit.MAX_PEN) },
                            valueRange = ImageEdit.MIN_PEN.toFloat()..ImageEdit.MAX_PEN.toFloat(),
                            colors = SliderDefaults.colors(thumbColor = colors.accent, activeTrackColor = colors.accent, inactiveTrackColor = colors.border),
                            modifier = Modifier.weight(1f).semantics { contentDescription = "Pen size" },
                        )
                    }
                    Text("Draw on the picture. Undo takes back the last line.", color = StoryStyle.track, fontSize = 12.sp)
                }
            }
        }
    }
}

@Composable
private fun AdjustSlider(label: String, value: Double, from: Double, to: Double, onChange: (Double) -> Unit) {
    val colors = RedvaultTheme.colors
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(label, color = Color.White, fontSize = 13.sp, modifier = Modifier.width(88.dp))
        Slider(
            value = value.toFloat(),
            onValueChange = { onChange(it.toDouble().let { v -> Math.round(v).toDouble() }) },
            valueRange = from.toFloat()..to.toFloat(),
            colors = SliderDefaults.colors(thumbColor = colors.accent, activeTrackColor = colors.accent, inactiveTrackColor = colors.border),
            modifier = Modifier.weight(1f).semantics { contentDescription = label },
        )
        Text(Math.round(value).toString(), color = StoryStyle.track, fontSize = 13.sp, modifier = Modifier.width(36.dp))
    }
}

/** The crop outline over the picture, dragged by its corners or its middle. Positions are fractions of the picture. */
@Composable
private fun CropOverlay(rect: Rect, onStart: () -> Rect, onLive: (Rect) -> Unit, onEnd: (Rect?) -> Unit) {
    val current by rememberUpdatedState(rect)
    Canvas(
        Modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                var handle: Handle? = null
                var start = Rect(0.0, 0.0, 1.0, 1.0)
                var total = Offset.Zero
                var live: Rect? = null
                val grab = 40.dp.toPx()
                detectDragGestures(
                    onDragStart = { at ->
                        val r = current
                        start = onStart()
                        total = Offset.Zero
                        live = null
                        handle = hit(at, r, size.width.toFloat(), size.height.toFloat(), grab)
                    },
                    onDrag = { change, amount ->
                        change.consume()
                        total += amount
                        handle?.let {
                            val next = ImageEdit.dragRect(start, it, total.x / size.width.toDouble(), total.y / size.height.toDouble())
                            live = next
                            onLive(next)
                        }
                    },
                    onDragEnd = { onEnd(live); handle = null },
                    onDragCancel = { onEnd(null); handle = null },
                )
            }
            .semantics { contentDescription = "Crop outline" },
    ) {
        val x = (rect.x * size.width).toFloat()
        val y = (rect.y * size.height).toFloat()
        val w = (rect.w * size.width).toFloat()
        val h = (rect.h * size.height).toFloat()
        val dim = Color(0x99000000)
        // The part that will be cut off is dimmed.
        drawRect(dim, Offset.Zero, Size(size.width, y))
        drawRect(dim, Offset(0f, y + h), Size(size.width, size.height - y - h))
        drawRect(dim, Offset(0f, y), Size(x, h))
        drawRect(dim, Offset(x + w, y), Size(size.width - x - w, h))
        drawRect(Color.White, Offset(x, y), Size(w, h), style = DrawStroke(width = 2.dp.toPx()))
        val corner = 12.dp.toPx()
        for ((cx, cy) in listOf(x to y, x + w to y, x to y + h, x + w to y + h)) {
            drawCircle(Color.White, corner / 2, Offset(cx, cy))
        }
    }
}

/** Which part of the outline a finger landed on: a corner, the middle, or nothing. */
private fun hit(at: Offset, r: Rect, width: Float, height: Float, grab: Float): Handle? {
    val x = (r.x * width).toFloat()
    val y = (r.y * height).toFloat()
    val w = (r.w * width).toFloat()
    val h = (r.h * height).toFloat()
    val near = { cx: Float, cy: Float -> abs(at.x - cx) <= grab && abs(at.y - cy) <= grab }
    return when {
        near(x, y) -> Handle.NW
        near(x + w, y) -> Handle.NE
        near(x, y + h) -> Handle.SW
        near(x + w, y + h) -> Handle.SE
        at.x in x..(x + w) && at.y in y..(y + h) -> Handle.Move
        else -> null
    }
}

/** Where a finger draws: it follows the finger as one stroke, from touch down to lift. */
@Composable
private fun DrawOverlay(inProgress: Stroke?, onDown: (Point) -> Unit, onMove: (Point) -> Unit, onUp: () -> Unit) {
    Canvas(
        Modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                awaitEachGesture {
                    val down = awaitFirstDown()
                    fun at(o: Offset) = Point((o.x / size.width).toDouble(), (o.y / size.height).toDouble())
                    down.consume()
                    onDown(at(down.position))
                    while (true) {
                        val event = awaitPointerEvent()
                        val change = event.changes.firstOrNull { it.id == down.id } ?: break
                        if (!change.pressed) {
                            change.consume()
                            break
                        }
                        change.consume()
                        onMove(at(change.position))
                    }
                    onUp()
                }
            }
            .semantics { contentDescription = "Drawing area" },
    ) {
        val s = inProgress ?: return@Canvas
        // A line in progress is shown over the picture; an eraser's is shown as a pale line until it lands.
        val color = if (s.eraser) Color(0x66FFFFFF) else Color(ImageRender.penColor(s.color))
        val width = (s.size * size.width).toFloat()
        if (s.points.size == 1) {
            drawCircle(color, width / 2, Offset((s.points[0].x * size.width).toFloat(), (s.points[0].y * size.height).toFloat()))
        } else {
            val path = Path()
            path.moveTo((s.points[0].x * size.width).toFloat(), (s.points[0].y * size.height).toFloat())
            for (i in 1 until s.points.size) path.lineTo((s.points[i].x * size.width).toFloat(), (s.points[i].y * size.height).toFloat())
            drawPath(path, color, style = DrawStroke(width = width, cap = StrokeCap.Round, join = StrokeJoin.Round))
        }
    }
}
