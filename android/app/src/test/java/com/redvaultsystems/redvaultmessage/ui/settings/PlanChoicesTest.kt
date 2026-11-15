// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import com.redvaultsystems.redvaultmessage.data.api.FilesStorageLimitsDto
import com.redvaultsystems.redvaultmessage.data.api.PlanStatusDto
import com.redvaultsystems.redvaultmessage.ui.settings.PlanChoices.Action
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The same cases as the desktop's desktop/tests/plan_choices_test.cpp: both apps offer the same plan actions. */
class PlanChoicesTest {
    private val gib = 1024L * 1024 * 1024

    private fun inputs(tier: String, status: PlanStatusDto? = null, checkout: Boolean = false, files: FilesStorageLimitsDto? = null) =
        PlanChoices.Inputs(tier, status, files, proCheckout = checkout, premiumCheckout = checkout)

    private fun List<PlanChoices.Choice>.plan(tier: String) = first { it.tier == tier }

    private fun renewingCard(tier: String, other: String) =
        PlanStatusDto(tier = tier, source = "stripe", renews = true, canCancel = true, canSwitchTo = other)

    private fun endingCard(tier: String, other: String) =
        PlanStatusDto(tier = tier, source = "stripe", endsOn = "2026-11-12", canResume = true, canSwitchTo = other)

    @Test
    fun `three plans with prices and storage`() {
        val all = PlanChoices.choices(inputs("free", checkout = true, files = FilesStorageLimitsDto(5 * gib, 50 * gib, 100 * gib)))
        assertEquals(listOf("Free", "Pro", "Premium"), all.map { it.name })
        assertEquals(listOf("$0", "$8", "$15"), all.map { it.price })
        assertEquals("5 GB of Files storage", all[0].includes.last())
        assertEquals("50 GB of Files storage", all[1].includes.last())
        assertEquals("100 GB of Files storage", all[2].includes.last())
        assertEquals("Everything in Pro", all[2].includes.first())
    }

    @Test
    fun `on Free a paid plan is chosen through Stripe`() {
        val all = PlanChoices.choices(inputs("free", checkout = true))
        assertTrue(all.plan("free").current)
        assertEquals("Your plan", all.plan("free").status)
        assertEquals(Action.Checkout, all.plan("pro").action)
        assertEquals("Choose Pro", all.plan("pro").actionLabel)
        assertTrue("payment page" in all.plan("premium").confirm)
        assertEquals("Password vault", all.plan("free").includes.last())

        val none = PlanChoices.choices(inputs("free"))
        assertEquals(Action.None, none.plan("pro").action)
        assertEquals("Not offered on this server yet.", none.plan("premium").note)
    }

    @Test
    fun `a renewing card subscription switches or cancels`() {
        val pro = PlanChoices.choices(inputs("pro", renewingCard("pro", "premium"), checkout = true))
        assertEquals("Your plan · renews monthly", pro.plan("pro").status)
        assertEquals(Action.Switch, pro.plan("premium").action)
        assertTrue("charged the difference" in pro.plan("premium").confirm)
        assertEquals(Action.Cancel, pro.plan("free").action)
        assertTrue("You keep Pro until the end" in pro.plan("free").confirm)

        val premium = PlanChoices.choices(inputs("premium", renewingCard("premium", "pro")))
        assertEquals(Action.Switch, premium.plan("pro").action)
        assertTrue("credited" in premium.plan("pro").confirm)
        assertTrue("You keep Premium" in premium.plan("free").confirm)
    }

    @Test
    fun `after Switch to Free the plan is kept by name`() {
        val pro = PlanChoices.choices(inputs("pro", endingCard("pro", "premium")))
        assertEquals("Your plan until 12 November 2026", pro.plan("pro").status)
        assertEquals(Action.Keep, pro.plan("pro").action)
        assertEquals("Keep Pro", pro.plan("pro").actionLabel)
        assertEquals("Your plan after 12 November 2026", pro.plan("free").status)
        assertTrue("renews again" in pro.plan("premium").confirm)

        // The old Android dialog said "Keep Pro" to Premium subscribers too.
        val premium = PlanChoices.choices(inputs("premium", endingCard("premium", "pro")))
        assertEquals("Keep Premium", premium.plan("premium").actionLabel)
    }

    @Test
    fun `prepaid and admin-given plans`() {
        val crypto = PlanChoices.choices(inputs("pro", PlanStatusDto(tier = "pro", source = "crypto", endsOn = "2026-11-12"), checkout = true))
        assertEquals("Your plan until 12 November 2026 (prepaid)", crypto.plan("pro").status)
        assertEquals(Action.Checkout, crypto.plan("premium").action)

        val granted = PlanChoices.choices(inputs("premium", PlanStatusDto(tier = "premium", source = "granted")))
        assertEquals("Your plan was set by an administrator.", granted.plan("free").note)
        assertEquals("Your plan was set by an administrator.", granted.plan("pro").note)
    }
}
