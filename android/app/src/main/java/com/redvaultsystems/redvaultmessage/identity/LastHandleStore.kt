// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.identity

import android.content.Context

/**
 * Port of ui/src/lib/lastIdentity.ts. Remembers the handle (never the
 * password or any key material) of whoever last signed in on this device,
 * so reopening the app lands on Log in with the handle filled in instead of
 * Create, where it's easy to register a second account by mistake.
 */
class LastHandleStore(context: Context) {
    private val prefs = context.getSharedPreferences("identity", Context.MODE_PRIVATE)

    fun load(): String? = prefs.getString(KEY, null)

    fun save(handle: String) {
        prefs.edit().putString(KEY, handle).apply()
    }

    private companion object {
        const val KEY = "lastHandle"
    }
}
