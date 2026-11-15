// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Settings > Appearance: "system", "dark" (the default, the app's original look) or "light". Kept on this
 * phone only, outside any account, so the sign-in screens follow it too. Nothing secret.
 */
object AppearanceStore {
    const val SYSTEM = "system"
    const val DARK = "dark"
    const val LIGHT = "light"
    private const val PREFS = "appearance"
    private const val KEY = "mode"

    private val _mode = MutableStateFlow(DARK)
    val mode: StateFlow<String> = _mode.asStateFlow()

    fun load(context: Context) {
        _mode.value = normalize(context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, DARK))
    }

    fun set(context: Context, value: String) {
        val mode = normalize(value)
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY, mode).apply()
        _mode.value = mode
    }

    fun normalize(value: String?): String = when (value) {
        SYSTEM, LIGHT -> value
        else -> DARK
    }

    /** Whether to draw dark, given the mode and whether the system is in dark mode right now. */
    fun isDark(mode: String, systemDark: Boolean): Boolean = when (mode) {
        LIGHT -> false
        SYSTEM -> systemDark
        else -> true
    }
}
