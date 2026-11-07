package com.redvaultsystems.redvaultmessage.crypto

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The transparency log's proofs, against vectors from the server's own
 * MerkleTree (server/src/crypto/merkle.ts): 7 leaves "commitment-0".."6".
 */
class MerkleTest {
    private fun h(hex: String) = Merkle.fromHex(hex)
    private fun leaf(i: Int) = Merkle.leafHash("commitment-$i".toByteArray())

    private val root3 = h("70dea859015f839d442ac7d77a13e52df8dc5befb51b5cf538572c99a2a44502")
    private val root4 = h("68652b9cc029d0b1e154e577a06f6f19750e11b23ddbd1420e3c7ce1db2fff23")
    private val root7 = h("f7cbfd3b6854c6c97f1645d52d9a02bff058b87b7f571394d9437fbaf0f058da")
    private val inclusion3of7 = listOf(
        "fe9fb74e24360db740d4b104571dcdee2186727c29b3b473da7cfad5f12d9d86",
        "8e720d9e5ca80b7e37b29754e0d83de3e3e2159533b26c4ee328e220d1256db6",
        "887dbac6ce69d865e5bd7c9daa0c3feadbeda1276f78ef76e4cb535681ac4fee",
    ).map(::h)
    private val inclusion6of7 = listOf(
        "795285c71d2d153648c73e7e9eb7046a927abdd6b0cee2d8c5c8c7cd8383408f",
        "68652b9cc029d0b1e154e577a06f6f19750e11b23ddbd1420e3c7ce1db2fff23",
    ).map(::h)
    private val consistency3to7 = listOf(
        "fe9fb74e24360db740d4b104571dcdee2186727c29b3b473da7cfad5f12d9d86",
        "3dd1ebb6555b745d427ea339eb7fad5bee05075551b809a88f45ef50a1531acd",
        "8e720d9e5ca80b7e37b29754e0d83de3e3e2159533b26c4ee328e220d1256db6",
        "887dbac6ce69d865e5bd7c9daa0c3feadbeda1276f78ef76e4cb535681ac4fee",
    ).map(::h)
    private val consistency4to7 = listOf("887dbac6ce69d865e5bd7c9daa0c3feadbeda1276f78ef76e4cb535681ac4fee").map(::h)

    @Test
    fun inclusionProofsFromTheServerVerify() {
        assertTrue(Merkle.verifyInclusion(leaf(3), 3, 7, inclusion3of7, root7))
        assertTrue(Merkle.verifyInclusion(leaf(6), 6, 7, inclusion6of7, root7))
    }

    @Test
    fun inclusionFailsForAnotherLeafIndexOrRoot() {
        assertFalse(Merkle.verifyInclusion(leaf(2), 3, 7, inclusion3of7, root7))
        assertFalse(Merkle.verifyInclusion(leaf(3), 2, 7, inclusion3of7, root7))
        assertFalse(Merkle.verifyInclusion(leaf(3), 3, 7, inclusion3of7, root4))
        assertFalse(Merkle.verifyInclusion(leaf(3), 7, 7, inclusion3of7, root7))
    }

    @Test
    fun consistencyProofsFromTheServerVerify() {
        assertTrue(Merkle.verifyConsistency(3, 7, root3, root7, consistency3to7))
        assertTrue(Merkle.verifyConsistency(4, 7, root4, root7, consistency4to7))
        assertTrue(Merkle.verifyConsistency(7, 7, root7, root7, emptyList()))
    }

    @Test
    fun aRewrittenHistoryFailsConsistency() {
        assertFalse(Merkle.verifyConsistency(3, 7, root4, root7, consistency3to7))
        assertFalse(Merkle.verifyConsistency(4, 7, root4, root3, consistency4to7))
        assertFalse(Merkle.verifyConsistency(7, 3, root7, root3, consistency3to7))
    }

    @Test
    fun checkpointMessageMatchesTheServer() {
        assertEquals(
            "redvault-transparency-log-v1\n7\nabc\n2026-10-01T12:00:00.000Z",
            Merkle.checkpointMessage(7, "abc", "2026-10-01T12:00:00.000Z"),
        )
        assertEquals("cb69 6178 8be7 b10d", Merkle.keyFingerprint("y2lheIvnsQ1q8EX4Je1M4g1uPFRg9JUx3gUPhkQFFsw="))
    }
}
