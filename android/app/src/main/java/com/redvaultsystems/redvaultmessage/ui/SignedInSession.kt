// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui

import com.redvaultsystems.redvaultmessage.devicekeys.DeviceKeysSession
import com.redvaultsystems.redvaultmessage.tor.TorController
import com.redvaultsystems.redvaultmessage.calls.CallManager
import com.redvaultsystems.redvaultmessage.calls.GroupCallManager
import com.redvaultsystems.redvaultmessage.data.ChatSettingsStore
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.push.PushRegistrar
import com.redvaultsystems.redvaultmessage.ui.account.AccountSession
import com.redvaultsystems.redvaultmessage.ui.channels.ChannelSession
import com.redvaultsystems.redvaultmessage.ui.chats.ChatSession
import com.redvaultsystems.redvaultmessage.ui.communities.CommunitySession
import com.redvaultsystems.redvaultmessage.ui.inbox.InboxSession
import com.redvaultsystems.redvaultmessage.ui.files.FilesSession
import com.redvaultsystems.redvaultmessage.ui.files.SharesSession
import com.redvaultsystems.redvaultmessage.ui.components.AvatarLoader
import com.redvaultsystems.redvaultmessage.ui.live.LiveSession
import com.redvaultsystems.redvaultmessage.ui.lock.AppLock
import com.redvaultsystems.redvaultmessage.ui.passwords.VaultSession
import com.redvaultsystems.redvaultmessage.ui.settings.SecuritySession
import com.redvaultsystems.redvaultmessage.ui.stories.StoriesSession

/** Everything that exists only while signed in. [close] ends it all and wipes keys. */
class SignedInSession(
    val identity: SelfIdentity,
    val chats: ChatSession,
    val account: AccountSession,
    val files: FilesSession,
    /** Public share links and the files uploaded through them (docs/public-share-links.md). */
    val shares: SharesSession,
    val vault: VaultSession,
    val live: LiveSession,
    val stories: StoriesSession,
    val calls: CallManager,
    val groupCalls: GroupCallManager,
    val security: SecuritySession,
    val channels: ChannelSession,
    /** The team's messages and polls (not end-to-end encrypted). */
    val inbox: InboxSession,
    val communities: CommunitySession,
    val settings: ChatSettingsStore,
    val push: PushRegistrar,
    val lock: AppLock,
    private val avatarLoader: AvatarLoader,
    /** The web app's address, which group call, invite and channel links in chats point at. */
    val webAppBase: String,
    /** Tor routing's gateway (null for a clearnet server): its status for the shell and Settings › Network routing. */
    val tor: TorController? = null,
    /** Per-device keys (protocol 3, development only): null unless this build has -PredvaultE2eV3=true. */
    val deviceKeys: DeviceKeysSession? = null,
    private val onClosed: () -> Unit,
) {
    fun close() {
        deviceKeys?.close()
        chats.close()
        communities.close()
        shares.close()
        files.close()
        vault.close()
        groupCalls.close()
        calls.close()
        security.close()
        live.close()
        channels.close()
        inbox.close()
        stories.close()
        avatarLoader.clear()
        onClosed()
    }
}
