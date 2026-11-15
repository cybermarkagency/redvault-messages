// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.privacy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Rules checked in the source, for code that only runs on a phone (screens) or needs Android's main thread and the
 * whole signed-in session (ChatSession, GroupCallManager), so a JVM test can't drive it. Unit tests run with the app
 * module (android/app) as the working directory.
 */
class SecretHandlingSourceTest {
    private val sources = File("src/main/java").walk().filter { it.extension == "kt" }.associate { it.invariantSeparatorsPath to it.readText() }

    private fun file(name: String) = sources.entries.single { it.key.endsWith("/$name") }.value

    private fun filesContaining(code: String) = sources.filterValues { code in it }.keys.map { it.substringAfterLast('/') }.toSet()

    /** The text of function [name] in [source], up to its closing brace (the next line with the same indent). */
    private fun body(source: String, name: String): String {
        val start = Regex("""(?m)^( *)(?:[a-z]+ )*fun (?:\w+\.)?$name\(""").find(source)
        assertTrue("$name is gone: update this test", start != null)
        val indent = start!!.groupValues[1]
        val end = Regex("""(?m)^$indent\}""").find(source, start.range.last)?.range?.first ?: source.length
        return source.substring(start.range.first, end)
    }

    @Test
    fun secretsReachTheClipboardOnlyThroughSecretClipboard() {
        // The safety number on the Verify screen is the one other copy: it's public, and marked sensitive there.
        assertEquals(setOf("SecretClipboard.kt", "VerifyScreens.kt"), filesContaining("setPrimaryClip("))
        assertTrue("SecretClipboard.copy(context, recoveryKey, secret = true)" in file("RecoveryScreens.kt"))
        assertTrue("SecretClipboard.copy(context, current.secretBase32, secret = true)" in file("SettingsScreens.kt"))
    }

    @Test
    fun onlyTheCountedHoldTouchesFlagSecure() {
        // Anything else clearing the flag would switch the protection off for everyone who still wants it.
        assertEquals(setOf("WindowProtection.kt"), filesContaining("LayoutParams.FLAG_SECURE"))
    }

    @Test
    fun theRecoveryScreensOutsideTheRoutesProtectThemselves() {
        val recovery = file("RecoveryScreens.kt")
        assertTrue("SecureWindow()" in body(recovery, "RecoveryKeyContent"))
        assertTrue("TouchGuard()" in body(recovery, "RecoveryKeyIssuedScreen"))
        assertTrue("SecureWindow()" in body(recovery, "RecoverAccountForm"))
        assertTrue("TouchGuard()" in body(recovery, "RecoverAccountForm"))
    }

    @Test
    fun aPastedPictureWaitsForAChangedKeyLikeAnyOtherFile() {
        val chat = file("ChatSession.kt")
        for (send in listOf("sendPending", "sendFile", "sendVoice")) {
            assertTrue("$send must honour the key-change pause and a refused key", "sendRefusal(conversationId)" in body(chat, send))
        }
        assertTrue("keyChangePaused(conversationId)" in body(chat, "sendRefusal"))
        assertTrue("repository.keyProblem(conversationId)" in body(chat, "sendRefusal"))
    }

    @Test
    fun aRefusedKeyIsSaidInTheChatAndNeverReopened() {
        assertTrue("conversation.keyProblem?.let { Banner(text = it) }" in file("ThreadScreen.kt"))
        assertTrue("keyProblem = repository.keyProblem(dto.id)" in file("ChatSession.kt"))
        val hydrate = body(file("ConversationRepository.kt"), "hydrateMeta")
        assertTrue("ring.currentKey ?: ByteArray(E2eCrypto.AEAD_KEY_BYTES)" in hydrate)
    }

    @Test
    fun everyRequestThatSetsAPasswordNamesItsKeyStretchingSettings() {
        val auth = file("AuthRepository.kt")
        for (code in listOf(body(auth, "register"), body(auth, "recover"), body(file("AccountRepository.kt"), "changePassword"))) {
            assertTrue("kdfOps = wrapped.kdf?.ops" in code && "kdfMem = wrapped.kdf?.mem" in code)
        }
    }

    @Test
    fun aRefusedKeyStretchingSettingIsShownAsItIsNotAsAWrongPassword() {
        val auth = file("AuthViewModel.kt")
        assertTrue(auth.indexOf("e is UnsafeKdfException ||") in 0 until auth.indexOf("genericError != null -> genericError"))
    }

    @Test
    fun whatDeviceKeysCantCarryIsRefusedInAChatOnThem() {
        val chat = file("ChatSession.kt")
        // Part A: replies, polls, votes, forwards and pins go on device keys, and since part D story replies; what's left
        // is a change to a message sent before the chat moved to device keys.
        mapOf(
            "vote" to "OlderProtocolOnly.OlderMessageChange",
            "endPoll" to "OlderProtocolOnly.OlderMessageChange",
            "togglePin" to "OlderProtocolOnly.OlderMessageChange",
            "unpin" to "OlderProtocolOnly.OlderMessageChange",
            "toggleReaction" to "OlderProtocolOnly.OlderMessageChange",
            "edit" to "OlderProtocolOnly.OlderMessageChange",
        ).forEach { (function, refusal) -> assertTrue("$function must refuse with $refusal", refusal in body(chat, function)) }
        // Part D: story replies and reactions go on device keys too.
        assertTrue("OlderProtocolOnly.StoryReply" !in body(chat, "sendStoryReply"))
        for (gone in listOf("Reply", "Poll", "Vote", "Forward", "Pin", "StoryReply")) {
            assertTrue("OlderProtocolOnly.$gone is gone", filesContaining("OlderProtocolOnly.$gone)").isEmpty() && filesContaining("OlderProtocolOnly.$gone,").isEmpty())
        }
        // Each goes on device keys first when the chat is on them.
        assertTrue("pollV3(meta, question, options, multiple)" in body(chat, "createPoll"))
        assertTrue("forwardsOnDeviceKeys(" in body(chat, "forwardMany"))
        assertTrue("forwardsOnDeviceKeys(meta)" in body(chat, "forwardChannelPost"))
    }

    @Test
    fun deviceKeysWorkOverTorInsteadOfBeingRefused() {
        // docs/tor-by-default.md rule 5: the old "device keys don't work with Tor routing yet" refusal is gone...
        assertTrue(filesContaining("OlderProtocolOnly.TorRouting").isEmpty())
        assertTrue(filesContaining("fileRefusal(").isEmpty())
        assertTrue(filesContaining("don't work with Tor routing").isEmpty())
        // ...and the Tor outbox sends each item on device keys first (a chat on them never goes out the usual way).
        val chat = file("ChatSession.kt")
        val drain = body(chat, "drain")
        assertTrue(drain.indexOf("flushDeviceKeyQueue()") in 0 until drain.indexOf("sendQueuedOnDeviceKeys(item, meta, box)"))
        assertTrue(drain.indexOf("sendQueuedOnDeviceKeys(item, meta, box)") < drain.indexOf("repository.send(meta, item.text"))
    }

    @Test
    fun nothingInTheAppTakesAChatBackToTheOlderProtocol() {
        // Step 1 (docs/signal-integration/07-step1-default-path.md section 3): no "Use the older protocol", no opt-in.
        // V3Chat.useOlderProtocol stays for tests only.
        assertEquals(setOf("V3Chat.kt"), filesContaining("useOlderProtocol("))
        assertTrue(filesContaining("Use the older protocol").isEmpty())
        assertTrue(filesContaining("sendErrorOlder").isEmpty())
        assertTrue(filesContaining("chat-older-").all { it == "V3Chat.kt" })
    }

    @Test
    fun aChatOnDeviceKeysOffersEverythingAgain() {
        // Part A (docs/signal-integration/08-part-a-chat-features.md): what step 1 hid is back.
        val thread = file("ThreadScreen.kt")
        for (gone in listOf(
            "onStartPoll = if (deviceKeyChat) null",
            "onSelect = if (deviceKeyChat) null",
            "onForward = if (!deviceKeyChat &&",
            "onSelect = if (!deviceKeyChat &&",
            "!deviceKeyChat || conversation.disappearing != Disappearing.Off",
            "!message.v3 && !deviceKeyChat",
        )) assertTrue("ThreadScreen must not hide: $gone", gone !in thread)
        val app = file("SignedInApp.kt")
        assertTrue("deviceKeyChat = session.chats.onDeviceKeys(conversation.id)" in app)
        assertTrue("!session.chats.onDeviceKeys(it.id)" !in app)
        assertTrue("session.chats.messagesOnScreen(conversation.id, ids)" in app)
    }

    @Test
    fun deviceKeysAreEnsuredOnStartUnlockAndEverySixHours() {
        assertTrue("deviceKeys?.startEnsuring()" in file("RedvaultApp.kt"))
        assertTrue("if (!locked) session.deviceKeys?.ensureSoon()" in file("SignedInApp.kt"))
        assertTrue("DeviceKeysChatsBanner(" in file("SignedInApp.kt"))
    }

    @Test
    fun aRemovedMemberLeavesTheCallAndNothingIsPublishedWithoutE2ee() {
        val calls = file("GroupCallManager.kt")
        assertTrue("""if (type == "group-removed") return onRemovedFromCall(callId)""" in body(calls, "handleEvent"))
        assertTrue("if (join.callId in removedFrom)" in body(calls, "connect"))
        assertTrue("if (!encryptionOn(s, room)) return endForEncryption(s)" in body(calls, "maybePublish"))
        assertTrue("if (!rekeySenders(s)) return endForEncryption(s)" in body(calls, "applyKey"))
        assertTrue("if (!installOrSilence({ s.keys.installKey(key, epoch) }, publishedSenders(s))) return endForEncryption(s)" in body(calls, "applyKey"))
    }

    @Test
    fun anonymousStoryAnswersNeverReadKeysOutOfTheEngineState() {
        // Part D: RVSD copies are sealed and opened by the engine (storySeal, storyUnseal); the app never decrypts its
        // own engine state to get at a private key.
        assertTrue(filesContaining("diffie_hellman_key").isEmpty())
        assertTrue(filesContaining("withIdentityKeyPair").isEmpty())
        val stories = file("V3Stories.kt")
        assertTrue("\"storySeal\"" in stories && "\"storyUnseal\"" in stories)
        assertTrue("cryptoBoxSealOpen" !in stories && "cryptoBoxSeal(" !in stories)
    }
}
