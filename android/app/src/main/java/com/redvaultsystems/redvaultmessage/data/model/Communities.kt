// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import java.math.BigDecimal
import java.text.NumberFormat
import java.util.Currency
import java.util.Locale

/** Wording and display rules for communities, as on the web (ui/src/screens/Communities.tsx, CommunityReportDialog.tsx). */
object Communities {
    private val CATEGORY_LABELS = mapOf(
        "art_design" to "Art & design",
        "business" to "Business",
        "education" to "Education",
        "entertainment" to "Entertainment",
        "gaming" to "Gaming",
        "health_fitness" to "Health & fitness",
        "lifestyle" to "Lifestyle",
        "music" to "Music",
        "news" to "News",
        "science_technology" to "Science & technology",
        "sports" to "Sports",
        "writing" to "Writing",
        "other" to "Other",
    )

    fun categoryLabel(category: String): String = CATEGORY_LABELS[category] ?: category

    fun memberCountLabel(count: Int): String = if (count == 1) "1 member" else "$count members"

    /** Money for display only: the amount is always the server's, in minor units ("usd" has 2 decimals). */
    fun formatMoney(amountMinor: Long, currency: String, locale: Locale = Locale.getDefault()): String {
        val code = currency.uppercase(Locale.ROOT)
        val exponent = EXPONENTS[currency.lowercase(Locale.ROOT)] ?: 2
        val amount = BigDecimal.valueOf(amountMinor, exponent)
        return try {
            NumberFormat.getCurrencyInstance(locale).apply {
                this.currency = Currency.getInstance(code)
                minimumFractionDigits = exponent
                maximumFractionDigits = exponent
            }.format(amount)
        } catch (_: IllegalArgumentException) {
            "$code ${amount.toPlainString()}"
        }
    }

    private val EXPONENTS = mapOf("usd" to 2)

    /** A subscription's entitlement in words (the chip on Memberships). */
    fun entitlementLabel(status: String): String = when (status) {
        "pending" -> "Processing payment"
        "active" -> "Active"
        "trialing" -> "Trial"
        "grace_period" -> "Payment failed"
        "cancel_scheduled" -> "Cancelled · access until period end"
        "expired" -> "Ended"
        "canceled" -> "Not completed"
        "refunded" -> "Refunded"
        "disputed" -> "Payment under review"
        "suspended" -> "Suspended"
        else -> status
    }

    /** What a membership that isn't active yet means for the member. Null when there's nothing to explain. */
    fun membershipNotice(status: String): String? = when (status) {
        "pending_enrollment" ->
            "You're in. Your app is waiting for the community's encryption key, which the creator's or another member's app " +
                "delivers automatically the next time one of them is online. Nothing to do on your side."
        "pending_approval" -> "Your request to join was sent. A moderator will review it."
        "banned" -> "You can't join this community."
        else -> null
    }

    /** The short status on My communities. */
    fun membershipChip(status: String): String = when (status) {
        "active" -> "Member"
        "pending_enrollment" -> "Waiting for key"
        "pending_approval" -> "Requested"
        "banned" -> "Banned"
        else -> status
    }

    data class ReportCategory(val value: String, val label: String, val platformOnly: Boolean = false)

    val REPORT_CATEGORIES = listOf(
        ReportCategory("spam", "Spam"),
        ReportCategory("scam", "Scam or fraud"),
        ReportCategory("harassment", "Harassment or hate"),
        ReportCategory("impersonation", "Impersonation"),
        ReportCategory("copyright", "Copyright"),
        ReportCategory("refund_dispute", "Billing or refund problem"),
        ReportCategory("child_safety", "Child safety", platformOnly = true),
        ReportCategory("nonconsensual_imagery", "Intimate images shared without consent", platformOnly = true),
        ReportCategory("illegal", "Other illegal content", platformOnly = true),
        ReportCategory("other", "Something else"),
    )

    /** Who reads a report: RedVault's own reviewers for serious categories and for reports about a community itself, else its moderators. */
    fun reportGoesToRedVault(category: String, aboutCommunity: Boolean): Boolean =
        aboutCommunity || REPORT_CATEGORIES.firstOrNull { it.value == category }?.platformOnly == true

    /** The disclosure next to "share this message's text", naming exactly who will see it. */
    fun disclosureText(toRedVault: Boolean): String =
        "Share this message's text with ${if (toRedVault) "RedVault's safety reviewers" else "this community's moderators"}. " +
            "It's end-to-end encrypted, so they can only see it if you share it. Your name isn't shown to the community's moderators."

    /** The line under a paid community's price on this app. */
    const val NOT_SOLD_HERE = "Paid memberships can't be bought in the Android app."
}
