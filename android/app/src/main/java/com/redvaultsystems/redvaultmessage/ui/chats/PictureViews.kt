package com.redvaultsystems.redvaultmessage.ui.chats

import android.graphics.Bitmap
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

private sealed interface PictureState {
    data object Loading : PictureState
    /** Past [PictureRules.AUTO_LOAD_BYTES]: waits for a tap. */
    data object Large : PictureState
    data object Failed : PictureState
    data class Ready(val bitmap: Bitmap) : PictureState
}

/**
 * A picture in its bubble, like the desktop's MessageDelegate: decrypted when it shows (a big one when
 * tapped), then tap for the full-size viewer. Long-press still opens the message's actions.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun PictureBubbleContent(message: Message, pictures: ChatPictures, muted: Color, onOpen: () -> Unit, onLongPress: () -> Unit) {
    val descriptor = message.file ?: return
    val colors = RedvaultTheme.colors
    val large = descriptor.originalSize > PictureRules.AUTO_LOAD_BYTES
    var state by remember(descriptor.objectId) {
        mutableStateOf<PictureState>(pictures.cachedThumb(descriptor)?.let { PictureState.Ready(it) } ?: if (large) PictureState.Large else PictureState.Loading)
    }
    // 0: not asked for yet (a big one); each tap on a failed or big one asks again.
    var attempt by remember(descriptor.objectId) { mutableIntStateOf(if (large) 0 else 1) }
    LaunchedEffect(descriptor.objectId, attempt) {
        if (attempt == 0 || state is PictureState.Ready) return@LaunchedEffect
        state = PictureState.Loading
        state = try {
            PictureState.Ready(pictures.thumb(descriptor))
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            PictureState.Failed
        }
    }
    val ready = state as? PictureState.Ready
    val image = remember(ready?.bitmap) { ready?.bitmap?.asImageBitmap() }
    val (width, height) = pictureSize(ready?.bitmap)
    val shape = RoundedCornerShape(10.dp)
    Box(
        Modifier
            .size(width, height)
            .clip(shape)
            .background(colors.bg.copy(alpha = 0.45f))
            .border(1.dp, colors.border, shape)
            .combinedClickable(
                onClick = {
                    when (state) {
                        is PictureState.Ready -> onOpen()
                        PictureState.Large, PictureState.Failed -> attempt++
                        PictureState.Loading -> {}
                    }
                },
                onLongClick = onLongPress,
            )
            .semantics {
                role = Role.Image
                contentDescription = "Picture, ${descriptor.filename}"
            },
        contentAlignment = Alignment.Center,
    ) {
        if (image != null) {
            Image(image, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
        } else {
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.padding(12.dp)) {
                if (state == PictureState.Loading) {
                    CircularProgressIndicator(color = muted, strokeWidth = 2.dp, modifier = Modifier.size(24.dp))
                } else {
                    Icon(painterResource(R.drawable.ui_image), contentDescription = null, tint = muted, modifier = Modifier.size(22.dp))
                }
                Text(
                    when (state) {
                        PictureState.Large -> FileMessage.formatBytes(descriptor.originalSize) + " · tap to load"
                        PictureState.Failed -> "Couldn't show this picture. Tap to try again."
                        else -> "Decrypting…"
                    },
                    color = muted,
                    textAlign = TextAlign.Center,
                    style = MaterialTheme.typography.labelSmall,
                )
            }
        }
    }
}

/** The bubble's size: the picture's shape within 248 x 320 dp (a fixed 240 x 160 until it's decoded). */
private fun pictureSize(bitmap: Bitmap?): Pair<Dp, Dp> {
    if (bitmap == null || bitmap.width <= 0 || bitmap.height <= 0) return 240.dp to 160.dp
    val ratio = bitmap.width.toFloat() / bitmap.height
    var w = 248f
    var h = w / ratio
    if (h > 320f) {
        h = 320f
        w = h * ratio
    }
    return w.coerceAtLeast(120f).dp to h.coerceAtLeast(80f).dp
}

/** A chat's picture full size, pinch to zoom, with Save and Open (like the desktop's ImageViewer). */
@Composable
internal fun PictureViewer(descriptor: FileDescriptor, pictures: ChatPictures, subtitle: String, onClose: () -> Unit) {
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    val scope = rememberCoroutineScope()
    var error by remember { mutableStateOf<String?>(null) }
    var bytes by remember { mutableStateOf<ByteArray?>(null) }
    val image by produceState<ImageBitmap?>(initialValue = null, descriptor.objectId) {
        value = try {
            val original = pictures.bytes(descriptor)
            bytes = original
            pictures.full(original)?.asImageBitmap().also { if (it == null) error = "Couldn't show this picture." }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = describeError(e, "Couldn't show this picture.")
            null
        }
    }
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    val transform = rememberTransformableState { zoom, pan, _ ->
        scale = (scale * zoom).coerceIn(1f, 6f)
        offset = if (scale == 1f) Offset.Zero else offset + pan
    }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument(descriptor.mimeType)) { target ->
        val original = bytes ?: return@rememberLauncherForActivityResult
        if (target == null) return@rememberLauncherForActivityResult
        runCatching { context.contentResolver.openOutputStream(target)?.use { it.write(original) } }
            .onFailure { error = "Couldn't save the picture." }
    }
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            val loaded = image
            if (loaded == null) {
                if (error == null) CircularProgressIndicator(color = Color.White, modifier = Modifier.align(Alignment.Center))
            } else {
                Image(
                    loaded,
                    contentDescription = descriptor.filename,
                    contentScale = ContentScale.Fit,
                    modifier = Modifier
                        .fillMaxSize()
                        .transformable(transform)
                        .graphicsLayer(scaleX = scale, scaleY = scale, translationX = offset.x, translationY = offset.y),
                )
            }
            Row(
                Modifier.fillMaxWidth().background(Color.Black.copy(alpha = 0.55f)).statusBarsPadding().padding(horizontal = 4.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                IconButton(onClick = onClose) {
                    Icon(painterResource(R.drawable.ic_close), contentDescription = "Close", tint = Color.White)
                }
                Column(Modifier.weight(1f)) {
                    Text(descriptor.filename, color = Color.White, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(subtitle, color = Color.White.copy(alpha = 0.7f), style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                TextButton(enabled = bytes != null, onClick = {
                    appLock?.expectExternalReturn()
                    save.launch(descriptor.filename)
                }) { Text("Save", color = Color.White, fontWeight = FontWeight.SemiBold) }
                TextButton(enabled = bytes != null, onClick = {
                    val original = bytes ?: return@TextButton
                    scope.launch {
                        try {
                            val file = pictures.fileFor(descriptor, original)
                            appLock?.expectExternalReturn()
                            openFile(context, file, descriptor.mimeType) { error = it }
                        } catch (e: CancellationException) {
                            throw e
                        } catch (e: Exception) {
                            error = "Couldn't open the picture."
                        }
                    }
                }) { Text("Open", color = Color.White, fontWeight = FontWeight.SemiBold) }
            }
            error?.let { Text(it, color = Color.White, textAlign = TextAlign.Center, modifier = Modifier.align(Alignment.Center).padding(24.dp)) }
        }
    }
}
