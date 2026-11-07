package com.redvaultsystems.redvaultmessage.ui.stories

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.HighlightView
import com.redvaultsystems.redvaultmessage.data.Story
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefs
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.components.SheetBar
import com.redvaultsystems.redvaultmessage.ui.components.SwitchCard
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

private enum class Page { Main, Close, Hide, Muted, Archive, Highlights, EditHighlight }

/** What a failed action says: our own messages as they are, the server's where it gave one. */
internal fun storyErrorText(e: Throwable): String = when (e) {
    is StoryInputException -> e.message.orEmpty()
    is ApiException -> if (e.status == 0) "Can't reach the server. Check your connection." else e.message.orEmpty().ifEmpty { "Something went wrong." }
    else -> "Something went wrong. Try again."
}

/**
 * Story settings: who sees your stories and what others see of you. Everything here is sealed on this phone with a key only your devices
 * hold (the server can't read who your close friends are), and the same settings apply on the web and desktop apps.
 */
@Composable
fun StorySettingsScreen(session: StoriesSession, state: StoriesState, onBack: () -> Unit, onOpenList: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var page by rememberSaveable { mutableStateOf(Page.Main) }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var editing by remember { mutableStateOf<HighlightView?>(null) }

    BackHandler(enabled = page != Page.Main) { page = if (page == Page.EditHighlight) Page.Highlights else Page.Main }

    fun change(archive: Boolean? = null, edit: (StoryPrefs) -> StoryPrefs) {
        if (busy) return
        busy = true
        error = null
        scope.launch {
            try {
                session.updatePrefs(edit, archive)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = storyErrorText(e)
            } finally {
                busy = false
            }
        }
    }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        SheetBar(
            when (page) {
                Page.Main -> "Stories"
                Page.Close -> "Close friends"
                Page.Hide -> "Hide stories from"
                Page.Muted -> "Muted"
                Page.Archive -> "Story archive"
                Page.Highlights -> "Highlights"
                Page.EditHighlight -> if (editing == null) "New highlight" else "Edit highlight"
            },
            onCancel = { if (page == Page.Main) onBack() else page = if (page == Page.EditHighlight) Page.Highlights else Page.Main },
            cancelText = if (page == Page.Main) "Back" else "Done",
        )
        if (!state.prefsReadable) {
            Callout(
                "Your story settings can't be opened on this phone, so nothing here can be changed and nothing is posted until they can. Try again later.",
                CalloutTone.Warn,
                R.drawable.ui_alert_circle,
                Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            )
        }
        error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle, Modifier.padding(horizontal = 20.dp, vertical = 8.dp)) }
        val prefs = state.prefs
        when (page) {
            Page.Main -> Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(bottom = 24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                SectionCaps("What others see of you")
                SwitchCard(
                    "Share that you viewed",
                    "People see you watched their story. Off: this phone never tells the server, and you can't see who watched yours.",
                    prefs.shareViews,
                    { v -> change { it.copy(shareViews = v) } },
                    Modifier.padding(horizontal = 20.dp),
                )
                SwitchCard(
                    "Allow replies by default",
                    "New stories let friends answer in a chat with you.",
                    prefs.allowReplies,
                    { v -> change { it.copy(allowReplies = v) } },
                    Modifier.padding(horizontal = 20.dp),
                )
                SectionCaps("Who sees your stories")
                NavRow("Close friends", countText(prefs.closeFriends, state), onClick = { page = Page.Close })
                NavRow("Hide stories from", countText(prefs.hideFrom, state), onClick = { page = Page.Hide })
                NavRow("Muted", countText(prefs.muted, state) + " · their stories stay out of your tray", onClick = { page = Page.Muted })
                SectionCaps("Keeping stories")
                SwitchCard(
                    "Save to archive",
                    "Stories that run out are kept for you alone. Off: they're deleted when their time is up.",
                    state.archiveEnabled,
                    { v -> change(archive = v) { it } },
                    Modifier.padding(horizontal = 20.dp),
                )
                NavRow("Story archive", "Your stories that left your story", onClick = { page = Page.Archive })
                NavRow("Highlights", "Stories kept on your profile", onClick = { page = Page.Highlights })
            }
            Page.Close -> FriendPicker(
                friends = state.friends,
                chosen = prefs.closeFriends - prefs.hideFrom.toSet(),
                note = "Only these friends see stories you share with Close friends. Someone you remove loses the close-friends stories already up.",
                enabled = !busy && state.prefsReadable,
                onToggle = { id, on -> change { p -> p.copy(closeFriends = if (on) p.closeFriends + id else p.closeFriends - id) } },
            )
            Page.Hide -> FriendPicker(
                friends = state.friends,
                chosen = prefs.hideFrom,
                note = "These friends never get your stories, and lose the ones already up. They aren't told.",
                enabled = !busy && state.prefsReadable,
                onToggle = { id, on -> change { p -> p.copy(hideFrom = if (on) p.hideFrom + id else p.hideFrom - id) } },
            )
            Page.Muted -> FriendPicker(
                friends = state.friends,
                chosen = prefs.muted,
                note = "Muted friends' stories stay out of your tray. They aren't told.",
                enabled = !busy && state.prefsReadable,
                onToggle = { id, on -> change { p -> p.copy(muted = if (on) p.muted + id else p.muted - id) } },
            )
            Page.Archive -> ArchivePage(session, onOpenList)
            Page.Highlights -> HighlightsPage(
                session,
                onNew = { editing = null; page = Page.EditHighlight },
                onEdit = { editing = it; page = Page.EditHighlight },
                onOpenList = onOpenList,
            )
            Page.EditHighlight -> HighlightEditor(session, state, editing, onDone = { page = Page.Highlights })
        }
    }
}

private fun countText(ids: Set<String>, state: StoriesState): String {
    val n = ids.count { id -> state.friends.any { it.userId == id } }
    return if (n == 0) "Nobody" else if (n == 1) "1 friend" else "$n friends"
}

@Composable
private fun NavRow(title: String, detail: String, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier
            .padding(horizontal = 20.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(colors.surface)
            .clickable(role = Role.Button, onClick = onClick)
            .padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, color = colors.text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
            Text(detail, color = colors.text2, fontSize = 13.sp)
        }
        Text("›", color = colors.text3, fontSize = 20.sp)
    }
}

@Composable
private fun FriendPicker(friends: List<StoryFriend>, chosen: Set<String>, note: String, enabled: Boolean, onToggle: (String, Boolean) -> Unit) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
        Text(note, color = colors.text2, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp))
        if (friends.isEmpty()) Text("You have no friends yet.", color = colors.text3, modifier = Modifier.padding(20.dp))
        for (friend in friends.sortedBy { it.handle.lowercase() }) {
            val on = friend.userId in chosen
            Row(
                Modifier
                    .fillMaxWidth()
                    .clickable(enabled = enabled, role = Role.Checkbox) { onToggle(friend.userId, !on) }
                    .padding(horizontal = 20.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Avatar(friend.avatarUrl, friend.handle, size = 40.dp)
                Text(friend.handle, color = colors.text, fontSize = 16.sp, modifier = Modifier.weight(1f))
                Checkbox(
                    checked = on,
                    onCheckedChange = null,
                    enabled = enabled,
                    colors = CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3),
                )
            }
        }
    }
}

private fun storyLine(s: Story): String {
    val what = when (s.kind) {
        StoryKind.Image -> "Photo"
        StoryKind.Video -> "Video"
        StoryKind.Text -> "Text"
    }
    val text = s.meta.text?.take(40)?.let { " · $it" }.orEmpty()
    val since = age(s.createdAtMs)
    return "$what$text · " + if (since == "now") "just now" else "$since ago"
}

private fun openAsList(session: StoriesSession, title: String, stories: List<Story>, handle: String = title) {
    session.openList(listOf(ViewerGroup("", handle, null, stories, isOwn = true)))
}

@Composable
private fun ArchivePage(session: StoriesSession, onOpenList: () -> Unit) {
    val colors = RedvaultTheme.colors
    var stories by remember { mutableStateOf<List<Story>?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) {
        try {
            stories = session.loadArchive()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = storyErrorText(e)
        }
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
        Text("Your stories that ran out or sit in a highlight, only for you. A story from another app that this app can't show isn't listed.", color = colors.text2, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp))
        error?.let { Text(it, color = colors.danger, modifier = Modifier.padding(20.dp)) }
        val list = stories
        when {
            list == null && error == null -> Text("Loading…", color = colors.text3, modifier = Modifier.padding(20.dp))
            list != null && list.isEmpty() -> Text("Nothing is kept yet. Turn on Save to archive and your stories stay here after their time is up.", color = colors.text3, modifier = Modifier.padding(20.dp))
            list != null -> list.forEachIndexed { i, story ->
                Text(
                    storyLine(story),
                    color = colors.text,
                    fontSize = 15.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable {
                            openAsList(session, "Archive", list.drop(i))
                            onOpenList()
                        }
                        .padding(horizontal = 20.dp, vertical = 14.dp),
                )
            }
        }
    }
}

@Composable
private fun HighlightsPage(session: StoriesSession, onNew: () -> Unit, onEdit: (HighlightView) -> Unit, onOpenList: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var list by remember { mutableStateOf<List<HighlightView>?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var reload by remember { mutableStateOf(0) }
    LaunchedEffect(reload) {
        try {
            list = session.highlightsOf(session.selfId())
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = storyErrorText(e)
        }
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Highlights keep chosen stories on your profile after their 24 hours. Friends who could see a story can see it here.", color = colors.text2, fontSize = 13.sp, modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp))
        PillButton("New highlight", onNew, modifier = Modifier.padding(horizontal = 20.dp), height = 46.dp)
        error?.let { Text(it, color = colors.danger, modifier = Modifier.padding(20.dp)) }
        for (h in list.orEmpty()) {
            Row(
                Modifier.padding(horizontal = 20.dp).fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(start = 16.dp, top = 8.dp, bottom = 8.dp, end = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(
                    Modifier.weight(1f).clickable {
                        openAsList(session, h.title ?: "Highlight", h.stories, handle = h.title ?: "Highlight")
                        onOpenList()
                    },
                ) {
                    Text(h.title ?: "Highlight", color = colors.text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
                    Text(if (h.stories.size == 1) "1 story" else "${h.stories.size} stories", color = colors.text2, fontSize = 13.sp)
                }
                PillButton("Edit", { onEdit(h) }, style = PillStyle.Quiet, height = 40.dp)
                PillButton(
                    "Delete",
                    {
                        scope.launch {
                            try {
                                session.deleteHighlight(h.id)
                                reload++
                            } catch (e: CancellationException) {
                                throw e
                            } catch (e: Exception) {
                                error = storyErrorText(e)
                            }
                        }
                    },
                    style = PillStyle.Danger,
                    height = 40.dp,
                )
            }
        }
        if (list != null && list.orEmpty().isEmpty()) Text("No highlights yet.", color = colors.text3, modifier = Modifier.padding(20.dp))
    }
}

@Composable
private fun HighlightEditor(session: StoriesSession, state: StoriesState, editing: HighlightView?, onDone: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var title by remember { mutableStateOf(editing?.title.orEmpty()) }
    var candidates by remember { mutableStateOf<List<Story>?>(null) }
    var chosen by remember { mutableStateOf(editing?.stories?.map { it.id }.orEmpty()) }
    var cover by remember { mutableStateOf(editing?.coverStoryId) }
    var error by remember { mutableStateOf<String?>(null) }
    var saving by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        try {
            // Today's stories and the archive; a story already in this highlight stays selectable even if it is in neither list.
            val archive = session.loadArchive()
            val all = (state.own + archive + editing?.stories.orEmpty()).distinctBy { it.id }.sortedByDescending { it.createdAtMs }
            candidates = all
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = storyErrorText(e)
            candidates = (state.own + editing?.stories.orEmpty()).distinctBy { it.id }
        }
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        TextField(
            value = title,
            onValueChange = { if (it.length <= StoryCrypto.MAX_HIGHLIGHT_TITLE) title = it },
            placeholder = { Text("Name", color = colors.placeholder) },
            singleLine = true,
            colors = TextFieldDefaults.colors(focusedContainerColor = colors.surface2, unfocusedContainerColor = colors.surface2, focusedTextColor = colors.text, unfocusedTextColor = colors.text),
            modifier = Modifier.padding(horizontal = 20.dp).fillMaxWidth(),
        )
        SectionCaps("Stories")
        val list = candidates
        if (list == null) Text("Loading…", color = colors.text3, modifier = Modifier.padding(20.dp))
        if (list != null && list.isEmpty()) Text("You have no stories to choose from. Turn on Save to archive first.", color = colors.text3, modifier = Modifier.padding(20.dp))
        for (story in list.orEmpty()) {
            val on = story.id in chosen
            Row(
                Modifier.fillMaxWidth().clickable(role = Role.Checkbox) {
                    chosen = if (on) chosen - story.id else chosen + story.id
                    if (on && cover == story.id) cover = null
                }.padding(horizontal = 20.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Checkbox(
                    checked = on,
                    onCheckedChange = null,
                    colors = CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3),
                )
                Text(storyLine(story), color = colors.text, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                if (on) {
                    val isCover = (cover ?: chosen.firstOrNull()) == story.id
                    Text(
                        if (isCover) "Cover" else "Make cover",
                        color = if (isCover) colors.accent else colors.text3,
                        fontSize = 12.sp,
                        modifier = Modifier.clip(RoundedCornerShape(10.dp)).clickable { cover = story.id }.padding(6.dp),
                    )
                }
            }
        }
        error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle, Modifier.padding(horizontal = 20.dp)) }
        PillButton(
            "Save highlight",
            {
                val picked = list.orEmpty().filter { it.id in chosen }
                val coverStory = picked.firstOrNull { it.id == (cover ?: chosen.firstOrNull()) } ?: picked.firstOrNull()
                saving = true
                error = null
                scope.launch {
                    try {
                        if (coverStory == null) throw StoryInputException("Choose at least one story.")
                        session.saveHighlight(editing?.id, title, picked, coverStory)
                        onDone()
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        error = storyErrorText(e)
                    } finally {
                        saving = false
                    }
                }
            },
            modifier = Modifier.padding(horizontal = 20.dp, vertical = 12.dp).fillMaxWidth(),
            enabled = !saving && title.isNotBlank() && chosen.isNotEmpty(),
            busy = saving,
        )
    }
}

/** The circles of someone's highlights (a friend's, on their contact page). Draws nothing unless Stories are on and they have any. */
@Composable
fun StoryHighlightsRow(session: StoriesSession, state: StoriesState, userId: String, onOpenList: () -> Unit) {
    if (!state.on) return
    val colors = RedvaultTheme.colors
    var list by remember(userId) { mutableStateOf<List<HighlightView>>(emptyList()) }
    LaunchedEffect(userId, state.friends.size) {
        list = try {
            session.highlightsOf(userId)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            emptyList()
        }
    }
    if (list.isEmpty()) return
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text("HIGHLIGHTS", color = colors.text3, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.8.sp)
        LazyRow(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            items(list, key = { it.id }) { h ->
                Column(
                    Modifier
                        .clip(RoundedCornerShape(14.dp))
                        .clickable {
                            session.openList(listOf(ViewerGroup(userId, h.title ?: "Highlight", null, h.stories, isOwn = false)))
                            onOpenList()
                        }
                        .padding(2.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Box(Modifier.size(60.dp).border(2.dp, StoryStyle.ringSeen, CircleShape).padding(5.dp).clip(CircleShape).background(colors.surface2), contentAlignment = Alignment.Center) {
                        Text((h.title ?: "H").take(1).uppercase(), color = colors.text, fontWeight = FontWeight.Bold, fontSize = 20.sp)
                    }
                    Text(h.title ?: "Highlight", color = colors.text2, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(horizontal = 2.dp))
                }
            }
        }
    }
}
