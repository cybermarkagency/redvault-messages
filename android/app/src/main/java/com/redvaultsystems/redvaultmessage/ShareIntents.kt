// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import com.redvaultsystems.redvaultmessage.ui.chats.SharedContent
import com.redvaultsystems.redvaultmessage.ui.chats.SharedItems

/**
 * "Share to RedVault": what another app handed over with ACTION_SEND or ACTION_SEND_MULTIPLE. Nothing is
 * read here, only checked: content:// from other apps only, never a file path and never one of this app's
 * own providers (any app can send this intent, and this app can read its own decrypted files), at most
 * [SharedContent.MAX_FILES] files. Nothing is sent until the user picks a chat and presses Send.
 */
internal object ShareIntents {
    fun itemsFrom(context: Context, intent: Intent): SharedItems? {
        val streams = ArrayList<Uri>()
        if (intent.action == Intent.ACTION_SEND_MULTIPLE) streams += streamList(intent)
        else stream(intent)?.let { streams += it }
        // Some apps put the files only in the ClipData.
        if (streams.isEmpty()) intent.clipData?.let { clip -> for (i in 0 until clip.itemCount) clip.getItemAt(i).uri?.let { streams += it } }
        val own = context.packageName
        val uris = streams.distinct()
            .filter { SharedContent.acceptable(it.scheme, it.authority, own) && !isOwnProvider(context, it.authority) }
            .take(SharedContent.MAX_FILES)
        val text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT)?.toString().orEmpty().trim().take(SharedContent.MAX_TEXT)
        if (uris.isEmpty() && text.isEmpty()) return null
        return SharedItems(uris, text)
    }

    @Suppress("DEPRECATION")
    private fun stream(intent: Intent): Uri? =
        if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
        else intent.getParcelableExtra(Intent.EXTRA_STREAM) as? Uri

    @Suppress("DEPRECATION")
    private fun streamList(intent: Intent): List<Uri> = runCatching {
        if (Build.VERSION.SDK_INT >= 33) intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java).orEmpty()
        else intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM).orEmpty()
    }.getOrDefault(emptyList())

    /** A provider of this app's own, whatever its name (when that can't be told, it's refused). */
    @Suppress("DEPRECATION")
    private fun isOwnProvider(context: Context, authority: String?): Boolean {
        if (authority.isNullOrEmpty()) return true
        return runCatching { context.packageManager.resolveContentProvider(authority, 0)?.packageName == context.packageName }.getOrDefault(true)
    }
}
