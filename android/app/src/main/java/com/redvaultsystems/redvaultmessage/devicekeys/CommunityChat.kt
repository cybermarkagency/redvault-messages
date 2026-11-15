// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.File

/**
 * Part C2 (docs/signal-integration/11-part-c2-communities.md): paid communities as full Signal groups on Android.
 *
 * - **`group` community:** the signed roster lists every entitled member, and only owner and staff devices issue it
 *   (the server's rule). Each message is encrypted once under the sender's Signal sender key and fanned out to every
 *   listed device of the roster's members.
 * - **`channel` community:** the roster lists only the owner and staff (the senders). The readers are the entitled
 *   members the server names (`GET /communities/:id/recipients`); a staff device shares its sender key with exactly
 *   their devices (`groupSignalShare` with `audience: "recipients"`). Members never see each other. A staff device
 *   rotates its sender key whenever anyone leaves by issuing the next roster epoch (same staff): every sender key is
 *   new at a new epoch, and a removed member never gets the new ones.
 * - Sends go out at most [ChatLimits.MAX_RECIPIENTS] devices per request, in order. A request that fails for a
 *   network reason is tried again; a 409 `recipient_not_entitled` drops the named accounts and that request is tried
 *   once more without them.
 * - Owner and staff devices run the membership tasks ([runMembershipTasks]): admit waiting members, issue rosters,
 *   rotate. Nothing about who is in a community is stored on this phone except the task cursor (a number).
 * - The server keeps no history: new members and new devices see only what arrives after they joined or were linked
 *   ([BEFORE_YOU_JOINED]).
 *
 * Never logs content or keys.
 */

/** Shown at the top of a community chat (Part C2: the server keeps no history). */
const val BEFORE_YOU_JOINED = "Messages from before you joined aren't available"

/** [BEFORE_YOU_JOINED] for a community chat, null for any other chat. */
fun beforeYouJoinedNotice(isCommunity: Boolean): String? = if (isCommunity) BEFORE_YOU_JOINED else null

/** A community conversation as the device-key path sees it. [staff]: this account is owner, admin or moderator. */
data class CommunityContext(
    val communityId: String,
    val conversationId: String,
    /** "group" or "channel". */
    val kind: String,
    val staff: Boolean,
) {
    val isChannel: Boolean get() = kind == "channel"

    companion object {
        fun isStaffRole(role: String?): Boolean = role == "owner" || role == "admin" || role == "moderator"
    }
}

/** One community where this account is active staff, from `GET /communities/membership-tasks`. */
data class MembershipTaskSummary(val communityId: String, val conversationId: String, val kind: String, val latestSeq: Long, val waiting: Int)

data class MembershipEvent(val seq: Long, val kind: String, val userId: String)

/** One page of `GET /communities/:id/membership-tasks`. */
data class MembershipTasks(
    val communityId: String,
    val conversationId: String,
    val kind: String,
    val cursor: Long,
    val hasMore: Boolean,
    val events: List<MembershipEvent>,
    val waiting: List<String>,
    val lapsed: List<String>,
)

/** The community routes the device-key path uses (server/src/routes/communityRoutes.ts, Part C2). A fake in tests. */
interface CommunityDeviceApi {
    /** Owner and staff: the entitled members' accounts, staff included. */
    suspend fun recipients(communityId: String): List<String>

    /** Moderators and up: the accounts of the active owner and staff. */
    suspend fun staff(communityId: String): List<String>

    suspend fun membershipTaskSummaries(): List<MembershipTaskSummary>
    suspend fun membershipTasks(communityId: String, after: Long, limit: Int = 200): MembershipTasks

    /** Moves a `pending_enrollment` member to active. */
    suspend fun admit(communityId: String, userId: String)
}

/** What a community send needs besides the chat: who this is, and the routes for its recipients. */
class CommunityRoute(val context: CommunityContext, val api: CommunityDeviceApi?)

private fun JsonObject.s(name: String): String? = this[name]?.jsonPrimitive?.contentOrNull

private fun notReady() =
    EngineException("community_not_ready", "This community's member list isn't set up on device keys yet. The owner's app does that; try again in a moment.")

private fun notInRoster() =
    EngineException("community_not_ready", "You're not in this community's member list yet. The owner's app adds you; try again in a moment.")

/** The accounts in the conversation's latest roster this device verified, or null when it has none yet. */
internal suspend fun rosterMembers(device: V3Device, conversationId: String): List<String>? =
    verifyRosters(device, conversationId)?.array("members")?.map { it.jsonPrimitive.content }

/** Every listed device of [users] in [conversationId], each list verified by the engine (which remembers it). */
internal suspend fun communityDevices(device: V3Device, conversationId: String, users: Collection<String>): List<JsonObject> {
    val wanted = users.mapTo(LinkedHashSet()) { it.lowercase() }
    val out = ArrayList<JsonObject>()
    val seen = HashSet<String>()
    // One request for everyone the server lets this account see (a channel's staff see every entitled member).
    val lists = runCatching { device.api.conversationDeviceLists(conversationId) }.getOrElse { if (it is CancellationException) throw it; emptyList() }
    for ((userId, view) in lists) {
        val user = userId.lowercase()
        if (user !in wanted || !seen.add(user)) continue
        val list = view.deviceList ?: continue
        val verified = device.op("verifyDeviceList", "deviceList" to list)
        if (!verified.ok) continue // That account's list doesn't check out: it isn't reached, nobody else is held up.
        out += verified.array("devices").map { it.jsonObject }
    }
    // Anyone the conversation list didn't include: their own list (GET users/:id/device-list with the conversation).
    for (user in wanted) {
        if (user in seen) continue
        seen += user
        try {
            out += verifiedDevices(device, user, conversationId).devices
        } catch (e: CancellationException) {
            throw e
        } catch (_: EngineException) {
        } catch (e: ApiException) {
            if (e.status == 401 || e.status == 403) throw e
        }
    }
    return out
}

/** The accounts a 409 `recipient_not_entitled` (or a roster refusal) names. */
internal fun refusedAccounts(e: ApiException): Set<String> =
    (e.details?.get("userIds") as? JsonArray)?.mapNotNull { it.jsonPrimitive.contentOrNull?.lowercase() }?.toSet().orEmpty()

private fun transient(e: ApiException) = e.status == 0 || e.status == 429 || e.status >= 500

/** How long to wait before the [attempt]th try of a failed batch. Tests set it to 0. */
@Volatile internal var batchRetryDelayMs: Long = 400

/**
 * Posts [items] (each addressed to [userOf]) in requests of at most [size], in order. A request that fails for a
 * network reason is tried again (up to 3 times); one refused with 409 `recipient_not_entitled` loses the accounts it
 * names and is tried once more. Returns the accounts dropped that way.
 */
internal suspend fun <T> postInBatches(items: List<T>, userOf: (T) -> String, size: Int = ChatLimits.MAX_RECIPIENTS, post: suspend (List<T>) -> Unit): Set<String> {
    val dropped = HashSet<String>()
    for (chunk in items.chunked(size)) {
        var batch = chunk.filter { userOf(it).lowercase() !in dropped }
        var tries = 0
        var refusedOnce = false
        while (batch.isNotEmpty()) {
            tries++
            try {
                post(batch)
                break
            } catch (e: ApiException) {
                when {
                    e.code == "recipient_not_entitled" && !refusedOnce -> {
                        val names = refusedAccounts(e)
                        if (names.isEmpty()) throw e
                        refusedOnce = true
                        dropped += names
                        batch = batch.filter { userOf(it).lowercase() !in names }
                    }
                    transient(e) && tries < 3 -> delay(batchRetryDelayMs * tries)
                    else -> throw e
                }
            }
        }
    }
    return dropped
}

/**
 * Makes the roster this device's account issues for the conversation match [members] (the issuer included). With
 * [rotate], the next epoch is issued even when nothing changed: that's a channel's sender-key rotation. Accounts the
 * server refuses (`roster_member_not_entitled`, `roster_member_not_staff`) are dropped and the roster is signed again
 * once. Another staff device's roster first (`stale_roster`) is taken as it is: true when this device issued one.
 */
internal suspend fun issueCommunityRoster(device: V3Device, conversationId: String, members: Collection<String>, rotate: Boolean = false): Boolean {
    var wanted = (members.map { it.lowercase() } + device.userId.lowercase()).distinct().sorted()
    for (attempt in 1..3) {
        val verified = verifyRosters(device, conversationId)
        val current = verified?.array("members")?.map { it.jsonPrimitive.content }
        if (verified != null && current == wanted && !rotate) return false
        val members0 = JsonArray(wanted.map(::JsonPrimitive))
        val before = device.snapshot()
        val made = if (verified == null) device.must("rosterCreate", "groupId" to conversationId, "members" to members0)
        else device.must("rosterUpdate", "groupId" to conversationId, "members" to members0)
        val after = device.snapshot()
        try {
            device.api.postRoster(conversationId, made.obj("roster"))
            return true
        } catch (e: ApiException) {
            // The engine keeps what it signed; the server didn't take it, so it mustn't stay here. Undone only when
            // nothing else changed this device's state meanwhile (that would be lost too).
            if (device.snapshot() == after) device.rollback(before)
            when (e.code) {
                "roster_member_not_entitled", "roster_member_not_staff" -> {
                    val names = refusedAccounts(e)
                    if (names.isEmpty() || attempt == 3) throw e
                    wanted = wanted.filter { it !in names || it == device.userId.lowercase() }
                }
                // Another staff device issued the next roster first: that one stands (and was a rotation too).
                "stale_roster" -> return false
                else -> throw e
            }
        }
    }
    return false
}

/** Who a community's roster lists: every entitled member (group) or the owner and staff (channel). */
private suspend fun rosterTarget(route: CommunityRoute): List<String> {
    val api = route.api ?: throw EngineException("community_not_ready", "Community tasks aren't available on this phone right now.")
    val ctx = route.context
    return if (ctx.isChannel) api.staff(ctx.communityId) else api.recipients(ctx.communityId)
}

/** The roster to send under: verified, with this account in it; owner and staff issue it when it's missing. */
private suspend fun ensureCommunityRoster(device: V3Device, route: CommunityRoute): List<String> {
    val conversation = route.context.conversationId
    val members = rosterMembers(device, conversation)
    if (members != null && device.userId in members) return members
    if (!route.context.staff) throw if (members == null) notReady() else notInRoster()
    issueCommunityRoster(device, conversation, rosterTarget(route))
    return rosterMembers(device, conversation)?.takeIf { device.userId in it } ?: throw notInRoster()
}

/** Accounts each community dropped after a `recipient_not_entitled` (memory only): the owner's tasks pick them up. */
internal val droppedRecipients = java.util.concurrent.ConcurrentHashMap<String, MutableSet<String>>()

/**
 * Shares this device's sender key for the conversation with [devices] (those that lack it) and posts the key
 * messages, batched. Devices the engine can't reach (no Signal keys, a changed identity) are skipped: one member's
 * device never stops a community's message. Returns the devices that have the key.
 */
private suspend fun shareSenderKey(device: V3Device, conversation: String, devices: List<JsonObject>, audience: String?): List<JsonObject> {
    // Even with nobody to share with, this makes the sender key for the current epoch, which encrypting needs.
    fun entries(list: List<Pair<JsonObject, JsonObject?>>) = buildJsonArray {
        for ((identity, claimed) in list) add(buildJsonObject {
            put("identity", identity)
            if (claimed != null) put("signalBundle", buildJsonObject {
                put("certificate", claimed["certificate"] ?: JsonNull)
                put("bundle", claimed["bundle"] ?: JsonNull)
            })
        })
    }
    val results = device.must("groupSignalShare", "groupId" to conversation, "audience" to audience, "recipients" to entries(devices.map { it to null }))
        .array("results").map { it.jsonObject }.toMutableList()
    val need = results.indices.filter { results[it].s("error") == "no_signal_session" }
    if (need.isNotEmpty()) {
        val claimed = ArrayList<JsonObject>()
        need.chunked(ChatLimits.MAX_RECIPIENTS).forEach { part ->
            claimed += device.api.signalClaim(conversation, part.map { devices[it].s("userId")!! to devices[it].s("deviceId")!! })
        }
        val again = device.must("groupSignalShare", "groupId" to conversation, "audience" to audience, "recipients" to entries(need.mapIndexed { k, i -> devices[i] to claimed[k] }))
            .array("results")
        need.forEachIndexed { k, i -> results[i] = again[k].jsonObject }
    }
    val keyMessages = ArrayList<Triple<String, String, String>>()
    val reached = ArrayList<JsonObject>()
    results.forEachIndexed { i, r ->
        if (r.s("error") != null) return@forEachIndexed
        reached += devices[i]
        r["message"]?.let { keyMessages += Triple(devices[i].s("userId")!!, devices[i].s("deviceId")!!, it.toString()) }
    }
    val dropped = postInBatches(keyMessages, { it.first }) { device.api.sendMessages(device.deviceId, conversation, it) }
    if (dropped.isNotEmpty()) droppedRecipients.getOrPut(conversation) { java.util.concurrent.ConcurrentHashMap.newKeySet() } += dropped
    return reached.filter { (it.s("userId") ?: "").lowercase() !in dropped }
}

/**
 * One community send of [envelope] (a Part A envelope). [opOnly]: a reaction, vote, edit, delete or other change,
 * which a channel's members may send to the owner and staff alone (pairwise); posts from them are refused.
 */
internal suspend fun deliverCommunity(device: V3Device, history: ChatHistory, envelope: String, record: ChatRecord, route: CommunityRoute, keepFailed: Boolean, opOnly: Boolean): SendOutcome {
    val ctx = route.context
    val conversation = record.conversationId
    var attempt = 0
    while (true) try {
        attempt++
        if (!engineHasSignal(device)) throw EngineException("signal_required", "Communities need Signal sessions on this phone; update the app.")
        if (ctx.isChannel && !ctx.staff) {
            if (!opOnly) throw EngineException("cannot_post", "Only the owner and staff post in this channel.")
            // To the owner and staff (the roster) and this account's other devices, pairwise. Never to other members.
            val staff = rosterMembers(device, conversation) ?: throw notReady()
            val targets = communityDevices(device, conversation, (staff + device.userId).distinct()).filter { it.s("deviceId") != device.deviceId }
            val messages = encryptToDevices(device, conversation, envelope, targets)
            postInBatches(messages, { it.first }) { device.api.sendMessages(device.deviceId, conversation, it) }
        } else {
            val roster = ensureCommunityRoster(device, route)
            val readers = if (ctx.isChannel) {
                val api = route.api ?: throw EngineException("community_not_ready", "Community tasks aren't available on this phone right now.")
                (api.recipients(ctx.communityId).map { it.lowercase() } + roster).distinct()
            } else roster
            val devices = communityDevices(device, conversation, readers).filter { it.s("deviceId") != device.deviceId }
            val reached = shareSenderKey(device, conversation, devices, if (ctx.isChannel) "recipients" else null)
            val sealed = device.must("groupSignalEncrypt", "groupId" to conversation, "plaintext" to envelope).obj("message").toString()
            val dropped = postInBatches(reached.map { it.s("userId")!! to it.s("deviceId")!! }, { it.first }) {
                device.api.sendGroupMessage(device.deviceId, conversation, sealed, it)
            }
            if (dropped.isNotEmpty()) droppedRecipients.getOrPut(conversation) { java.util.concurrent.ConcurrentHashMap.newKeySet() } += dropped
        }
        break
    } catch (e: Throwable) {
        // A device list changed meanwhile: fetched again and sent once more (a device that already has it drops the
        // copy as a duplicate).
        if (e is ApiException && e.code == "device_list_stale" && attempt == 1) continue
        // Kept as failed for Retry, except a post this account may never make here.
        val never = e is EngineException && e.code == "cannot_post"
        if (keepFailed && e !is CancellationException && !never) history.put(record.copy(status = "failed"), replace = true)
        throw e
    }
    history.pin(conversation)
    history.put(record, replace = true)
    return SendOutcome.Sent(record)
}

/**
 * A channel's sender key for [users]' devices now (new members on a join, everyone after a rotation), so they can
 * read the next post without waiting for it to carry the key. Returns how many devices have it.
 */
internal suspend fun shareChannelKey(device: V3Device, conversationId: String, users: Collection<String>): Int {
    if (users.isEmpty() || !engineHasSignal(device)) return 0
    val devices = communityDevices(device, conversationId, users).filter { it.s("deviceId") != device.deviceId }
    return shareSenderKey(device, conversationId, devices, "recipients").size
}

// --- Membership tasks (owner and staff devices) ---

/** Each community's task cursor, in a small file beside the chat pins: community ids and numbers only. */
class CommunityCursors(private val file: File) {
    private val values: MutableMap<String, Long> = runCatching {
        file.takeIf { it.exists() }?.readLines()?.mapNotNull { line ->
            val (id, seq) = line.split('=', limit = 2).takeIf { it.size == 2 } ?: return@mapNotNull null
            seq.toLongOrNull()?.let { id to it }
        }?.toMap()
    }.getOrNull().orEmpty().toMutableMap()

    @Synchronized fun get(communityId: String): Long? = values[communityId]

    @Synchronized fun put(communityId: String, seq: Long) {
        if (values[communityId] == seq) return
        values[communityId] = seq
        file.parentFile?.mkdirs()
        file.writeText(values.entries.joinToString("\n") { "${it.key}=${it.value}" })
    }
}

/** What one round of membership tasks did (counts only). */
data class MembershipTaskReport(val admitted: Int = 0, val rosters: Int = 0, val rotations: Int = 0, val shared: Int = 0, val failed: Int = 0) {
    operator fun plus(o: MembershipTaskReport) =
        MembershipTaskReport(admitted + o.admitted, rosters + o.rosters, rotations + o.rotations, shared + o.shared, failed + o.failed)
    val changed: Boolean get() = admitted + rosters + rotations > 0
}

/**
 * One round of the owner and staff upkeep (Part C2, "What the app does with them"), for every community the server
 * lists for this account. Per community:
 * - admit each waiting member;
 * - `group`: issue the next roster (the entitled members, which already leaves out who left or lapsed);
 * - `channel`: issue the staff roster on a role change; on a leave (or a lapsed member) rotate the sender key by
 *   issuing the next epoch and share the new key with the remaining readers; on joins share the key with the newcomers.
 * The cursor is saved only once a community's tasks are done, so a failure is tried again next round.
 */
suspend fun runMembershipTasks(device: V3Device, api: CommunityDeviceApi, cursors: CommunityCursors): MembershipTaskReport {
    var report = MembershipTaskReport()
    for (summary in api.membershipTaskSummaries()) {
        val saved = cursors.get(summary.communityId)
        val conversation = summary.conversationId.lowercase()
        val dropped = droppedRecipients[conversation].orEmpty()
        if (saved == summary.latestSeq && summary.waiting == 0 && dropped.isEmpty() && rosterMembers(device, conversation)?.contains(device.userId) == true) continue
        try {
            report += communityTasks(device, api, summary, saved ?: 0, cursors)
            droppedRecipients.remove(conversation)
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            report += MembershipTaskReport(failed = 1)
        } catch (_: EngineException) {
            report += MembershipTaskReport(failed = 1)
        }
    }
    return report
}

private suspend fun communityTasks(device: V3Device, api: CommunityDeviceApi, summary: MembershipTaskSummary, from: Long, cursors: CommunityCursors): MembershipTaskReport {
    val conversation = summary.conversationId.lowercase()
    val route = CommunityRoute(CommunityContext(summary.communityId, conversation, summary.kind, staff = true), api)
    var after = from
    val joins = LinkedHashSet<String>()
    var leaves = false
    var roles = false
    var waiting: List<String> = emptyList()
    var lapsed: List<String> = emptyList()
    do {
        val page = api.membershipTasks(summary.communityId, after)
        for (e in page.events) when (e.kind) {
            "join" -> joins += e.userId.lowercase()
            "leave" -> {
                leaves = true
                joins -= e.userId.lowercase()
            }
            "role" -> roles = true
        }
        waiting = page.waiting
        lapsed = page.lapsed
        after = page.cursor
    } while (page.hasMore)

    var admitted = 0
    for (user in waiting) {
        try {
            api.admit(summary.communityId, user)
            admitted++
            joins += user.lowercase()
        } catch (e: ApiException) {
            // Another staff device got there first, their payment lapsed, or the community is full: next round.
            if (e.status == 401 || transient(e)) throw e
        }
    }

    val rosterBefore = rosterMembers(device, conversation)
    var rosters = 0
    var rotations = 0
    var shared = 0
    if (summary.kind == "channel") {
        val staffNow = rosterTarget(route)
        var issued = false
        if (rosterBefore == null || roles || device.userId !in rosterBefore) {
            issued = issueCommunityRoster(device, conversation, staffNow)
            if (issued) rosters++
        }
        if (leaves || lapsed.isNotEmpty() || droppedRecipients[conversation].orEmpty().isNotEmpty()) {
            // A new epoch means a new sender key for every staff device; the removed member never gets them. A roster
            // this round already issued is that new epoch.
            if (issued || issueCommunityRoster(device, conversation, rosterMembers(device, conversation) ?: staffNow, rotate = true)) rotations++
            val lapsedSet = lapsed.mapTo(HashSet()) { it.lowercase() }
            shared += shareChannelKey(device, conversation, api.recipients(summary.communityId).map { it.lowercase() }.filter { it !in lapsedSet })
        } else if (joins.isNotEmpty()) {
            shared += shareChannelKey(device, conversation, joins)
        }
    } else {
        val lapsedSet = lapsed.mapTo(HashSet()) { it.lowercase() }
        val target = api.recipients(summary.communityId).map { it.lowercase() }.filter { it !in lapsedSet }
        if (issueCommunityRoster(device, conversation, target)) rosters++
    }
    cursors.put(summary.communityId, after)
    return MembershipTaskReport(admitted, rosters, rotations, shared)
}
