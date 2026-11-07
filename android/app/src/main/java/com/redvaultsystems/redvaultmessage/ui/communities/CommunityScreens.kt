package com.redvaultsystems.redvaultmessage.ui.communities

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.CommunitiesConfigDto
import com.redvaultsystems.redvaultmessage.data.api.CommunityListingDto
import com.redvaultsystems.redvaultmessage.data.api.CommunityPageDto
import com.redvaultsystems.redvaultmessage.data.api.MembershipViewDto
import com.redvaultsystems.redvaultmessage.data.api.MyCommunityDto
import com.redvaultsystems.redvaultmessage.data.model.Communities
import com.redvaultsystems.redvaultmessage.ui.TimeFormat
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.groups.GroupAvatar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.time.Instant

/** The tabs of the Communities screen. Creating and running a community stays on the web and desktop. */
enum class CommunitiesPage(val key: String, val label: String) {
    Discover("discover", "Discover"),
    Mine("mine", "My communities"),
    Memberships("memberships", "Memberships");

    companion object {
        fun of(key: String): CommunitiesPage = entries.firstOrNull { it.key == key } ?: Discover
    }
}

/** Port of the member's half of ui/src/screens/Communities.tsx: Discover, My communities and Memberships. */
@Composable
fun CommunitiesScreen(
    session: CommunitySession,
    page: CommunitiesPage,
    onBack: () -> Unit,
    onPage: (CommunitiesPage) -> Unit,
    onOpenCommunity: (communityId: String) -> Unit,
) {
    var config by remember { mutableStateOf<CommunitiesConfigDto?>(null) }
    var configError by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) {
        try {
            config = session.config()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            configError = describeError(e, "Communities aren't reachable right now.")
        }
    }
    SubScreen("Communities", onBack) {
        ChipRow(CommunitiesPage.entries, page, { it.label }, onPage, edge = 0.dp)
        configError?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        if (config?.enabled == false) {
            Callout("Communities aren't open on this server yet. Memberships you already have keep working.", CalloutTone.Neutral, R.drawable.ui_info)
        }
        when (page) {
            CommunitiesPage.Discover -> DiscoverPage(session, config, onOpenCommunity)
            CommunitiesPage.Mine -> MinePage(session, onOpenCommunity)
            CommunitiesPage.Memberships -> MembershipsPage(session, onOpenCommunity)
        }
    }
}

// --- Discover ---------------------------------------------------------------------------------------

@Composable
private fun DiscoverPage(session: CommunitySession, config: CommunitiesConfigDto?, onOpen: (String) -> Unit) {
    var query by rememberSaveable { mutableStateOf("") }
    var category by rememberSaveable { mutableStateOf("") }
    var results by remember { mutableStateOf<List<CommunityListingDto>?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(query, category, config?.enabled) {
        if (config?.enabled == false) return@LaunchedEffect
        delay(250)
        try {
            results = session.discover(query, category)
            error = null
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = describeError(e, "Couldn't load communities. Try again.")
        }
    }
    if (config?.enabled == false) return
    SearchBox(query, { query = it.take(80) }, "Search communities", R.drawable.ui_search)
    val categories = listOf("") + (config?.categories ?: emptyList())
    if (categories.size > 1) ChipRow(categories, category, { if (it.isEmpty()) "All" else Communities.categoryLabel(it) }, { category = it }, edge = 0.dp)
    error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    val list = results
    when {
        list == null && error == null -> Muted("Loading…")
        list?.isEmpty() == true -> Muted("No communities match. Unlisted communities only open from an invite link.")
        list != null -> list.forEach { c -> CommunityCard(c) { onOpen(c.id) } }
    }
}

@Composable
private fun CommunityCard(c: CommunityListingDto, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).clickable(role = Role.Button, onClick = onClick).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            CommunityThumb(c.id, c.name, c.hasAvatar, 44)
            Column(Modifier.weight(1f)) {
                Text(c.name, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("@${c.creator.handle}" + if (c.creator.verified) " · verified" else "", color = colors.text3, fontSize = 13.sp)
            }
        }
        val blurb = c.description.ifBlank { c.previewText }
        if (blurb.isNotBlank()) Text(blurb, color = colors.text2, fontSize = 14.sp, maxLines = 3, overflow = TextOverflow.Ellipsis)
        Chips(
            listOfNotNull(
                if (c.kind == "channel") "Channel" else "Group",
                c.price?.let { "${Communities.formatMoney(it.amountMinor, it.currency)}/month" } ?: "Free",
                c.memberCount?.let(Communities::memberCountLabel),
                Communities.categoryLabel(c.category).takeIf { c.category.isNotEmpty() },
            ),
            accentFirstPrice = c.price != null,
        )
    }
}

// --- My communities ------------------------------------------------------------------------------

@Composable
private fun MinePage(session: CommunitySession, onOpen: (String) -> Unit) {
    var list by remember { mutableStateOf<List<MyCommunityDto>?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) {
        try {
            list = session.mine()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = describeError(e, "Couldn't load your communities. Try again.")
        }
    }
    error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    val mine = list
    if (mine == null) {
        if (error == null) Muted("Loading…")
        return
    }
    if (mine.isEmpty()) {
        Muted("You haven't joined any communities yet.")
        return
    }
    val colors = RedvaultTheme.colors
    mine.forEach { c ->
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).clickable(role = Role.Button) { onOpen(c.id) }.padding(14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            CommunityThumb(c.id, c.name, c.hasAvatar, 40)
            Column(Modifier.weight(1f)) {
                Text(c.name, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(
                    (if (c.kind == "channel") "Channel" else "Group") +
                        (if (c.membership.role != "member") " · ${c.membership.role}" else "") +
                        (if (c.status != "published") " · ${c.status}" else ""),
                    color = colors.text3,
                    fontSize = 13.sp,
                )
            }
            Chip(Communities.membershipChip(c.membership.status), accent = c.membership.status == "active", warn = c.membership.status.startsWith("pending"))
        }
    }
}

// --- Memberships ----------------------------------------------------------------------------------

/**
 * The member's own paid memberships: status, renewal and end dates, cancel and resume. No payment-method
 * portal and nothing to buy: this app is distributed through Google Play (see data/api/CommunityApi.kt).
 */
@Composable
private fun MembershipsPage(session: CommunitySession, onOpen: (String) -> Unit) {
    val scope = rememberCoroutineScope()
    var list by remember { mutableStateOf<List<MembershipViewDto>?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var busyId by remember { mutableStateOf<String?>(null) }
    var confirmCancel by remember { mutableStateOf<MembershipViewDto?>(null) }
    var reload by remember { mutableIntStateOf(0) }
    LaunchedEffect(reload) {
        try {
            list = session.memberships().sortedByDescending { it.createdAt }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = describeError(e, "Couldn't load your memberships. Try again.")
        }
    }
    fun act(id: String, action: suspend () -> Unit) {
        busyId = id
        error = null
        scope.launch {
            try {
                action()
                reload++
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                error = describeError(e, "That didn't work. Try again.")
            } finally {
                busyId = null
            }
        }
    }
    confirmCancel?.let { m ->
        AlertDialog(
            onDismissRequest = { confirmCancel = null },
            title = { Text("Cancel your ${m.community.name} membership?") },
            text = { Text("You won't be charged again, and you keep access until ${day(m.accessUntil)}.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmCancel = null
                    act(m.id) { session.cancelMembership(m.id) }
                }) { Text("Cancel membership", color = RedvaultTheme.colors.danger) }
            },
            dismissButton = { TextButton(onClick = { confirmCancel = null }) { Text("Keep it") } },
        )
    }
    error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
    val memberships = list
    if (memberships == null) {
        if (error == null) Muted("Loading…")
        return
    }
    if (memberships.isEmpty()) {
        Muted("No paid memberships.")
        return
    }
    val colors = RedvaultTheme.colors
    memberships.forEach { m ->
        Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(m.community.name, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            Text("@${m.creatorHandle ?: "creator"} · code ${m.community.code}", color = colors.text3, fontSize = 13.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Chip(
                    Communities.entitlementLabel(m.entitlementStatus),
                    accent = m.entitlementStatus == "active",
                    warn = m.entitlementStatus in setOf("grace_period", "pending", "cancel_scheduled"),
                    danger = m.entitlementStatus in setOf("refunded", "disputed", "suspended"),
                )
                m.price?.let { Chip("${Communities.formatMoney(it.amountMinor, it.currency)}/month") }
            }
            val line = buildString {
                m.renewsOn?.let { append("Renews on ${day(it)}. ") }
                if (m.entitlementStatus == "cancel_scheduled") append("Access until ${day(m.accessUntil)}; you won't be charged again. ")
                if (m.entitlementStatus == "grace_period") append("The last renewal payment didn't go through. Access lasts until ${day(m.graceUntil)}. ")
                if (m.entitlementStatus == "expired" && m.endedAt != null) append("Ended on ${day(m.endedAt)}. ")
            }.trim()
            if (line.isNotEmpty()) Text(line, color = colors.text2, fontSize = 13.sp)
            val wide = Modifier.fillMaxWidth()
            PillButton("View community", { onOpen(m.community.id) }, wide, style = PillStyle.Secondary, height = 44.dp)
            if ((m.entitlementStatus == "active" || m.entitlementStatus == "grace_period") && !m.cancelAtPeriodEnd) {
                PillButton("Cancel membership", { confirmCancel = m }, wide, style = PillStyle.Danger, height = 44.dp, enabled = busyId != m.id)
            }
            if (m.entitlementStatus == "cancel_scheduled" && m.billingStatus != "canceled") {
                PillButton("Keep membership", { act(m.id) { session.resumeMembership(m.id) } }, wide, style = PillStyle.Secondary, height = 44.dp, busy = busyId == m.id)
            }
        }
    }
    Muted("Payment methods and receipts are managed in RedVault on the web or desktop.")
}

// --- One community -----------------------------------------------------------------------------------

/**
 * Port of CommunityPage in ui/src/screens/Communities.tsx for a member's phone: the listing, joining a free
 * community, opening its chat, leaving, and reporting it. A paid community shows its price and that it
 * can't be bought here; nothing on this screen starts a purchase.
 */
@Composable
fun CommunityScreen(
    session: CommunitySession,
    communityId: String,
    onBack: () -> Unit,
    onOpenChat: (conversationId: String) -> Unit,
    /** The membership changed (joined, enrolled, left): the chat list should reload. */
    onMembershipChanged: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var community by remember { mutableStateOf<CommunityPageDto?>(null) }
    var missing by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var message by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var confirmLeave by remember { mutableStateOf(false) }
    var reporting by remember { mutableStateOf(false) }
    var reload by remember { mutableIntStateOf(0) }

    LaunchedEffect(communityId, reload) {
        try {
            val page = session.page(communityId)
            if (page == null) missing = true else community = page
            error = null
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            error = describeError(e, "Couldn't load this community. Try again.")
        }
    }
    // Back in the app (say, after subscribing on the web): look again.
    val lifecycleOwner = LocalLifecycleOwner.current
    LaunchedEffect(lifecycleOwner) {
        var first = true
        lifecycleOwner.repeatOnLifecycle(Lifecycle.State.RESUMED) {
            if (!first) reload++
            first = false
        }
    }
    // Waiting for the key: look again every few seconds, so the page (and the chat) open once it arrives.
    val status = community?.viewer?.membership?.status
    LaunchedEffect(status) {
        if (status == "active") {
            // The key arrived: "waiting for the key" no longer applies.
            if (message == JOINED_WAITING) message = null
            onMembershipChanged()
        }
        if (status != "pending_enrollment") return@LaunchedEffect
        while (true) {
            delay(5_000)
            reload++
        }
    }

    fun act(success: String?, action: suspend () -> Unit) {
        busy = true
        message = null
        scope.launch {
            try {
                action()
                if (success != null) message = success
                reload++
                onMembershipChanged()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                message = describeError(e, "That didn't work. Try again.")
            } finally {
                busy = false
            }
        }
    }

    SubScreen(community?.name ?: "Community", onBack) {
        if (missing) {
            Muted("This community doesn't exist, or you need an invite link to see it.")
            return@SubScreen
        }
        error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        val c = community
        if (c == null) {
            if (error == null) Muted("Loading…")
            return@SubScreen
        }
        val membership = c.viewer.membership
        val subscription = c.viewer.subscription
        val isActiveMember = membership?.status == "active"
        val closedForJoining = c.status != "published" || c.newMembersPaused

        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            CommunityThumb(c.id, c.name, c.hasAvatar, 56)
            Text(
                "by ${c.creator.displayName.ifBlank { c.creator.handle }} (@${c.creator.handle})" + if (c.creator.verified) " · verified creator" else "",
                color = colors.text3,
                fontSize = 14.sp,
                modifier = Modifier.weight(1f),
            )
        }
        Chips(
            listOfNotNull(
                if (c.kind == "channel") "Channel: the creator posts, members read" else "Group: members talk together",
                Communities.categoryLabel(c.category).takeIf { c.category.isNotEmpty() },
                c.memberCount?.let(Communities::memberCountLabel),
                c.status.takeIf { it != "published" },
                "Code ${c.code}",
            ),
        )
        if (c.description.isNotBlank()) Text(c.description, color = colors.text2, fontSize = 15.sp)

        if (isActiveMember) {
            Panel(accent = true) {
                Text(
                    "You're a member" + (if (membership.role != "member") " (${membership.role})" else "") + ".",
                    color = colors.text,
                    fontWeight = FontWeight.SemiBold,
                )
                if (subscription != null) {
                    val renewal = when {
                        subscription.entitlementStatus == "cancel_scheduled" -> "Your membership ends on ${day(subscription.accessUntil)}."
                        subscription.renewsOn != null -> "Renews on ${day(subscription.renewsOn)}."
                        else -> null
                    }
                    renewal?.let { Text(it, color = colors.text3, fontSize = 13.sp) }
                }
                if (!membership.canPost) Text("Only the creator and staff post here; you read.", color = colors.text3, fontSize = 13.sp)
                c.conversationId?.let { conversationId ->
                    PillButton(if (c.kind == "channel") "Open channel" else "Open chat", { onOpenChat(conversationId) }, Modifier.fillMaxWidth(), height = 44.dp)
                }
                if (membership.role != "owner") {
                    PillButton("Leave", { confirmLeave = true }, Modifier.fillMaxWidth(), style = PillStyle.Danger, height = 44.dp, enabled = !busy)
                }
            }
        }

        membership?.status?.let(Communities::membershipNotice)?.let { Callout(it, CalloutTone.Neutral, R.drawable.ui_clock) }
        if (subscription?.entitlementStatus == "pending") Callout("Your payment is being confirmed. This page updates once it's done.", CalloutTone.Neutral, R.drawable.ui_clock)

        val canAskToJoin = !isActiveMember && membership?.status != "banned" && membership?.status?.startsWith("pending") != true
        if (canAskToJoin) {
            Panel {
                val price = c.price
                when {
                    c.accessType == "free" -> {
                        Text("Free to join", color = colors.text, fontWeight = FontWeight.SemiBold)
                        if (closedForJoining) Text("Not accepting new members right now.", color = colors.text3, fontSize = 13.sp)
                        PillButton(
                            "Join",
                            { act(JOINED_WAITING) { session.join(c.id) } },
                            Modifier.fillMaxWidth(),
                            height = 44.dp,
                            enabled = !closedForJoining,
                            busy = busy,
                        )
                    }
                    subscription != null && (subscription.entitlementStatus == "cancel_scheduled" || subscription.entitlementStatus == "active") -> {
                        Text("You still have a paid membership until ${day(subscription.accessUntil)}.", color = colors.text, fontWeight = FontWeight.SemiBold)
                        PillButton("Rejoin", { act("Welcome back.") { session.join(c.id) } }, Modifier.fillMaxWidth(), height = 44.dp, busy = busy)
                    }
                    price != null -> {
                        Text(
                            "${Communities.formatMoney(price.amountMinor, price.currency)} / month",
                            color = colors.text,
                            fontSize = 20.sp,
                            fontWeight = FontWeight.Bold,
                        )
                        Callout(Communities.NOT_SOLD_HERE, CalloutTone.Neutral, R.drawable.ui_info)
                    }
                    else -> Text("This community has no price yet.", color = colors.text3, fontSize = 13.sp)
                }
            }
        }

        message?.let { Callout(it, CalloutTone.Neutral, R.drawable.ui_info) }

        if (c.previewText.isNotBlank()) {
            SectionLabel("Preview")
            Text(c.previewText, color = colors.text2, fontSize = 15.sp, fontStyle = FontStyle.Italic)
        }
        if (c.rules.isNotBlank()) {
            SectionLabel("Rules")
            Text(c.rules, color = colors.text2, fontSize = 15.sp)
        }
        SectionLabel("Privacy")
        Text(
            "Messages, images, voice notes and files inside are end-to-end encrypted: neither RedVault nor Stripe can read them. " +
                "RedVault does see this public listing, who is a member, and the state of each membership's billing. When someone " +
                "leaves, the members' apps switch to a new key they never get.",
            color = colors.text3,
            fontSize = 13.sp,
        )
        TextButton(onClick = { reporting = true }) { Text("Report this community", color = colors.danger) }

        if (confirmLeave) {
            val note = if (subscription != null) " Your subscription stops renewing; you won't be charged again. You can come back until the paid period ends." else ""
            AlertDialog(
                onDismissRequest = { confirmLeave = false },
                title = { Text("Leave ${c.name}?") },
                text = { Text("You'll stop seeing its messages.$note") },
                confirmButton = {
                    TextButton(onClick = {
                        confirmLeave = false
                        act("You left the community.") { session.leave(c.id) }
                    }) { Text("Leave", color = colors.danger) }
                },
                dismissButton = { TextButton(onClick = { confirmLeave = false }) { Text("Stay") } },
            )
        }
        if (reporting) {
            CommunityReportDialog(
                messageText = null,
                onSubmit = { category, details, _ -> session.reportCommunity(c.id, category, details) },
                onDismiss = { reporting = false },
            )
        }
    }
}

// --- Reporting ---------------------------------------------------------------------------------------

/**
 * Port of ui/src/screens/CommunityReportDialog.tsx. Reporting never reads anything for you: for a message
 * ([messageText] set), the reporter chooses to disclose its decrypted text, and is told exactly who will
 * see it. Unticked, only the category, details and the message's sender go to the reviewers.
 */
@Composable
fun CommunityReportDialog(
    messageText: String?,
    onSubmit: suspend (category: String, details: String, shareText: Boolean) -> Unit,
    onDismiss: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var category by remember { mutableStateOf(Communities.REPORT_CATEGORIES.first()) }
    var choosing by remember { mutableStateOf(false) }
    var details by remember { mutableStateOf("") }
    var share by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var sent by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val toRedVault = Communities.reportGoesToRedVault(category.value, aboutCommunity = messageText == null)
    AlertDialog(
        onDismissRequest = { if (!busy) onDismiss() },
        containerColor = colors.surface,
        title = { Text(if (sent) "Report sent" else if (messageText == null) "Report this community" else "Report this message", color = colors.text) },
        text = {
            if (sent) {
                Text("Thank you. ${if (toRedVault) "RedVault's reviewers" else "The community's moderators"} will look at it.", color = colors.text2)
                return@AlertDialog
            }
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("What's wrong?", color = colors.text, fontWeight = FontWeight.SemiBold)
                Box {
                    Text(
                        "${category.label} ▾",
                        color = colors.text,
                        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(colors.surface2)
                            .clickable(role = Role.DropdownList) { choosing = true }.padding(horizontal = 14.dp, vertical = 12.dp),
                    )
                    DropdownMenu(expanded = choosing, onDismissRequest = { choosing = false }) {
                        Communities.REPORT_CATEGORIES.forEach { option ->
                            DropdownMenuItem(text = { Text(option.label) }, onClick = {
                                category = option
                                choosing = false
                            })
                        }
                    }
                }
                OutlinedTextField(
                    value = details,
                    onValueChange = { if (it.length <= 2000) details = it },
                    placeholder = { Text("Details (optional)") },
                    minLines = 2,
                    modifier = Modifier.fillMaxWidth(),
                )
                if (messageText != null) {
                    Text(
                        messageText.take(600),
                        color = colors.text2,
                        fontSize = 14.sp,
                        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(colors.surface2).padding(12.dp),
                    )
                    Row(
                        Modifier.fillMaxWidth().clickable(role = Role.Checkbox) { share = !share },
                        verticalAlignment = Alignment.Top,
                    ) {
                        Checkbox(checked = share, onCheckedChange = { share = it }, colors = CheckboxDefaults.colors(checkedColor = colors.accent))
                        Text(Communities.disclosureText(toRedVault), color = colors.text2, fontSize = 13.sp, modifier = Modifier.padding(top = 12.dp))
                    }
                }
                Text(
                    if (toRedVault) "This goes to RedVault's own reviewers, not to the community's moderators."
                    else "This goes to the community's moderators. Serious categories go to RedVault instead.",
                    color = colors.text3,
                    fontSize = 12.sp,
                )
                error?.let { Text(it, color = colors.danger, fontSize = 13.sp) }
            }
        },
        confirmButton = {
            if (sent) {
                TextButton(onClick = onDismiss) { Text("Done") }
            } else {
                TextButton(enabled = !busy, onClick = {
                    busy = true
                    error = null
                    scope.launch {
                        try {
                            onSubmit(category.value, details, messageText != null && share)
                            sent = true
                        } catch (e: CancellationException) {
                            throw e
                        } catch (e: Exception) {
                            error = describeError(e, "Couldn't send the report. Try again.")
                        } finally {
                            busy = false
                        }
                    }
                }) { Text(if (busy) "Sending…" else "Send report", color = colors.danger) }
            }
        },
        dismissButton = { if (!sent) TextButton(enabled = !busy, onClick = onDismiss) { Text("Cancel") } },
    )
}

// --- Bits ---------------------------------------------------------------------------------------------

/** A community's picture (served only to someone who can see it), or its initials. */
@Composable
fun CommunityThumb(communityId: String, name: String, hasAvatar: Boolean, size: Int) {
    if (hasAvatar) Avatar(avatarUrl = "/api/communities/$communityId/avatar", seed = name, size = size.dp)
    else GroupAvatar(name, size = size.dp)
}

@Composable
private fun Panel(accent: Boolean = false, content: @Composable () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(if (accent) colors.accentSoft else colors.surface).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) { content() }
}

@Composable
private fun Chips(labels: List<String>, accentFirstPrice: Boolean = false) {
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        labels.forEachIndexed { i, label -> Chip(label, accent = accentFirstPrice && i == 1) }
    }
}

@Composable
private fun Chip(text: String, accent: Boolean = false, warn: Boolean = false, danger: Boolean = false) {
    val colors = RedvaultTheme.colors
    val (bg, ink) = when {
        accent -> colors.accentSoft to colors.accent
        warn -> colors.warnSoft to colors.warn
        danger -> colors.dangerSoft to colors.danger
        else -> colors.surface2 to colors.text2
    }
    Text(
        text,
        color = ink,
        fontSize = 12.sp,
        fontWeight = FontWeight.SemiBold,
        maxLines = 1,
        modifier = Modifier.heightIn(min = 24.dp).clip(RoundedCornerShape(12.dp)).background(bg).padding(horizontal = 10.dp, vertical = 4.dp),
    )
}

@Composable
private fun Muted(text: String) {
    Text(text, color = RedvaultTheme.colors.text3, fontSize = 14.sp, modifier = Modifier.padding(vertical = 8.dp))
}

private const val JOINED_WAITING = "Joined. Waiting for the community's key…"

/** "2026-11-18T12:00:00.000Z" as a date in the phone's locale; "—" for none. */
private fun day(iso: String?): String =
    iso?.let { runCatching { TimeFormat.longDate(Instant.parse(it).toEpochMilli()) }.getOrNull() } ?: "—"
