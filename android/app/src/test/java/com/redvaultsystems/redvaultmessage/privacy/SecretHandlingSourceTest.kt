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
        mapOf(
            "send" to "OlderProtocolOnly.Reply",
            "createPoll" to "OlderProtocolOnly.Poll",
            "vote" to "OlderProtocolOnly.Vote",
            "endPoll" to "OlderProtocolOnly.Vote",
            "forwardMany" to "OlderProtocolOnly.Forward",
            "forwardChannelPost" to "OlderProtocolOnly.Forward",
            "sendStoryReply" to "OlderProtocolOnly.StoryReply",
            "togglePin" to "OlderProtocolOnly.Pin",
            "unpin" to "OlderProtocolOnly.Pin",
            "toggleReaction" to "OlderProtocolOnly.OlderMessageChange",
            "edit" to "OlderProtocolOnly.OlderMessageChange",
        ).forEach { (function, refusal) -> assertTrue("$function must refuse with $refusal", refusal in body(chat, function)) }
    }

    @Test
    fun torRoutingDoesntTakeAChatOnDeviceKeysBackToProtocolOne() {
        val chat = file("ChatSession.kt")
        assertTrue("OlderProtocolOnly.TorRouting" in body(chat, "send"))
        for (send in listOf("sendFile", "sendPending", "sendVoice")) assertTrue("$send must refuse", "fileRefusal(conversationId)" in body(chat, send))
        assertTrue("OlderProtocolOnly.TorRouting" in body(chat, "drain"))
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
}
