// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Locale

/** The same wording and rules as ui/src/screens/Communities.tsx and CommunityReportDialog.tsx. */
class CommunitiesTest {
    @Test
    fun moneyIsTheServersMinorUnits() {
        assertEquals("$10.00", Communities.formatMoney(1000, "usd", Locale.US))
        assertEquals("$0.99", Communities.formatMoney(99, "USD", Locale.US))
        assertEquals("$1,234.56", Communities.formatMoney(123456, "usd", Locale.US))
    }

    @Test
    fun seriousCategoriesAndReportsAboutACommunityGoToRedVault() {
        assertFalse(Communities.reportGoesToRedVault("spam", aboutCommunity = false))
        assertTrue(Communities.reportGoesToRedVault("spam", aboutCommunity = true))
        for (category in listOf("child_safety", "nonconsensual_imagery", "illegal")) {
            assertTrue(Communities.reportGoesToRedVault(category, aboutCommunity = false))
        }
        // Every category the server accepts (reportSchema), and nothing else.
        assertEquals(
            setOf("spam", "scam", "harassment", "child_safety", "nonconsensual_imagery", "copyright", "illegal", "impersonation", "refund_dispute", "other"),
            Communities.REPORT_CATEGORIES.map { it.value }.toSet(),
        )
    }

    @Test
    fun theDisclosureNamesWhoWillSeeTheText() {
        assertTrue(Communities.disclosureText(toRedVault = true).contains("RedVault's safety reviewers"))
        assertTrue(Communities.disclosureText(toRedVault = false).contains("this community's moderators"))
        assertTrue(Communities.disclosureText(toRedVault = false).contains("only see it if you share it"))
    }

    @Test
    fun labels() {
        assertEquals("Science & technology", Communities.categoryLabel("science_technology"))
        assertEquals("brand_new", Communities.categoryLabel("brand_new"))
        assertEquals("Payment failed", Communities.entitlementLabel("grace_period"))
        assertEquals("Waiting for key", Communities.membershipChip("pending_enrollment"))
        assertEquals("1 member", Communities.memberCountLabel(1))
        assertEquals("2 members", Communities.memberCountLabel(2))
    }
}
