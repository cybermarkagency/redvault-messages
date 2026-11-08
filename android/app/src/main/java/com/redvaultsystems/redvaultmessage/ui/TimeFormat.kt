// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui

import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import java.util.Locale

/** Timestamps in the phone's own locale and time zone. */
object TimeFormat {
    private val timeFormatter = DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT)
    private val shortDateFormatter = DateTimeFormatter.ofLocalizedDate(FormatStyle.SHORT)
    private val longDateFormatter = DateTimeFormatter.ofLocalizedDate(FormatStyle.LONG)

    private fun zoned(ms: Long) = Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault())

    fun time(ms: Long): String = zoned(ms).format(timeFormatter)

    /** Chat-list label: the time for today, the date otherwise. */
    fun listLabel(ms: Long): String {
        val at = zoned(ms)
        return if (at.toLocalDate() == LocalDate.now(at.zone)) at.format(timeFormatter) else at.format(shortDateFormatter)
    }

    fun longDate(ms: Long): String = zoned(ms).format(longDateFormatter)

    /** The day a message was sent, in the phone's time zone (for the thread's day headers). */
    fun localDate(ms: Long): LocalDate = zoned(ms).toLocalDate()

    /** A thread's day header, like the desktop's: "Today", "Yesterday", "Tuesday, October 21", or with the year when it isn't this one. */
    fun dayLabel(date: LocalDate, today: LocalDate = LocalDate.now(), locale: Locale = Locale.getDefault()): String = when {
        date == today -> "Today"
        date == today.minusDays(1) -> "Yesterday"
        date.year == today.year -> date.format(DateTimeFormatter.ofPattern("EEEE, MMMM d", locale))
        else -> date.format(DateTimeFormatter.ofPattern("MMMM d, yyyy", locale))
    }
}
