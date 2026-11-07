package com.redvaultsystems.redvaultmessage.ui.stories

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.MediaStore
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContract
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.DraftContent
import com.redvaultsystems.redvaultmessage.data.DraftSummary
import com.redvaultsystems.redvaultmessage.data.PostStage
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.SlideMode
import com.redvaultsystems.redvaultmessage.data.StoryAudience
import com.redvaultsystems.redvaultmessage.data.StoryDraft
import com.redvaultsystems.redvaultmessage.data.StoryDraftStore
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.StoryMediaPrep
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.StickerLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.ui.chats.EmojiPickerDialog
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.components.SheetBar
import com.redvaultsystems.redvaultmessage.ui.components.SwitchCard
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.DateFormat
import java.util.Date

/**
 * What the composer holds while a story is being made: its slides and the choices that apply to all of them. It lives in the session, so
 * turning the phone or leaving the screen for a moment doesn't lose the work (a draft is what keeps it across restarts).
 */
internal class ComposerModel(defaultReplies: Boolean) {
    var slides by mutableStateOf(listOf(SlideUi(key = 0)))
    var current by mutableIntStateOf(0)
    var selected by mutableStateOf<Selected?>(null)
    var audience by mutableStateOf(StoryAudience.Friends)
    var customIds by mutableStateOf<Set<String>>(emptySet())
    var ttl by mutableStateOf<Int?>(null)
    var replies by mutableStateOf(defaultReplies)
    var draftId by mutableStateOf<String?>(null)
    var dirty by mutableStateOf(false)
    private var nextKey = 1

    val slide: SlideUi get() = slides[current.coerceIn(0, slides.lastIndex)]

    fun change(edit: (SlideUi) -> SlideUi) {
        val at = current.coerceIn(0, slides.lastIndex)
        slides = slides.mapIndexed { i, s -> if (i == at) edit(s) else s }
        dirty = true
    }

    fun addSlide(): Boolean {
        if (slides.size >= StoryDraftStore.MAX_SLIDES) return false
        slides = slides + SlideUi(key = nextKey++)
        current = slides.lastIndex
        selected = null
        dirty = true
        return true
    }

    fun removeCurrent() {
        if (slides.size <= 1) return
        val at = current.coerceIn(0, slides.lastIndex)
        slides = slides.filterIndexed { i, _ -> i != at }
        current = (at - 1).coerceAtLeast(0)
        selected = null
        dirty = true
    }

    fun select(index: Int) {
        current = index.coerceIn(0, slides.lastIndex)
        selected = null
    }

    fun hasWork(): Boolean = slides.any { it.hasContent }

    fun load(content: DraftContent, id: String, thumbnails: List<ImageBitmap?>, allowedTtl: List<Int>) {
        slides = content.slides.mapIndexed { i, s ->
            SlideUi(nextKey++, s.mode, s.text, s.background, s.font, s.media, thumbnails.getOrNull(i), s.overlays, s.stickers, s.captions)
        }
        current = 0
        selected = null
        audience = content.audience
        customIds = content.customIds.toSet()
        replies = content.allowReplies
        ttl = content.ttlSeconds.takeIf { it != 86_400 && it in allowedTtl }
        draftId = id
        dirty = false
    }

    fun toDraftContent() = DraftContent(slides.map { it.toDraft() }, audience, customIds.toList(), replies, ttl ?: 86_400)
}

private sealed interface ComposerDialog {
    data object AddWords : ComposerDialog
    data class EditWords(val index: Int) : ComposerDialog
    data object Emoji : ComposerDialog
    data object Sticker : ComposerDialog
    data object People : ComposerDialog
    data object Leave : ComposerDialog
    data class Drafts(val drafts: List<DraftSummary>) : ComposerDialog
}

private enum class Capture { Photo, Video }

private class CaptureStoryVideo : ActivityResultContract<Uri, Boolean>() {
    override fun createIntent(context: Context, input: Uri): Intent =
        Intent(MediaStore.ACTION_VIDEO_CAPTURE).apply {
            putExtra(MediaStore.EXTRA_OUTPUT, input)
            putExtra(MediaStore.EXTRA_DURATION_LIMIT, 60)
            putExtra(MediaStore.EXTRA_SIZE_LIMIT, StoryLimits.MAX_VIDEO_BYTES - 1024 * 1024)
            clipData = ClipData.newRawUri("", input)
            addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION or Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }

    override fun parseResult(resultCode: Int, intent: Intent?): Boolean = resultCode == Activity.RESULT_OK
}

/**
 * Making a story: one or several slides, each words on a colored background or a photo or video (from the gallery or the camera), with
 * text, emoji and stickers placed on it; who sees it and for how long. Everything is encrypted on this phone before it goes anywhere
 * (docs/stories.md). A photo is redrawn and saved fresh, so its location and camera details stay behind; a video is posted as it is,
 * which the screen says. Unfinished work can be saved as a draft that stays, encrypted, on this phone.
 */
@Composable
fun StoryComposerScreen(session: StoriesSession, state: StoriesState, onClose: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val model = remember { session.composerModel() }
    var stage by remember { mutableStateOf<PostStage?>(null) }
    var progress by remember { mutableStateOf<Pair<Int, Int>?>(null) }
    var preparing by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var note by remember { mutableStateOf<String?>(null) }
    var dialog by remember { mutableStateOf<ComposerDialog?>(null) }
    var draftCount by remember { mutableIntStateOf(0) }
    var pendingPath by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingCapture by rememberSaveable { mutableStateOf<String?>(null) }
    val busy = stage != null
    val slide = model.slide

    LaunchedEffect(Unit) { draftCount = runCatching { session.listDrafts().size }.getOrDefault(0) }

    val closeFriends = state.prefs.closeFriends
    val friendsWithKeys = state.friends.map { it.userId }.toSet()
    val closeCount = closeFriends.count { it in friendsWithKeys && it !in state.prefs.hideFrom }
    val hiddenCount = state.prefs.hideFrom.count { it in friendsWithKeys }
    val customCount = model.customIds.count { it in friendsWithKeys && it !in state.prefs.hideFrom }
    val audienceCount = when (model.audience) {
        StoryAudience.Friends -> state.friends.count { it.userId !in state.prefs.hideFrom }
        StoryAudience.CloseFriends -> closeCount
        StoryAudience.Custom -> customCount
    }

    fun attachMedia(uri: Uri?, kind: StoryKind, captured: File?) {
        if (uri == null) {
            captured?.delete()
            return
        }
        preparing = true
        error = null
        scope.launch {
            try {
                val prepared = if (kind == StoryKind.Image) StoryMediaPrep.prepareImage(context, uri) else StoryMediaPrep.prepareVideo(context, uri)
                val thumbnail = thumbnailOf(prepared)
                model.change { it.copy(mode = SlideMode.Media, media = prepared, thumbnail = thumbnail, captions = emptyList()) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: StoryInputException) {
                error = e.message
                captured?.delete()
            } catch (e: Exception) {
                error = "That couldn't be opened."
                captured?.delete()
            } finally {
                // A taken photo has been redrawn into memory; the original (with its location, if the camera wrote it) is not kept. A taken video
                // stays until the session ends, because it is what gets uploaded.
                if (kind == StoryKind.Image) captured?.delete()
                preparing = false
            }
        }
    }
    val pickPhoto = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { attachMedia(it, StoryKind.Image, null) }
    val pickVideo = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { attachMedia(it, StoryKind.Video, null) }

    fun captureTarget(extension: String): Pair<File, Uri> {
        val dir = File(context.cacheDir, "stories/capture").also { it.mkdirs() }
        val file = File(dir, "capture-${System.currentTimeMillis()}.$extension")
        return file to FileProvider.getUriForFile(context, "${context.packageName}.files", file)
    }

    fun pendingFile(): File? = pendingPath?.let { File(it) }
    fun pendingUri(): Uri? = pendingFile()?.let { FileProvider.getUriForFile(context, "${context.packageName}.files", it) }

    val takePhoto = rememberLauncherForActivityResult(ActivityResultContracts.TakePicture()) { ok ->
        val file = pendingFile()
        val uri = pendingUri()
        pendingPath = null
        if (ok && uri != null) attachMedia(uri, StoryKind.Image, file) else file?.delete()
    }
    val takeVideo = rememberLauncherForActivityResult(CaptureStoryVideo()) { ok ->
        val file = pendingFile()
        val uri = pendingUri()
        pendingPath = null
        if (ok && uri != null && file != null && file.length() > 0) attachMedia(uri, StoryKind.Video, file) else file?.delete()
    }

    fun launchCapture(what: Capture) {
        try {
            val (file, uri) = captureTarget(if (what == Capture.Photo) "jpg" else "mp4")
            pendingPath = file.absolutePath
            if (what == Capture.Photo) takePhoto.launch(uri) else takeVideo.launch(uri)
        } catch (e: Exception) {
            pendingPath = null
            error = "There is no camera app to use."
        }
    }
    val cameraPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val what = pendingCapture?.let { runCatching { Capture.valueOf(it) }.getOrNull() }
        pendingCapture = null
        if (granted && what != null) launchCapture(what) else error = "Allow the camera in the phone's settings to take a photo or video."
    }

    fun openCamera(what: Capture) {
        error = null
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            launchCapture(what)
        } else {
            pendingCapture = what.name
            cameraPermission.launch(Manifest.permission.CAMERA)
        }
    }

    fun slideReady(s: SlideUi) = when (s.mode) {
        SlideMode.Text -> s.text.isNotBlank() && s.text.length <= StoryLimits.MAX_STORY_TEXT
        SlideMode.Media -> s.media != null
    }

    val audienceOk = when (model.audience) {
        StoryAudience.Friends -> true
        StoryAudience.CloseFriends -> closeCount > 0
        StoryAudience.Custom -> customCount > 0
    }
    val canShare = !busy && !preparing && model.slides.all(::slideReady) && audienceOk

    fun saveDraft(then: (() -> Unit)? = null) {
        error = null
        scope.launch {
            try {
                model.draftId = session.saveDraft(model.toDraftContent(), model.draftId)
                model.dirty = false
                draftCount = runCatching { session.listDrafts().size }.getOrDefault(draftCount)
                note = "Saved as a draft. It stays on this phone, encrypted."
                then?.invoke()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = "Couldn't save the draft."
            }
        }
    }

    fun leave() {
        // What was taken with the camera for this story has been posted or copied into a draft by now; the originals go.
        File(context.cacheDir, "stories/capture").deleteRecursively()
        session.discardComposer()
        onClose()
    }

    fun attemptClose() {
        if (busy) return
        if (model.dirty && model.hasWork()) dialog = ComposerDialog.Leave else leave()
    }
    BackHandler(onBack = ::attemptClose)

    fun share() {
        error = null
        note = null
        val total = model.slides.size
        stage = PostStage.Encrypting
        scope.launch {
            var posted = 0
            try {
                while (model.slides.isNotEmpty()) {
                    progress = posted to total
                    val s = model.slides.first()
                    val story = StoryDraft(
                        text = if (s.mode == SlideMode.Text) s.text else null,
                        background = s.background,
                        font = s.font,
                        media = if (s.mode == SlideMode.Media) s.media else null,
                        audience = model.audience,
                        allowReplies = model.replies,
                        ttlSeconds = model.ttl?.takeIf { it != 86_400 && it in state.ttlSeconds },
                        overlays = s.overlays,
                        stickers = s.stickers,
                        customIds = model.customIds,
                        captions = if (s.mode == SlideMode.Media) s.captions else emptyList(),
                    )
                    session.publish(story, refreshAfter = model.slides.size == 1) { stage = it }
                    posted++
                    if (model.slides.size == 1) break
                    // What is posted is gone from the list, so a failure further on never posts it twice.
                    model.slides = model.slides.drop(1)
                    model.current = 0
                    model.selected = null
                    model.draftId?.let { id -> runCatching { session.saveDraft(model.toDraftContent(), id) } }
                }
                model.draftId?.let { id -> runCatching { session.deleteDraft(id) } }
                leave()
            } catch (e: CancellationException) {
                throw e
            } catch (e: StoryInputException) {
                error = e.message
            } catch (e: ApiException) {
                error = if (e.status == 0) "Can't reach the server. Check your connection." else e.message
            } catch (e: Exception) {
                error = (e as? com.redvaultsystems.redvaultmessage.data.UserFacingException)?.message ?: "Couldn't post that. Try again."
            } finally {
                stage = null
                progress = null
            }
            if (posted > 0 && model.slides.isNotEmpty() && error != null) {
                error = "Posted $posted of $total. ${error.orEmpty()}"
            }
        }
    }

    fun placeY(count: Int) = 0.35 + 0.15 * (count % 4)

    fun updateOverlay(index: Int, overlay: StoryOverlay) = model.change { it.copy(overlays = it.overlays.mapIndexed { i, o -> if (i == index) overlay else o }) }

    fun updateSticker(sticker: StorySticker) = model.change { it.copy(stickers = it.stickers.map { s -> if (s.id == sticker.id) sticker else s }) }

    /** Smaller, larger, or turned: the selected item's scale and rotation. */
    fun adjustSelected(factor: Double, turn: Double) {
        when (val sel = model.selected) {
            is Selected.Overlay -> slide.overlays.getOrNull(sel.index)?.let { o ->
                updateOverlay(sel.index, o.copy(scale = (o.scale * factor).coerceIn(0.5, 4.0), rotation = (((o.rotation + turn) % 360) + 360) % 360))
            }
            is Selected.Sticker -> slide.stickers.firstOrNull { it.id == sel.id }?.let { s ->
                updateSticker(withPlacement(s, s.x, s.y, (s.scale * factor).coerceIn(0.6, 2.5), (((s.rotation + turn) % 360) + 360) % 360))
            }
            null -> {}
        }
    }

    fun removeSelected() {
        when (val sel = model.selected) {
            is Selected.Overlay -> model.change { it.copy(overlays = it.overlays.filterIndexed { i, _ -> i != sel.index }) }
            is Selected.Sticker -> model.change { it.copy(stickers = it.stickers.filter { s -> s.id != sel.id }) }
            null -> {}
        }
        model.selected = null
    }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        SheetBar("New story", onCancel = ::attemptClose, cancelText = "Close")
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(bottom = 16.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            if (draftCount > 0) {
                Text(
                    "Drafts ($draftCount)",
                    color = colors.accent,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .padding(horizontal = 20.dp)
                        .clickable(enabled = !busy) { scope.launch { dialog = ComposerDialog.Drafts(runCatching { session.listDrafts() }.getOrDefault(emptyList())) } }
                        .padding(vertical = 4.dp),
                )
            }
            val slideChoices = model.slides.indices.toList() + (if (model.slides.size < StoryDraftStore.MAX_SLIDES) listOf(-1) else emptyList())
            ChipRow(
                options = slideChoices,
                selected = model.current,
                label = { if (it < 0) "+ Add slide" else "Slide ${it + 1}" },
                onSelect = { if (!busy) { if (it < 0) model.addSlide() else model.select(it) } },
            )
            if (model.slides.size > 1) {
                Text(
                    "Remove this slide",
                    color = colors.danger,
                    fontSize = 14.sp,
                    modifier = Modifier.padding(horizontal = 20.dp).clickable(enabled = !busy) { model.removeCurrent() }.padding(vertical = 2.dp),
                )
            }

            if (state.media) {
                ChipRow(
                    options = listOf(SlideMode.Text, SlideMode.Media),
                    selected = slide.mode,
                    label = { if (it == SlideMode.Text) "Words" else "Photo or video" },
                    onSelect = { if (!busy) model.change { s -> s.copy(mode = it) } },
                )
            }

            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                SlideEditor(
                    slide = slide,
                    selected = model.selected,
                    friends = state.friends.associateBy { it.userId },
                    onSelect = { model.selected = it },
                    onOverlay = ::updateOverlay,
                    onSticker = ::updateSticker,
                    modifier = Modifier.fillMaxWidth(0.62f),
                )
            }

            if (slide.mode == SlideMode.Text) {
                StyledTextField(
                    slide.text,
                    { if (it.length <= StoryLimits.MAX_STORY_TEXT && !busy) model.change { s -> s.copy(text = it) } },
                    "Type something",
                    Modifier.padding(horizontal = 20.dp),
                    singleLine = false,
                )
                Text("${slide.text.length}/${StoryLimits.MAX_STORY_TEXT}", color = colors.text3, fontSize = 11.sp, modifier = Modifier.padding(horizontal = 20.dp))
                SectionCaps("Background")
                ChipRow(options = StoryBackground.entries.toList(), selected = slide.background, label = { StoryStyle.label(it) }, onSelect = { model.change { s -> s.copy(background = it) } })
                SectionCaps("Style")
                ChipRow(options = StoryFont.entries.toList(), selected = slide.font, label = { StoryStyle.label(it) }, onSelect = { model.change { s -> s.copy(font = it) } })
            } else {
                Column(Modifier.padding(horizontal = 20.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    if (preparing) Text("Getting it ready…", color = colors.text2)
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        PillButton("Choose photo", { pickPhoto.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)) }, Modifier.weight(1f), PillStyle.Secondary, enabled = !busy && !preparing, height = 44.dp)
                        PillButton("Take photo", { openCamera(Capture.Photo) }, Modifier.weight(1f), PillStyle.Secondary, enabled = !busy && !preparing, height = 44.dp)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        PillButton("Choose video", { pickVideo.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.VideoOnly)) }, Modifier.weight(1f), PillStyle.Secondary, enabled = !busy && !preparing, height = 44.dp)
                        PillButton("Record video", { openCamera(Capture.Video) }, Modifier.weight(1f), PillStyle.Secondary, enabled = !busy && !preparing, height = 44.dp)
                    }
                    val media = slide.media
                    if (media != null && media.kind == StoryKind.Video) {
                        Text("Video · ${((media.durationMs ?: 0) / 1000)} s · ${"%.1f".format(media.sizeBytes / 1024.0 / 1024.0)} MB", color = colors.text, fontWeight = FontWeight.SemiBold)
                    }
                    Text(
                        "A photo is saved fresh before it's posted: its location, camera and time stay behind. A video is posted as it is (up to 60 seconds and 60 MB), so details inside the file, such as a location tag, stay in it. Only the people who can see the story can read them.",
                        color = colors.text3,
                        fontSize = 12.sp,
                    )
                }
            }

            SectionCaps("Add to this slide")
            ChipRow(
                options = listOf("Words", "Emoji", "Sticker"),
                selected = "",
                label = { it },
                onSelect = {
                    if (!busy) when (it) {
                        "Words" -> if (slide.overlays.size < StoryLimits.MAX_OVERLAYS) dialog = ComposerDialog.AddWords else error = "That's as many words and emoji as fit."
                        "Emoji" -> if (slide.overlays.size < StoryLimits.MAX_OVERLAYS) dialog = ComposerDialog.Emoji else error = "That's as many words and emoji as fit."
                        else -> if (slide.stickers.size < StickerLimits.MAX_STICKERS) dialog = ComposerDialog.Sticker else error = "That's as many stickers as fit."
                    }
                },
            )
            val sel = model.selected
            if (sel != null) {
                val editable = sel is Selected.Overlay && slide.overlays.getOrNull(sel.index)?.isEmoji == false
                ChipRow(
                    options = buildList { add("Smaller"); add("Larger"); add("Turn"); if (editable) add("Edit"); add("Remove") },
                    selected = "",
                    label = { it },
                    onSelect = {
                        when (it) {
                            "Smaller" -> adjustSelected(0.85, 0.0)
                            "Larger" -> adjustSelected(1.18, 0.0)
                            "Turn" -> adjustSelected(1.0, 15.0)
                            "Edit" -> (sel as? Selected.Overlay)?.let { o -> dialog = ComposerDialog.EditWords(o.index) }
                            else -> removeSelected()
                        }
                    },
                )
            }
            Text(
                "Drag to move, pinch to size and turn. Tap the picture to let go.",
                color = colors.text3,
                fontSize = 12.sp,
                modifier = Modifier.padding(horizontal = 20.dp),
            )

            SectionCaps("Who sees it")
            ChipRow(
                options = StoryAudience.entries.toList(),
                selected = model.audience,
                label = {
                    when (it) {
                        StoryAudience.Friends -> "Friends"
                        StoryAudience.CloseFriends -> "Close friends"
                        StoryAudience.Custom -> "Choose people"
                    }
                },
                onSelect = {
                    if (!busy) {
                        model.audience = it
                        model.dirty = true
                        if (it == StoryAudience.Custom) dialog = ComposerDialog.People
                    }
                },
            )
            Text(
                when {
                    model.audience == StoryAudience.CloseFriends && closeCount == 0 -> "You have no close friends yet. Add some in your story settings."
                    model.audience == StoryAudience.Custom && customCount == 0 -> "Choose at least one friend."
                    else -> (if (audienceCount == 1) "1 friend" else "$audienceCount friends") + " can see it" +
                        (if (hiddenCount > 0 && model.audience == StoryAudience.Friends) ", not the $hiddenCount you hide stories from" else "") + "."
                },
                color = colors.text2,
                fontSize = 13.sp,
                modifier = Modifier.padding(horizontal = 20.dp),
            )

            if (state.ttlSeconds.size > 1) {
                SectionCaps("Disappears after")
                ChipRow(
                    options = state.ttlSeconds,
                    selected = model.ttl ?: 86_400,
                    label = { lifetimeLabel(it) },
                    onSelect = { if (!busy) { model.ttl = it; model.dirty = true } },
                )
            }
            SwitchCard(
                "Allow replies",
                "Friends can answer your story in a chat with you.",
                model.replies,
                { if (!busy) { model.replies = it; model.dirty = true } },
                Modifier.padding(horizontal = 20.dp),
            )

            Callout(
                "End-to-end encrypted on this phone before it's sent. Anyone who can see a story can still screenshot or copy it.",
                CalloutTone.Neutral,
                R.drawable.ui_lock,
                Modifier.padding(horizontal = 20.dp),
            )
        }
        // Said where it can always be seen, not somewhere down the page.
        if (error != null || note != null) {
            Column(Modifier.padding(horizontal = 20.dp).padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
                note?.let { Callout(it, CalloutTone.Ok, R.drawable.ui_lock) }
            }
        }
        Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            PillButton("Save draft", { saveDraft() }, Modifier.weight(0.8f), PillStyle.Secondary, enabled = !busy && !preparing && model.hasWork())
            PillButton(
                when (stage) {
                    PostStage.Encrypting -> progress?.let { (done, total) -> if (total > 1) "Encrypting ${done + 1} of $total…" else null } ?: "Encrypting…"
                    PostStage.Uploading -> progress?.let { (done, total) -> if (total > 1) "Posting ${done + 1} of $total…" else null } ?: "Posting…"
                    null -> if (model.slides.size > 1) "Share ${model.slides.size} slides" else "Share story"
                },
                onClick = ::share,
                enabled = canShare || busy,
                busy = busy,
                modifier = Modifier.weight(1.2f),
            )
        }
    }

    when (val d = dialog) {
        null -> {}
        ComposerDialog.AddWords -> OverlayDialog(
            initial = null,
            onDone = { overlay ->
                model.change { it.copy(overlays = it.overlays + overlay.copy(y = placeY(it.overlays.size))) }
                model.selected = Selected.Overlay(model.slide.overlays.lastIndex)
                dialog = null
            },
            onDismiss = { dialog = null },
        )
        is ComposerDialog.EditWords -> OverlayDialog(
            initial = slide.overlays.getOrNull(d.index),
            onDone = { overlay -> updateOverlay(d.index, overlay); dialog = null },
            onDismiss = { dialog = null },
        )
        ComposerDialog.Emoji -> EmojiPickerDialog(
            onPick = { emoji ->
                model.change { it.copy(overlays = it.overlays + StoryOverlay(true, emoji, 0.5, placeY(it.overlays.size), 1.0, 0.0, StoryColor.White, false, StoryFont.Classic, null)) }
                model.selected = Selected.Overlay(model.slide.overlays.lastIndex)
                dialog = null
            },
            onDismiss = { dialog = null },
        )
        ComposerDialog.Sticker -> StickerDialog(
            friends = state.friends,
            interactive = state.interactive,
            onDone = { sticker ->
                model.change { it.copy(stickers = it.stickers + withPlacement(sticker, 0.5, placeY(it.stickers.size), 1.0, 0.0)) }
                model.selected = Selected.Sticker(sticker.id)
                dialog = null
            },
            onDismiss = { dialog = null },
        )
        ComposerDialog.People -> PeopleDialog(
            friends = state.friends,
            hidden = state.prefs.hideFrom,
            chosen = model.customIds,
            onDone = { picked -> model.customIds = picked; model.dirty = true; dialog = null },
            onDismiss = { dialog = null },
        )
        ComposerDialog.Leave -> AlertDialog(
            onDismissRequest = { dialog = null },
            containerColor = colors.surface,
            title = { Text("Keep this story?", color = colors.text) },
            text = { Text("You haven't shared it. Save it as a draft to finish later; a draft stays on this phone, encrypted.", color = colors.text2) },
            confirmButton = { TextButton(onClick = { dialog = null; saveDraft(then = ::leave) }) { Text("Save draft", color = colors.accent) } },
            dismissButton = { TextButton(onClick = { dialog = null; leave() }) { Text("Discard", color = colors.danger) } },
        )
        is ComposerDialog.Drafts -> DraftsDialog(
            drafts = d.drafts,
            onOpen = { id ->
                dialog = null
                scope.launch {
                    val content = runCatching { session.loadDraft(id) }.getOrNull()
                    if (content == null) {
                        error = "That draft couldn't be opened."
                    } else {
                        model.load(content, id, content.slides.map { s -> s.media?.let { thumbnailOf(it) } }, state.ttlSeconds)
                        note = null
                        error = null
                    }
                }
            },
            onDelete = { id ->
                scope.launch {
                    runCatching { session.deleteDraft(id) }
                    if (model.draftId == id) model.draftId = null
                    val left = runCatching { session.listDrafts() }.getOrDefault(emptyList())
                    draftCount = left.size
                    dialog = if (left.isEmpty()) null else ComposerDialog.Drafts(left)
                }
            },
            onDismiss = { dialog = null },
        )
    }
}

@Composable
private fun DraftsDialog(drafts: List<DraftSummary>, onOpen: (String) -> Unit, onDelete: (String) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text("Drafts", color = colors.text) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                for (d in drafts) {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f).clickable { onOpen(d.id) }.padding(vertical = 8.dp)) {
                            Text(d.preview.ifBlank { "Untitled" }, color = colors.text, fontWeight = FontWeight.SemiBold, maxLines = 1)
                            Text(
                                (if (d.slideCount == 1) "1 slide" else "${d.slideCount} slides") + " · " +
                                    DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(d.updatedAt)),
                                color = colors.text3,
                                fontSize = 12.sp,
                            )
                        }
                        TextButton(onClick = { onDelete(d.id) }) { Text("Delete", color = colors.danger) }
                    }
                }
                Text("Drafts stay on this phone, encrypted. They are kept for 30 days.", color = colors.text3, fontSize = 12.sp)
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Close", color = colors.accent) } },
    )
}

/** A photo as a small picture for the editor (the full one stays in the file that gets posted). */
private suspend fun thumbnailOf(media: PreparedStoryMedia): ImageBitmap? = withContext(Dispatchers.IO) {
    if (media.kind != StoryKind.Image) return@withContext null
    runCatching {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        media.open().use { BitmapFactory.decodeStream(it, null, bounds) }
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= 800) sample *= 2
        media.open().use { BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample }) }?.asImageBitmap()
    }.getOrNull()
}

/** "1 hour", "6 hours", "2 days": what a lifetime in seconds is called. */
internal fun lifetimeLabel(seconds: Int): String {
    val hours = seconds / 3600
    return when {
        seconds % 86_400 == 0 && seconds >= 2 * 86_400 -> "${seconds / 86_400} days"
        hours == 1 -> "1 hour"
        hours > 1 -> "$hours hours"
        else -> "${seconds / 60} minutes"
    }
}
