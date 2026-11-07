package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

/**
 * Pins group invite links to the web byte for byte. Every vector below came out of the web's own
 * ui/src/lib/groupInviteCrypto.ts (libsodium-wrappers-sumo, run under vitest): two links an admin's browser
 * made, the approval toggle's wrapped key, and the keys a fixed secret derives. If these fail, a link made
 * on the web won't open in this app (or the other way round). Runs on the JVM with lazysodium-java: the same
 * libsodium behind the same LazySodium API the app uses on a phone.
 */
class GroupInviteCryptoInteropTest {
    private val sodium = LazySodiumJava(SodiumJava())
    private val e2e = E2eCrypto(sodium)
    private val crypto = GroupInviteCrypto(sodium, e2e)

    private fun b64(s: String) = Base64Codec.decode(s)
    private fun aead(ciphertext: String, nonce: String) = AeadCiphertext(b64(ciphertext), b64(nonce))
    private fun secret(text: String) = GroupInviteLink.secretFromFragment(text)!!

    private val groupKey = b64("//r18Ovm4dzX0s3Iw765tK+qpaCblpGMh4J9eHNuaWQ=")
    private val webAppBase = "http://localhost:3000/redvaultmessages/"

    // The fixed secret: bytes (i * 7 + 3) & 0xff.
    private val fixedSecret = b64("AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=")

    // newGroupInviteLink(groupKey, openName, false): anyone with the link joins.
    private val openName = "Weekend hikers 🥾 \"quoted\" \\ ünïcode\nline"
    private val openSecret = "xFRiZvwqdHTBk7LYxE7NnPOlBICPHKLZvdXzZs6Dkxs"
    private val openAuth = "d2NyIs92a5xqxgLPBhmugw/OsSXerigDp6Ql5+GEdf4="
    private val openSecretSealed = aead("CVNnr4RzIiJF4PqG6lNxmhrhZv8O5FyvYhrKJfbdydtqirU4TcMJ69Hb5jXEbZYB", "RavcktO0Ozz4h9bxEJr/Tbjs1cnHXIuy")
    private val openPreview = aead(
        "qQ58e5hirBFxoNFd8GNxzc8YJeE2gXZWghoo4r0wfan7Io4BH2UAl9SQqnAATvIeN8Zj7G6ce0NU3uwByYTSH0XMnjN7jk15jk4QM5Yz3nAf1A==",
        "GCw7VH1QX84gGryRDvI+7sHvEJBW2yxZ",
    )
    private val openWrapped = aead("9HSYNb3rbWDNAlQmmibJ9g60hQR+XYKyWYXFCIYFW9/c/liSAk4JRq6ORR/Qg3yz", "d0y1EO/ROmqElS49CDTuDfrqt8pDx3SY")
    // openGroupInviteLink({ id: "AbCdEf123_-x", ... }, groupKey).url
    private val openUrl = "http://localhost:3000/redvaultmessages/?invite=AbCdEf123_-x#xFRiZvwqdHTBk7LYxE7NnPOlBICPHKLZvdXzZs6Dkxs"

    // newGroupInviteLink(groupKey, "Book club", true): an admin approves each join, so no key on the server.
    private val approvalSecret = "1-vqrMKWnxFem2JxtA52Rqho2AZ92Yg2WVf7th9qUy0"
    private val approvalAuth = "Oo4luiP3mqZwCkCgcZlh0iCILAKchHJ9ngk0fIuOofU="
    private val approvalSecretSealed = aead("bdxA9OvN34rYtiQkesZLcd1/W1TEIxiqGcjKoSvSFxXlUwP6qNQ1BVJQ7WXQPILu", "mJ1lYe9JtO4genbfC/gvWUbfmZ9f1ojo")
    private val approvalPreview = aead("Pa1jXgMXTpRqxwtu4qZ4SDSHagQLTdaw8B8YZPwiFlMktiLjK9OI2I3S", "LIYW2CMoQXhRD0Jz+eES3CIJm1K1LkpH")
    // wrapGroupKeyForLink(groupKey, approvalSecret): what turning approval off sends.
    private val approvalWrapped = aead("M9tLTgsrSpi60IMezQAHAsf92EdBcgvbUnx0/Z/7zqomlX6f5dzfiZMUDIPQCSzZ", "u/j8UXsclESMkXEpB1eM78CZgEFHiW1I")

    @Test
    fun derivesTheSameLinkKeyAndAuthTokenAsTheWeb() {
        val keys = crypto.inviteKeys(fixedSecret)
        assertArrayEquals(b64("MAOiwrtIGRNC5TPRXIKQCg19/ANNp/ZvyFXU0NiMkrE="), keys.key)
        assertEquals("+mNrdXj1MIYrfUa4jK84Pbtoy8o+Qnv3bCJbtfSWFwA=", keys.auth)
        // The auth token the server checks is the web's for the web's own links too.
        assertEquals(openAuth, crypto.inviteKeys(secret(openSecret)).auth)
        assertEquals(approvalAuth, crypto.inviteKeys(secret(approvalSecret)).auth)
        // Neither key is the secret, and they differ from each other.
        assertNotEquals(Base64Codec.encode(fixedSecret), keys.auth)
        assertNotEquals(Base64Codec.encode(keys.key), keys.auth)
    }

    @Test
    fun opensTheNameAndGroupKeyOfALinkTheWebMade() {
        val opened = crypto.openPreview(openPreview, openWrapped, secret(openSecret))
        assertEquals(openName, opened.name)
        assertArrayEquals(groupKey, opened.groupKey)
    }

    @Test
    fun aMemberRebuildsTheWebsLinkFromTheGroupKey() {
        val recovered = crypto.openLinkSecret(openSecretSealed.ciphertext, openSecretSealed.nonce, groupKey)!!
        assertArrayEquals(secret(openSecret), recovered)
        assertEquals(openUrl, GroupInviteLink.url(webAppBase, "AbCdEf123_-x", recovered))
        assertArrayEquals(secret(approvalSecret), crypto.openLinkSecret(approvalSecretSealed.ciphertext, approvalSecretSealed.nonce, groupKey))
        // Without the group key there's no link to show.
        assertNull(crypto.openLinkSecret(openSecretSealed.ciphertext, openSecretSealed.nonce, ByteArray(32)))
    }

    @Test
    fun aLinkThatNeedsApprovalCarriesNoGroupKey() {
        val opened = crypto.openPreview(approvalPreview, null, secret(approvalSecret))
        assertEquals("Book club", opened.name)
        assertNull(opened.groupKey)
        // Once the web turned approval off, the wrapped key it sent opens to the group key.
        assertArrayEquals(groupKey, crypto.openPreview(approvalPreview, approvalWrapped, secret(approvalSecret)).groupKey)
    }

    @Test
    fun anotherSecretOpensNothing() {
        assertThrows(CryptoException::class.java) { crypto.openPreview(openPreview, openWrapped, fixedSecret) }
        assertThrows(CryptoException::class.java) { crypto.openPreview(openPreview, null, secret(approvalSecret)) }
        // The right preview but a wrapped key from another link.
        assertThrows(CryptoException::class.java) { crypto.openPreview(approvalPreview, openWrapped, secret(approvalSecret)) }
    }

    @Test
    fun aNameThatIsntTextShowsAsGroupLikeTheWeb() {
        // {"v":1,"name":5} under the fixed secret's key; the web's openGroupInvitePreview shows "Group".
        val odd = aead("EQosMZ24ri5se/c51aPaYA3puRWRmVGyvO9cHeewdJo=", "6W5Tu06RAK6Q2g4NTYxxXH192lS3Jq6q")
        assertEquals("Group", crypto.openPreview(odd, null, fixedSecret).name)
    }

    @Test
    fun aLinkThisAppMakesHasTheShapeTheServerAndTheWebExpect() {
        val made = crypto.newLink(groupKey, openName, approval = false)
        val m = made.material
        assertEquals(GroupInviteLink.SECRET_BYTES, made.secret.size)
        assertEquals(crypto.inviteKeys(made.secret).auth, m.auth)
        assertEquals(32, b64(m.auth).size)
        // The server takes exactly these sizes (groupInviteService.ts: 24-byte nonces, 32 + 16 for keys).
        for (sealed in listOf(m.secretSealed, m.wrappedKey!!)) {
            assertEquals(48, sealed.ciphertext.size)
            assertEquals(24, sealed.nonce.size)
        }
        assertEquals(24, m.preview.nonce.size)
        // The preview is the web's JSON.stringify({ v: 1, name }), which its JSON.parse reads back.
        val preview = e2e.aeadDecrypt(m.preview.ciphertext, m.preview.nonce, crypto.inviteKeys(made.secret).key).toString(Charsets.UTF_8)
        val parsed = Json.parseToJsonElement(preview).jsonObject
        assertEquals(listOf("v", "name"), parsed.keys.toList())
        assertEquals(1, parsed["v"]!!.jsonPrimitive.int)
        assertEquals(openName, parsed["name"]!!.jsonPrimitive.content)
        assertEquals("{\"v\":1,\"name\":\"Book club\"}", previewOf(crypto.newLink(groupKey, "Book club", approval = true)))
        // And it opens the way the web opens one.
        val opened = crypto.openPreview(m.preview, m.wrappedKey, made.secret)
        assertEquals(openName, opened.name)
        assertArrayEquals(groupKey, opened.groupKey)
        assertArrayEquals(made.secret, crypto.openLinkSecret(m.secretSealed.ciphertext, m.secretSealed.nonce, groupKey))
        // Approval on: nothing for the server to hand out.
        assertNull(crypto.newLink(groupKey, "Book club", approval = true).material.wrappedKey)
        // Every link gets its own secret.
        assertNotEquals(Base64Codec.encode(made.secret), Base64Codec.encode(crypto.newLink(groupKey, openName, false).secret))
    }

    @Test
    fun theJoinerSealsTheGroupKeyToTheirOwnKey() {
        val me = e2e.generateKeyPair()
        val sealed = e2e.sealConversationKey(groupKey, me.publicKey)
        assertArrayEquals(groupKey, e2e.unsealConversationKey(sealed, me.publicKey, me.privateKey))
    }

    private fun previewOf(link: NewInviteLink): String =
        e2e.aeadDecrypt(link.material.preview.ciphertext, link.material.preview.nonce, crypto.inviteKeys(link.secret).key).toString(Charsets.UTF_8)
}
