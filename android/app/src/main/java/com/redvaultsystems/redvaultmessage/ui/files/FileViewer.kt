// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color as AndroidColor
import android.graphics.ImageDecoder
import android.graphics.drawable.AnimatedImageDrawable
import android.graphics.drawable.Drawable
import android.graphics.pdf.PdfRenderer
import android.os.Build
import android.os.ParcelFileDescriptor
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.MediaController
import android.widget.VideoView
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.DriveEntry
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

internal sealed interface Loaded {
    data class Loading(val progress: Float) : Loaded
    data class Ready(val file: File?, val text: FilePreview.TextPreview?) : Loaded
    data class Failed(val message: String) : Loaded
}

private val Stage = Color(0xFF0B0C0F)
private val Ink = Color(0xFFEDEEF0)
private val InkSoft = Color(0xFF9599A3)

/**
 * The Files viewer: a picture, text, CSV, JSON, audio, video or PDF shown over
 * the whole screen, without handing the decrypted file to another app. The
 * file is decrypted into the app's private cache and deleted again when this
 * closes (text is read into memory and its copy deleted at once). Links,
 * forms and scripts inside a PDF do nothing: its pages are drawn as pictures.
 */
@Composable
fun FileViewerDialog(files: FilesSession, entry: DriveEntry, onDismiss: () -> Unit, onSave: () -> Unit) {
    val kind = FilePreview.kindFor(entry.name) ?: return
    var progress by remember(entry.id) { mutableFloatStateOf(0f) }
    val loaded by produceState<Loaded>(Loaded.Loading(0f), entry.id) {
        if (entry.size > FilePreview.limitFor(kind)) {
            value = Loaded.Failed("This file is too big to preview here (up to ${sizeText(FilePreview.limitFor(kind))}). Save it instead.")
            return@produceState
        }
        val (file, error) = files.openCopy(entry.id) { progress = it }
        if (file == null) {
            value = Loaded.Failed(error ?: "Couldn't open it. Try again.")
            return@produceState
        }
        value = withContext(Dispatchers.IO) {
            when (kind) {
                FilePreview.Kind.Text, FilePreview.Kind.Csv, FilePreview.Kind.Json -> {
                    val text = FilePreview.readText(file, kind)
                    files.discardCopy(entry.id, file)  // only the text in memory is kept
                    text.error?.let { Loaded.Failed(it) } ?: Loaded.Ready(null, text)
                }
                FilePreview.Kind.Pdf ->
                    if (FilePreview.looksLikePdf(file)) Loaded.Ready(file, null)
                    else {
                        files.discardCopy(entry.id, file)
                        Loaded.Failed("This doesn't look like a PDF, so it can't be shown here. Save it instead.")
                    }
                else -> Loaded.Ready(file, null)
            }
        }
    }
    // The decrypted copy goes when the viewer closes.
    DisposableEffect(entry.id) {
        onDispose { (loaded as? Loaded.Ready)?.file?.let { files.discardCopy(entry.id, it) } }
    }

    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        FileViewerFrame(entry.name, entry.size, kind, loaded, progress, onDismiss, onSave)
    }
}

/** The viewer's contents (header, then the file) for a file already decrypted, or still loading, or failed. */
@Composable
internal fun FileViewerFrame(name: String, size: Long, kind: FilePreview.Kind, loaded: Loaded, progress: Float, onDismiss: () -> Unit, onSave: () -> Unit) {
        Column(Modifier.fillMaxSize().background(Stage).statusBarsPadding().navigationBarsPadding()) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f).padding(start = 6.dp)) {
                    Text(name, color = Ink, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("${sizeText(size)} · decrypted on this phone, shown only in RedVault", color = InkSoft, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                RoundButton(R.drawable.ui_download, "Save", onClick = onSave, container = Color(0xFF20232A), tint = Ink, size = 40.dp, iconSize = 18.dp)
                Box(Modifier.width(8.dp))
                RoundButton(R.drawable.ui_close, "Close", onClick = onDismiss, container = Color(0xFF20232A), tint = Ink, size = 40.dp, iconSize = 18.dp)
            }
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                when (val state = loaded) {
                    is Loaded.Loading -> Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        CircularProgressIndicator(color = RedvaultTheme.colors.accent)
                        Text(if (progress > 0f) "Decrypting… ${(progress * 100).toInt()}%" else "Decrypting…", color = Ink, fontSize = 14.sp)
                    }
                    is Loaded.Failed -> Failed(state.message, onSave)
                    is Loaded.Ready -> when (kind) {
                        FilePreview.Kind.Image -> ImagePreview(state.file!!, name)
                        FilePreview.Kind.Text, FilePreview.Kind.Json -> TextPreviewView(state.text!!.text, wrap = kind == FilePreview.Kind.Text)
                        FilePreview.Kind.Csv -> CsvPreview(state.text!!.csv!!)
                        FilePreview.Kind.Audio, FilePreview.Kind.Video -> MediaPreview(state.file!!, audio = kind == FilePreview.Kind.Audio)
                        FilePreview.Kind.Pdf -> PdfPreview(state.file!!, onSave)
                    }
                }
            }
        }
}

@Composable
private fun Failed(message: String, onSave: () -> Unit) {
    Column(Modifier.padding(32.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(14.dp)) {
        DsIcon(R.drawable.ui_alert_circle, RedvaultTheme.colors.warn, 30.dp)
        Text(message, color = Ink, fontSize = 15.sp, textAlign = TextAlign.Center)
        PillButton("Save instead", onSave, style = PillStyle.Secondary, icon = R.drawable.ui_download)
    }
}

/** A picture, sized to the screen; pinch to zoom and drag to move. GIFs and animated WebPs play (Android 9+). */
@Composable
private fun ImagePreview(file: File, name: String) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val targetPx = with(LocalDensity.current) { maxOf(maxWidth.toPx(), maxHeight.toPx()).toInt() * 2 }
        val drawable by produceState<Result<Any>?>(null, file) {
            value = withContext(Dispatchers.IO) { runCatching { decodePicture(file, targetPx) } }
        }
        var scale by remember { mutableFloatStateOf(1f) }
        var offsetX by remember { mutableFloatStateOf(0f) }
        var offsetY by remember { mutableFloatStateOf(0f) }
        val zoom = Modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                detectTransformGestures { _, pan, gestureZoom, _ ->
                    scale = (scale * gestureZoom).coerceIn(1f, 8f)
                    if (scale == 1f) { offsetX = 0f; offsetY = 0f } else { offsetX += pan.x; offsetY += pan.y }
                }
            }
            .graphicsLayer(scaleX = scale, scaleY = scale, translationX = offsetX, translationY = offsetY)
            .semantics { contentDescription = "Preview of $name" }
        when (val result = drawable?.getOrNull()) {
            null -> if (drawable?.isFailure == true) {
                Text("This picture can't be shown here. Save it instead.", color = Ink, modifier = Modifier.align(Alignment.Center).padding(24.dp), textAlign = TextAlign.Center)
            } else {
                CircularProgressIndicator(Modifier.align(Alignment.Center), color = RedvaultTheme.colors.accent)
            }
            is Bitmap -> Image(result.asImageBitmap(), contentDescription = null, contentScale = ContentScale.Fit, modifier = zoom)
            is Drawable -> AndroidView(
                factory = { context ->
                    ImageView(context).apply {
                        scaleType = ImageView.ScaleType.FIT_CENTER
                        setImageDrawable(result)
                        (result as? AnimatedImageDrawable)?.start()
                    }
                },
                onRelease = { ((it.drawable) as? AnimatedImageDrawable)?.stop() },
                modifier = zoom,
            )
        }
    }
}

/** Decodes a picture no bigger than needed: a Drawable (it may be animated) on Android 9+, a Bitmap before. */
private fun decodePicture(file: File, targetPx: Int): Any {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        return ImageDecoder.decodeDrawable(ImageDecoder.createSource(file)) { decoder, info, _ ->
            val longest = maxOf(info.size.width, info.size.height)
            var sample = 1
            while (longest / (sample * 2) >= targetPx) sample *= 2
            decoder.setTargetSampleSize(sample)
            decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
        }
    }
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeFile(file.path, bounds)
    var sample = 1
    while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= targetPx) sample *= 2
    return BitmapFactory.decodeFile(file.path, BitmapFactory.Options().apply { inSampleSize = sample })
        ?: throw IllegalArgumentException("Not a picture")
}

/** Text and JSON (pretty-printed when it parses): plain text only, never rendered. */
@Composable
private fun TextPreviewView(text: String, wrap: Boolean) {
    val colors = RedvaultTheme.colors
    val vertical = rememberScrollState()
    val horizontal = rememberScrollState()
    Box(
        Modifier
            .fillMaxSize()
            .padding(12.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(colors.surface)
            .verticalScroll(vertical)
            .let { if (wrap) it else it.horizontalScroll(horizontal) },
    ) {
        SelectionContainer {
            Text(text, color = colors.text, fontFamily = MonoFamily, fontSize = 13.sp, lineHeight = 19.sp, softWrap = wrap, modifier = Modifier.padding(16.dp))
        }
    }
}

/** CSV as a table, the first row as its header. */
@Composable
private fun CsvPreview(csv: FilePreview.Csv) {
    val colors = RedvaultTheme.colors
    val columns = csv.rows.maxOfOrNull { it.size } ?: 0
    val cellWidth = 150.dp
    Column(Modifier.fillMaxSize().padding(12.dp)) {
        if (csv.truncated) {
            Text("Showing the first 1,000 rows and 50 columns. Save the file to see all of it.", color = colors.warn, fontSize = 12.sp, modifier = Modifier.padding(bottom = 8.dp))
        }
        if (csv.rows.isEmpty()) {
            Text("This file is empty.", color = InkSoft)
            return@Column
        }
        Box(Modifier.fillMaxSize().clip(RoundedCornerShape(14.dp)).background(colors.surface).horizontalScroll(rememberScrollState())) {
            LazyColumn(Modifier.width(cellWidth * columns).fillMaxSize()) {
                itemsIndexed(csv.rows) { index, row ->
                    Row(Modifier.background(if (index == 0) colors.surface2 else if (index % 2 == 0) colors.surface2.copy(alpha = 0.4f) else Color.Transparent)) {
                        for (c in 0 until columns) {
                            Text(
                                row.getOrElse(c) { "" },
                                color = colors.text,
                                fontSize = 13.sp,
                                fontWeight = if (index == 0) FontWeight.SemiBold else FontWeight.Normal,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier.width(cellWidth).padding(horizontal = 12.dp, vertical = 9.dp),
                            )
                        }
                    }
                }
            }
        }
    }
}

/** Audio and video through Android's own player, with its standard controls. */
@Composable
private fun MediaPreview(file: File, audio: Boolean) {
    var failed by remember(file) { mutableStateOf(false) }
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        if (audio) {
            Box(Modifier.size(132.dp).clip(CircleShape).background(Color(0xFF20232A)), contentAlignment = Alignment.Center) {
                DsIcon(R.drawable.ui_headphones, RedvaultTheme.colors.accent, 52.dp)
            }
        }
        AndroidView(
            factory = { context ->
                val frame = FrameLayout(context)
                val video = VideoView(context)
                val controller = MediaController(context)
                // Audio has no picture: the player's surface is a single pixel in a corner.
                frame.addView(
                    video,
                    if (audio) FrameLayout.LayoutParams(1, 1, Gravity.TOP or Gravity.START)
                    else FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER),
                )
                frame.setBackgroundColor(AndroidColor.TRANSPARENT)
                controller.setAnchorView(frame)
                video.setMediaController(controller)
                video.setOnPreparedListener {
                    video.start()
                    controller.show(if (audio) 0 else 3000)
                }
                video.setOnErrorListener { _, _, _ ->
                    failed = true
                    true
                }
                video.setVideoPath(file.path)
                frame.setOnClickListener { if (controller.isShowing) controller.hide() else controller.show(if (audio) 0 else 3000) }
                frame.tag = video
                frame
            },
            onRelease = { (it.tag as? VideoView)?.stopPlayback() },
            modifier = Modifier.fillMaxSize(),
        )
        if (failed) {
            Text(
                "This file can't be played here (its format or codec isn't supported). Save it instead.",
                color = Ink,
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(32.dp).background(Stage),
            )
        }
    }
}

/** A PDF, page by page as pictures (Android's PdfRenderer): links, forms and scripts in it do nothing. */
@Composable
private fun PdfPreview(file: File, onSave: () -> Unit) {
    val opened = remember(file) {
        runCatching {
            val descriptor = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
            try {
                PdfRenderer(descriptor)
            } catch (e: Exception) {
                descriptor.close()
                throw e
            }
        }
    }
    val renderer = opened.getOrNull()
    val lock = remember(file) { Mutex() }
    DisposableEffect(renderer) {
        onDispose {
            // Rendering may still be running: close once it's done.
            if (renderer != null) CoroutineScope(Dispatchers.IO).launch { lock.withLock { renderer.close() } }
        }
    }
    if (renderer == null) {
        Failed("This PDF can't be shown here (it may be damaged or password-protected). Save it instead.", onSave)
        return
    }
    val pageCount = renderer.pageCount
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val widthPx = with(LocalDensity.current) { (maxWidth - 24.dp).toPx().toInt() }
        LazyColumn(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            items(pageCount) { index ->
                val page by produceState<Bitmap?>(null, index, widthPx) {
                    value = withContext(Dispatchers.IO) {
                        lock.withLock {
                            runCatching {
                                renderer.openPage(index).use { p ->
                                    val height = (widthPx.toLong() * p.height / maxOf(1, p.width)).toInt().coerceIn(1, widthPx * 4)
                                    Bitmap.createBitmap(widthPx, height, Bitmap.Config.ARGB_8888).also { bitmap ->
                                        bitmap.eraseColor(AndroidColor.WHITE)
                                        p.render(bitmap, null, null, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY)
                                    }
                                }
                            }.getOrNull()
                        }
                    }
                }
                Box(Modifier.fillMaxWidth().padding(horizontal = 12.dp).background(Color.White).aspectRatio(page?.let { it.width.toFloat() / it.height } ?: 0.707f)) {
                    page?.let { Image(it.asImageBitmap(), contentDescription = "Page ${index + 1} of $pageCount", modifier = Modifier.fillMaxSize()) }
                }
            }
            item { Text("$pageCount ${if (pageCount == 1) "page" else "pages"}", color = InkSoft, fontSize = 12.sp, modifier = Modifier.fillMaxWidth().padding(12.dp), textAlign = TextAlign.Center) }
        }
    }
}
