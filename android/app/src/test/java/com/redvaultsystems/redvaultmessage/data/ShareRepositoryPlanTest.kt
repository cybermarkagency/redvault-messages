package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.data.api.ShareOwnerItemDto
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What a folder link shows as Files changes (the same cases as the desktop's
 * share_repository_test and the web's shareOwner): newest versions only,
 * nothing trashed, locked or outside the folder, and stable item ids.
 */
class ShareRepositoryPlanTest {
    private fun folder(id: String, name: String, parent: String) = DriveEntry(
        id = id, kind = "folder", name = name, parent = parent, mimeType = "", size = 0, encryptedSize = 0, modifiedMs = 1,
        fileId = id, version = 1, trashed = false, trashedMs = 0, meta = JsonObject(emptyMap()),
    )

    private fun file(id: String, name: String, parent: String, fileId: String = id, version: Int = 1, trashed: Boolean = false, locked: Boolean = false) =
        DriveEntry(
            id = id, kind = "file", name = name, parent = parent, mimeType = "text/plain", size = 10, encryptedSize = 27, modifiedMs = 1000L + version,
            fileId = fileId, version = version, trashed = trashed, trashedMs = 0,
            meta = if (locked) buildJsonObject { put("lock", JsonObject(emptyMap())) } else buildJsonObject {
                put("key", "a2V5")
                put("header", "aGVhZGVy")
                put("chunk", 65536)
                put("sha", "b".repeat(64))
            },
        )

    private val entries = listOf(
        folder("root", "Project", ""),
        folder("sub", "Drafts", "root"),
        folder("other", "Private", ""),
        file("f1-v1", "report.txt", "root", "f1", 1),
        file("f1-v2", "report.txt", "root", "f1", 2),
        file("f2", "notes.txt", "sub"),
        file("f3", "secret.txt", "other"),
        file("f-locked", "locked.txt", "root", locked = true),
        file("f-trash", "old.txt", "root", trashed = true),
    )

    @Test
    fun showsTheFolderAndWhatIsInItOnly() {
        val plan = ShareRepository.plan("root", "ROOT-ITEM", entries, emptyList())
        assertEquals(setOf("root", "sub", "f1-v2", "f2"), plan.map { it.backupId }.toSet())
        val byBackup = plan.associateBy { it.backupId }
        assertEquals("ROOT-ITEM", byBackup.getValue("root").itemId)
        assertEquals("", byBackup.getValue("root").meta.getValue("parent").jsonPrimitive.content)
        assertEquals("ROOT-ITEM", byBackup.getValue("sub").meta.getValue("parent").jsonPrimitive.content)
        assertEquals(byBackup.getValue("sub").itemId, byBackup.getValue("f2").meta.getValue("parent").jsonPrimitive.content)
        assertEquals(JsonPrimitive("a2V5"), byBackup.getValue("f1-v2").meta["key"])
        assertFalse(byBackup.getValue("f1-v2").meta.containsKey("fileId"))
    }

    @Test
    fun keepsItemIdsAcrossVersions() {
        val existing = listOf(
            ShareOwnerItemDto("ROOT-ITEM", "root", "folder", "", ""),
            ShareOwnerItemDto("SUB-ITEM", "sub", "folder", "", ""),
            ShareOwnerItemDto("REPORT-ITEM", "f1-v1", "file", "", ""),
        )
        val byBackup = ShareRepository.plan("root", "ROOT-ITEM", entries, existing).associateBy { it.backupId }
        assertEquals("SUB-ITEM", byBackup.getValue("sub").itemId)
        assertEquals("REPORT-ITEM", byBackup.getValue("f1-v2").itemId)
    }

    @Test
    fun aFileLinkIsThatFileUntilItIsTrashed() {
        assertEquals(1, ShareRepository.plan("f2", "ITEM", entries, emptyList()).size)
        assertTrue(ShareRepository.plan("f-trash", "ITEM", entries, emptyList()).isEmpty())
        assertTrue(ShareRepository.plan("missing", "ITEM", entries, emptyList()).isEmpty())
        assertTrue(ShareRepository.plan("f-locked", "ITEM", entries, emptyList()).isEmpty())
    }
}
