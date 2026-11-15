// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.ChannelCrypto
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.KeyPair
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaStartRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelQuizRequest
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.nio.file.Files

/**
 * Part C1 end to end against FakeChannelServer (the server's rules, in memory): what the owner's and a
 * reader's phones send and read in a private channel, link resets, both visibility switches with their
 * conversions, and the automatic key setup for a channel made private before C1.
 */
class PrivateChannelsTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val e2e = E2eCrypto(sodium)
    private val crypto = ChannelCrypto(sodium, e2e)
    private val files = FileCrypto(sodium)
    private val server = FakeChannelServer()
    private val work = Files.createTempDirectory("channel-e2ee").toFile()
    private val ownerKeys = e2e.generateKeyPair()
    private val readerKeys = e2e.generateKeyPair()
    private var waits = 0

    @After
    fun cleanUp() {
        work.deleteRecursively()
    }

    private fun phone(userId: String, keys: KeyPair) = Phone(userId, keys)

    private inner class Phone(userId: String, keys: KeyPair) {
        val repository = ChannelRepository(server.public, server.signedIn(userId))
        val channels = PrivateChannels(repository, crypto, files, server.transfer, work.resolve(userId), { keys.publicKey }, { keys.privateKey }, pause = { waits++ })

        /** A reader opening the link: the channel, then its posts, opened. */
        suspend fun read(id: String, secret: String): List<ChannelPostDto> {
            val token = crypto.accessToken(secret)
            channels.useLink(id, secret)
            channels.remember(repository.get(id, token))
            return channels.openPosts(id, repository.posts(id, null, token, 50), token)
        }

        suspend fun upload(id: String, bytes: ByteArray, kind: String = "photo", forSuggestion: Boolean = false, access: String? = null): String =
            channels.sealUpload(id, { bytes.inputStream() }, ChannelMediaStartRequest(kind, "image/png", bytes.size.toLong(), "pic.png", null, 640, 480), forSuggestion, access)
    }

    private fun opened(m: ChannelMediaDto): ByteArray {
        val s = m.sealed ?: throw AssertionError("not sealed")
        val stored = server.media[m.id]!!.bytes!!
        val out = ByteArrayOutputStream()
        files.decrypt(stored.inputStream(), out, s.fileKey, s.header, s.encryptedSize, s.ciphertextSha256)
        return out.toByteArray()
    }

    private val photo = ByteArray(200_000) { (it * 31 % 251).toByte() }

    @Test
    fun everythingInAPrivateChannelIsSealedAndOpensOnlyWithTheLink() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val created = owner.repository.create("Club", "")
        owner.channels.rememberOwned(created)
        val (private, secret) = owner.channels.makePrivate(created.id)
        assertEquals(1, private.keyEpoch)
        val id = created.id

        // Text, a quiz, a checklist, photos, and a scheduled post.
        owner.channels.post(id, ChannelPostRequest("Hello, *members*"))
        owner.channels.post(id, ChannelPostRequest("", poll = ChannelPollRequest("Capital?", listOf("Rome", "Oslo", "Lima"), quiz = ChannelQuizRequest(1, "Norway"))))
        owner.channels.post(id, ChannelPostRequest("", checklist = ChannelChecklistRequest("Bring", listOf("Tent", "Stove"), collaborative = true)))
        val mediaId = owner.upload(id, photo)
        owner.channels.post(id, ChannelPostRequest("Look", media = listOf(mediaId)))
        val later = owner.channels.post(id, ChannelPostRequest("Tomorrow", publishAt = "2026-12-01T09:00:00Z"))
        assertEquals("Tomorrow", later.body)

        // The server holds no text, no options, no item names and no picture.
        for (p in server.posts) {
            assertEquals("", p.body)
            assertNotNull(p.encrypted)
            assertNull(p.poll)
            assertNull(p.checklist)
            for (word in listOf("Hello", "Capital", "Rome", "Norway", "Tent", "Look", "Tomorrow")) assertFalse(p.encrypted!!.ciphertext.contains(word))
        }
        assertEquals(3, server.posts.single { it.encrypted!!.kind == "poll" }.encrypted!!.poll!!.optionCount)
        assertTrue(server.posts.single { it.encrypted!!.kind == "poll" }.encrypted!!.poll!!.quiz)
        assertEquals(2, server.posts.single { it.encrypted!!.kind == "checklist" }.encrypted!!.checklist!!.itemCount)
        val stored = server.media[mediaId]!!
        assertTrue(stored.encrypted)
        assertFalse(stored.bytes!!.contentEquals(photo))

        // A reader with the link opens everything.
        val reader = phone("reader", readerKeys)
        val posts = reader.read(id, secret)
        assertEquals(listOf("Look", "", "", "Hello, *members*"), posts.map { it.body })
        assertTrue(posts.none { it.locked })
        val quiz = posts.first { it.poll != null }.poll!!
        assertEquals("Capital?", quiz.question)
        assertEquals(listOf("Rome", "Oslo", "Lima"), quiz.options)
        assertTrue(quiz.quiz)
        assertNull("an open quiz doesn't give its answer away", quiz.correct)
        val list = posts.first { it.checklist != null }.checklist!!
        assertEquals(listOf("Tent", "Stove"), list.items.map { it.text })
        val m = posts.first().media.single()
        assertEquals("photo", m.kind)
        assertEquals("pic.png", m.filename)
        assertEquals(photo.size.toLong(), m.size)
        assertArrayEquals(photo, opened(m))

        // Votes and ticks go by index; the answers come back filled in.
        val token = crypto.accessToken(secret)
        val quizPost = posts.first { it.poll != null }
        val vote = reader.channels.fillVote(quizPost.id, reader.repository.vote(id, quizPost.id, listOf(1), token))
        assertEquals("Capital?", vote.poll.question)
        assertEquals(1, vote.poll.correct)
        assertEquals("Norway", vote.poll.explanation)
        assertEquals(listOf(0, 1, 0), vote.poll.counts)
        val listPost = posts.first { it.checklist != null }
        val ticked = reader.channels.fillChecklist(listPost.id, reader.repository.setChecklistItem(id, listPost.id, 1, true, token))
        assertEquals(listOf("Tent" to false, "Stove" to true), ticked.items.map { it.text to it.done })

        // The owner's scheduled list opens too.
        assertEquals(listOf("Tomorrow"), owner.channels.openPosts(id, owner.repository.scheduled(id), null).map { it.body })

        // Someone without the link gets nothing; a link with the wrong secret opens nothing.
        try {
            reader.repository.posts(id, null, null, 30)
            fail("read without the link")
        } catch (e: ApiException) {
            assertEquals(404, e.status)
        }
        val stranger = phone("stranger", e2e.generateKeyPair())
        stranger.channels.useLink(id, crypto.newSecret())
        val locked = stranger.channels.openPosts(id, server.posts.filter { it.published }.map(server::dto), token)
        assertTrue(locked.all { it.locked && it.body.isEmpty() })
        assertEquals("This channel's posts can't be opened with this link.", PrivateChannels.LOCKED_MESSAGE)
    }

    @Test
    fun editsSuggestionsAndApprovalsStaySealed() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val created = owner.repository.create("Club", "").also(owner.channels::rememberOwned)
        val (_, secret) = owner.channels.makePrivate(created.id)
        val id = created.id
        val poll = owner.channels.post(id, ChannelPostRequest("Vote", poll = ChannelPollRequest("Day?", listOf("Sat", "Sun"))))
        val reader = phone("reader", readerKeys)
        val token = crypto.accessToken(secret)
        reader.read(id, secret)
        reader.repository.vote(id, poll.id, listOf(0), token)

        // The owner edits the text: re-sealed, the poll and its votes kept.
        val edited = owner.channels.edit(id, poll.id, "Vote by Friday")
        assertEquals("Vote by Friday", edited.body)
        assertEquals(listOf("Sat", "Sun"), edited.poll!!.options)
        val again = reader.read(id, secret).single()
        assertEquals("Vote by Friday", again.body)
        assertEquals(listOf(1, 0), again.poll!!.counts)

        // A follower suggests a post with a photo; the owner opens it, edits it and publishes it.
        reader.channels.remember(reader.repository.get(id, token))
        val pic = reader.upload(id, photo, forSuggestion = true, access = token)
        val sent = reader.channels.suggest(id, "My photo", listOf(pic), token)
        assertEquals("My photo", sent.body)
        val raw = server.suggestionsList.single()
        assertEquals("", raw.body)
        assertNotNull(raw.encrypted)
        val waiting = owner.channels.openSuggestions(id, owner.repository.suggestions(id), null).single()
        assertEquals("My photo", waiting.body)
        assertArrayEquals(photo, opened(waiting.media.single()))
        val published = owner.channels.approve(id, waiting.id, "Photo of the week", null)
        assertEquals("Photo of the week", published.body)
        assertTrue(published.suggested)
        val shown = reader.read(id, secret).first()
        assertEquals("Photo of the week", shown.body)
        assertArrayEquals(photo, opened(shown.media.single()))
    }

    @Test
    fun aResetLinkReadsTheWholeHistoryAndTheOldOneNothingNew() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val created = owner.repository.create("Club", "").also(owner.channels::rememberOwned)
        val id = created.id
        val (_, first) = owner.channels.makePrivate(id)
        owner.channels.post(id, ChannelPostRequest("Before the reset"))
        val oldKeys = server.channels[id]!!.keys

        val (reset, second) = owner.channels.makePrivate(id)
        assertEquals(2, reset.keyEpoch)
        assertEquals(setOf(1, 2), server.channels[id]!!.keys.keys)
        owner.channels.post(id, ChannelPostRequest("After the reset"))
        assertEquals(2, server.posts.last().encrypted!!.keyEpoch)

        // The new link opens both epochs.
        val reader = phone("reader", readerKeys)
        assertEquals(listOf("After the reset", "Before the reset"), reader.read(id, second).map { it.body })

        // The old link: the server refuses its token, and even with the posts in hand its keys open nothing new.
        val old = phone("old", e2e.generateKeyPair())
        try {
            old.read(id, first)
            fail("the old link still read the channel")
        } catch (e: ApiException) {
            assertEquals(404, e.status)
        }
        val oldWrap = crypto.wrapKey(first)
        assertNotNull(crypto.unwrapContentKey(oldKeys[1]!!, id, 1, oldWrap))
        assertNull(crypto.unwrapContentKey(server.channels[id]!!.keys[2]!!, id, 2, oldWrap))
        val epoch1 = crypto.unwrapContentKey(oldKeys[1]!!, id, 1, oldWrap)!!
        val newest = server.posts.last().encrypted!!
        assertNull(crypto.openPost(newest.nonce, newest.ciphertext, epoch1, id, 2))

        // A reset on another device: this phone's stale epoch is noticed and the post goes out under the newest.
        val otherDevice = phone("owner", ownerKeys)
        otherDevice.channels.rememberOwned(otherDevice.repository.mine().single())
        otherDevice.channels.makePrivate(id)
        owner.channels.post(id, ChannelPostRequest("From the first phone"))
        assertEquals(3, server.posts.last().encrypted!!.keyEpoch)
    }

    @Test
    fun goingPrivateConvertsOldPostsAndGoingPublicTurnsThemBack() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val created = owner.repository.create("News", "").also(owner.channels::rememberOwned)
        val id = created.id
        // Public: plain posts, with media.
        owner.channels.remember(created)
        val pid = owner.repository.startMedia(id, ChannelMediaStartRequest("photo", "image/png", photo.size.toLong(), "pic.png"))
        val tmp = work.resolve("plain.png").also { it.writeBytes(photo) }
        server.transfer.send(pid.uploadUrl, tmp)
        owner.channels.post(id, ChannelPostRequest("Plain photo", media = listOf(pid.id)))
        owner.channels.post(id, ChannelPostRequest("", poll = ChannelPollRequest("Which?", listOf("a", "b"))))
        owner.channels.post(id, ChannelPostRequest("", checklist = ChannelChecklistRequest("List", listOf("one"))))
        owner.channels.post(id, ChannelPostRequest("Plain text"))
        // An open quiz: the conversion route gives the owner its answer, so the sealed copy keeps it.
        owner.channels.post(id, ChannelPostRequest("", poll = ChannelPollRequest("Capital?", listOf("Rome", "Oslo", "Lima"), quiz = ChannelQuizRequest(2, "Peru"))))
        assertTrue(server.posts.all { it.encrypted == null })

        // Public → private: every old post sealed, media uploaded again encrypted. A rate limit is waited out.
        val (_, secret) = owner.channels.makePrivate(id)
        server.failNext["replace"] = ApiException(429, "rate_limited", "Too many requests")
        val result = owner.channels.convert(id)
        assertEquals(ConversionResult.Done(5), result)
        assertEquals(1, waits)
        assertTrue(server.posts.all { it.encrypted != null && it.body.isEmpty() })
        assertNull("the plain upload is gone", server.media[pid.id])
        val reader = phone("reader", readerKeys)
        val read = reader.read(id, secret)
        assertEquals(listOf("", "Plain text", "", "", "Plain photo"), read.map { it.body })
        val quizPost = read.first { it.poll?.quiz == true }
        assertNull("still open: not shown before answering", quizPost.poll!!.correct)
        assertTrue(server.posts.single { it.id == quizPost.id }.encrypted!!.poll!!.quiz)
        val answered = reader.channels.fillVote(quizPost.id, reader.repository.vote(id, quizPost.id, listOf(0), crypto.accessToken(secret)))
        assertEquals(2, answered.poll.correct)
        assertEquals("Peru", answered.poll.explanation)
        assertEquals(listOf("a", "b"), read.first { it.poll?.quiz == false }.poll!!.options)
        assertEquals(listOf("one"), read.first { it.checklist != null }.checklist!!.items.map { it.text })
        val sealedPhoto = read.last().media.single()
        assertTrue(server.media[sealedPhoto.id]!!.encrypted)
        assertArrayEquals(photo, opened(sealedPhoto))

        // Private → public: the owner's phone opens each post with the former link's keys and stores it plain.
        val public = owner.channels.makePublic(id)
        assertFalse(public.isPrivate)
        assertNotNull(public.formerSecretSealed)
        assertEquals(1, public.keyEpoch)
        // Until converted, a public channel shows none of the encrypted posts.
        assertTrue(server.public.posts(id, null, 30, null).isEmpty())
        assertEquals(ConversionResult.Done(5), owner.channels.convert(id))
        val plain = server.public.posts(id, null, 30, null)
        assertEquals(listOf("", "Plain text", "", "", "Plain photo"), plain.map { it.body })
        assertTrue(plain.all { it.encrypted == null })
        assertEquals(listOf("a", "b"), plain.first { it.poll?.quiz == false }.poll!!.options)
        // Back to plain with its real answer, not a stand-in.
        val plainQuiz = server.posts.single { it.id == quizPost.id }
        assertEquals(2, plainQuiz.quizCorrect)
        assertEquals("Peru", plainQuiz.quizExplanation)
        assertEquals(listOf("Rome", "Oslo", "Lima"), plainQuiz.poll!!.options)
        val back = plain.last().media.single()
        assertEquals("photo", back.kind)
        assertEquals("pic.png", back.filename)
        assertFalse(server.media[back.id]!!.encrypted)
        assertArrayEquals(photo, server.media[back.id]!!.bytes)

        // And private again: the old epoch is kept and a new one added (1..n+1).
        val (again, _) = owner.channels.makePrivate(id)
        assertEquals(2, again.keyEpoch)
        assertEquals(ConversionResult.Done(5), owner.channels.convert(id))
        assertTrue(server.posts.all { it.encrypted?.keyEpoch == 2 })
    }

    @Test
    fun aPrivateChannelFromBeforeEncryptionGetsItsKeysWithTheSameLink() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val secret = crypto.newSecret()
        val token = crypto.accessToken(secret)
        val old = server.oldPrivate("owner", "Old club", token, crypto.sealSecret(secret, ownerKeys.publicKey))
        server.plainPost(old.id, "Written before encryption")
        server.plainPost(old.id, "Also old")

        // Posting refuses until there are keys: the owner's phone sets them up on the spot.
        owner.channels.rememberOwned(owner.repository.mine().single())
        val result = owner.channels.maintain(old.id)
        assertEquals(ConversionResult.Done(2), result)
        val c = server.channels[old.id]!!
        assertEquals(1, c.keyEpoch)
        assertEquals(token, c.access)
        assertTrue(server.posts.all { it.encrypted?.keyEpoch == 1 })

        // Followers' links keep working and read the converted history.
        val reader = phone("reader", readerKeys)
        assertEquals(listOf("Also old", "Written before encryption"), reader.read(old.id, secret).map { it.body })
        owner.channels.post(old.id, ChannelPostRequest("New and sealed"))
        assertEquals("New and sealed", reader.read(old.id, secret).first().body)
    }

    @Test
    fun postingToAnOldPrivateChannelSetsUpItsKeysFirst() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val secret = crypto.newSecret()
        val old = server.oldPrivate("owner", "Old club", crypto.accessToken(secret), crypto.sealSecret(secret, ownerKeys.publicKey))
        owner.channels.rememberOwned(owner.repository.mine().single())
        val post = owner.channels.post(old.id, ChannelPostRequest("First sealed post"))
        assertEquals("First sealed post", post.body)
        assertEquals(1, server.channels[old.id]!!.keyEpoch)
    }

    @Test
    fun aConversionStoppedByRateLimitsCarriesOnLater() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val created = owner.repository.create("News", "").also(owner.channels::rememberOwned)
        owner.channels.remember(created)
        repeat(3) { owner.channels.post(created.id, ChannelPostRequest("Post $it")) }
        owner.channels.makePrivate(created.id)
        // More rate limits in a row than the phone waits out: the round stops, keeping what's done.
        server.refusals["replace"] = 6
        val result = owner.channels.convert(created.id)
        assertTrue(result is ConversionResult.Paused)
        assertEquals(0, result.converted)
        // Next time it finishes.
        assertEquals(ConversionResult.Done(3), owner.channels.convert(created.id))
    }

    @Test
    fun publicChannelsAreUnchanged() = runBlocking {
        val owner = phone("owner", ownerKeys)
        val created = owner.repository.create("News", "").also(owner.channels::rememberOwned)
        val post = owner.channels.post(created.id, ChannelPostRequest("Hello world"))
        assertNull(post.encrypted)
        assertEquals("Hello world", server.posts.single().body)
        try {
            owner.channels.sealUpload(created.id, { photo.inputStream() }, ChannelMediaStartRequest("photo", "image/png", 1), false, null)
            fail("an encrypted upload went to a public channel")
        } catch (e: ApiException) {
            assertEquals("plain_text_required", e.code)
        }
    }
}
