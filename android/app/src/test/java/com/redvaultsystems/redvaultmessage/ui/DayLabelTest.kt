package com.redvaultsystems.redvaultmessage.ui

import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.LocalDate
import java.util.Locale

/** The thread's day headers, worded like the desktop's (desktop/app/src/chat_models.cpp formatDay). */
class DayLabelTest {
    private val today = LocalDate.of(2026, 10, 21)

    @Test
    fun todayYesterdayThenTheDate() {
        assertEquals("Today", TimeFormat.dayLabel(today, today, Locale.US))
        assertEquals("Yesterday", TimeFormat.dayLabel(today.minusDays(1), today, Locale.US))
        assertEquals("Monday, October 19", TimeFormat.dayLabel(today.minusDays(2), today, Locale.US))
        assertEquals("Thursday, January 1", TimeFormat.dayLabel(LocalDate.of(2026, 1, 1), today, Locale.US))
    }

    @Test
    fun anotherYearSaysWhich() {
        assertEquals("December 31, 2025", TimeFormat.dayLabel(LocalDate.of(2025, 12, 31), today, Locale.US))
        // Yesterday across New Year is still "Yesterday".
        assertEquals("Yesterday", TimeFormat.dayLabel(LocalDate.of(2025, 12, 31), LocalDate.of(2026, 1, 1), Locale.US))
    }
}
