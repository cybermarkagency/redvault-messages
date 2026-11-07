package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.data.model.Disappearing
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageKind

internal object ChatTestData {
    fun text(id: String, atMs: Long, fromMe: Boolean = false, text: String = "hello $id") =
        Message(id = id, fromMe = fromMe, senderId = if (fromMe) "me" else "them", kind = MessageKind.Text, text = text, createdAtMs = atMs, deleteAtMs = null)

    fun file(id: String, mimeType: String, size: Long = 1_000, status: String? = null, atMs: Long = 0) = Message(
        id = id,
        fromMe = false,
        kind = MessageKind.File,
        text = "photo.jpg",
        file = FileDescriptor(
            type = "file", version = 1, objectId = "obj-$id", fileKey = "a2V5", filename = "photo.jpg", mimeType = mimeType,
            originalSize = size, encryptedSize = size + 32, ciphertextSha256 = "0".repeat(64),
            encryption = FileEncryptionInfo("xchacha20poly1305-secretstream", 1, "aGVhZGVy", 65536),
        ),
        fileStatus = status,
        createdAtMs = atMs,
        deleteAtMs = null,
    )

    fun chat(id: String, otherUserId: String = "", isGroup: Boolean = false, unread: Int = 0, verified: Boolean = false, handle: String = id) =
        Conversation(
            meta = ConversationMeta(
                id = id, otherUserId = otherUserId, handle = handle, avatarUrl = null, otherPublicKey = ByteArray(0), otherPublicKeyB64 = "",
                conversationKey = ByteArray(32), safetyNumber = emptyList(), fetchedAtMs = 0, isGroup = isGroup,
            ),
            disappearing = Disappearing.Off,
            messages = emptyList(),
            unread = unread,
            verified = verified,
        )
}
