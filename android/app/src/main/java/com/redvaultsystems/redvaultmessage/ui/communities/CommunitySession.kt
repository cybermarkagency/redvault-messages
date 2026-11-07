package com.redvaultsystems.redvaultmessage.ui.communities

import com.redvaultsystems.redvaultmessage.data.CommunityKeys
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.CommunitiesConfigDto
import com.redvaultsystems.redvaultmessage.data.api.CommunityApi
import com.redvaultsystems.redvaultmessage.data.api.CommunityListingDto
import com.redvaultsystems.redvaultmessage.data.api.CommunityPageDto
import com.redvaultsystems.redvaultmessage.data.api.CommunityReportRequest
import com.redvaultsystems.redvaultmessage.data.api.MembershipDto
import com.redvaultsystems.redvaultmessage.data.api.MembershipViewDto
import com.redvaultsystems.redvaultmessage.data.api.MyCommunityDto
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.serialization.json.buildJsonObject

/**
 * Communities while signed in: the screens' calls (discover, a community's page, joining free ones,
 * leaving, the member's own subscriptions, reports) and the key upkeep loop. Nothing here can buy a
 * membership: see data/api/CommunityApi.kt. Each call throws [UserFacingException] with a message that's
 * safe to show, and a 401 ends the session.
 */
class CommunitySession(
    private val api: CommunityApi,
    private val keys: CommunityKeys,
    private val onSessionExpired: () -> Unit,
) {
    /**
     * Port of the web's 20-second upkeep (ui/src/state/AppStateContext.tsx): delivers the current key to
     * members who are waiting and rotates after someone left. Runs while the app is on screen; [onChanged]
     * reloads the chat list after a rotation or an enrollment.
     */
    suspend fun maintainKeys(onChanged: suspend () -> Unit) {
        delay(FIRST_UPKEEP_MS)
        while (true) {
            try {
                val result = keys.runMaintenance()
                if (result.rotated + result.enrolled > 0) onChanged()
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
            } catch (_: Exception) {
                // Offline or a server hiccup: the next round tries again.
            }
            delay(UPKEEP_INTERVAL_MS)
        }
    }

    suspend fun config(): CommunitiesConfigDto = call { api.config() }

    suspend fun discover(query: String, category: String?): List<CommunityListingDto> =
        call { api.discover(query.trim().ifEmpty { null }, category?.ifEmpty { null }, null) }

    suspend fun mine(): List<MyCommunityDto> = call { api.mine() }

    /** Null when it doesn't exist, or it's unlisted and this account isn't in it. */
    suspend fun page(communityId: String): CommunityPageDto? = call(notFoundIsNull = true) { api.community(communityId, null) }

    /** Free communities, and coming back to a paid one before its period ends. Paid joins start on the web. */
    suspend fun join(communityId: String): MembershipDto = call { api.join(communityId, buildJsonObject {}).membership }

    suspend fun leave(communityId: String) = call { api.leave(communityId) }

    suspend fun memberships(): List<MembershipViewDto> = call { api.memberships() }

    suspend fun cancelMembership(subscriptionId: String): MembershipViewDto = call { api.cancelMembership(subscriptionId) }

    suspend fun resumeMembership(subscriptionId: String): MembershipViewDto = call { api.resumeMembership(subscriptionId) }

    /** Reports the community itself. */
    suspend fun reportCommunity(communityId: String, category: String, details: String) = call {
        api.report(communityId, CommunityReportRequest(targetType = "community", category = category, details = details.trim()))
    }

    /**
     * Reports a message. Its decrypted [text] goes to the reviewers only when the reporter chose to share
     * it ([shareText]); otherwise only the category, details and who sent it do.
     */
    suspend fun reportMessage(communityId: String, messageId: String, senderId: String?, text: String, category: String, details: String, shareText: Boolean) = call {
        api.report(
            communityId,
            CommunityReportRequest(
                targetType = "content",
                targetUserId = senderId,
                category = category,
                details = details.trim(),
                evidenceMessageId = if (shareText) messageId else null,
                evidencePlaintext = if (shareText) text.take(8000) else null,
                disclosureAcknowledged = shareText,
            ),
        )
    }

    /** Signing out: every epoch key is overwritten. */
    fun close() = keys.forget()

    private suspend fun <T> call(block: suspend () -> T): T = call(notFoundIsNull = false, block)!!

    private suspend fun <T> call(notFoundIsNull: Boolean, block: suspend () -> T): T? {
        try {
            return apiCall(ApiClient.json, block)
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            if (e.status == 404 && notFoundIsNull) return null
            throw UserFacingException(e.message?.takeIf { it.isNotBlank() } ?: "Something went wrong. Try again.")
        }
    }

    private companion object {
        const val FIRST_UPKEEP_MS = 3_000L
        const val UPKEEP_INTERVAL_MS = 20_000L
    }
}
