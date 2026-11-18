// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import com.redvaultsystems.redvaultmessage.data.api.FilesStorageLimitsDto
import com.redvaultsystems.redvaultmessage.data.api.PlanStatusDto
import java.time.LocalDate
import java.time.format.DateTimeFormatter
import java.util.Locale

/**
 * The three plan cards and what each one offers, the same rules as the
 * desktop app's desktop/core/src/model/plan_choices.cpp (keep them in step):
 * Free users choose Pro or Premium through Stripe's payment page in the
 * browser; card subscribers switch between Pro and Premium or to Free; a
 * cancelled card subscription offers "Keep <plan>".
 */
object PlanChoices {
    enum class Action { None, Checkout, Switch, Cancel, Keep }

    data class Choice(
        val tier: String,
        val name: String,
        val price: String,
        val per: String = "",
        val includes: List<String>,
        val current: Boolean = false,
        /** Under the name: "Your plan", "Your plan until 3 December 2026". */
        val status: String = "",
        val action: Action = Action.None,
        val actionLabel: String = "",
        /** Asked before the action; empty: no question. */
        val confirm: String = "",
        /** Why there's no action ("Not offered on this server yet."). */
        val note: String = "",
    )

    data class Inputs(
        val tier: String,
        val status: PlanStatusDto?,
        val filesStorage: FilesStorageLimitsDto?,
        val proCheckout: Boolean,
        val premiumCheckout: Boolean,
    )

    fun nameOf(tier: String): String = when (tier) {
        "premium" -> "Premium"
        "pro" -> "Pro"
        else -> "Free"
    }

    fun storageSize(bytes: Long): String {
        val gib = 1024L * 1024 * 1024
        return if (bytes > 0 && bytes % gib == 0L) "${bytes / gib} GB" else String.format(Locale.US, "%.1f GB", bytes.toDouble() / gib)
    }

    /** "2026-12-03" as "3 December 2026". */
    fun longDate(isoDay: String): String =
        runCatching { LocalDate.parse(isoDay).format(DateTimeFormatter.ofPattern("d MMMM yyyy", Locale.ENGLISH)) }.getOrDefault(isoDay)

    private fun base(tier: String, files: FilesStorageLimitsDto?): Choice {
        fun storage(bytes: Long) = "${storageSize(bytes)} of Files storage"
        return when (tier) {
            "free" -> Choice(
                tier, "Free", "$0",
                includes = listOf(
                    "Unlimited end-to-end encrypted messages and groups",
                    "One-to-one voice and video calls",
                    "Send files and voice messages in chats, up to 25 MB each",
                    "Password vault",
                ) + listOfNotNull(files?.let { storage(it.free) }),
            )
            "pro" -> Choice(
                tier, "Pro", "$8", "per month",
                includes = listOf("Everything in Free", "Send files in chats up to 100 MB each", "Voice call links") +
                    listOfNotNull(files?.let { storage(it.pro) }),
            )
            else -> Choice(
                tier, "Premium", "$15", "per month",
                includes = listOf(
                    "Everything in Pro",
                    "Video call links and group voice and video calls",
                    "Screen sharing in calls",
                    "Create your own channels",
                ) + listOfNotNull(files?.let { storage(it.premium) }),
            )
        }
    }

    fun choices(input: Inputs): List<Choice> {
        val tier = if (input.tier == "pro" || input.tier == "premium") input.tier else "free"
        val s = input.status
        val source = s?.source.orEmpty()
        val card = source == "stripe"
        // "Switch to Free" was chosen: the card subscription ends with the paid period.
        val ending = card && s?.canResume == true
        val endsOn = s?.endsOn?.let(::longDate).orEmpty()

        return listOf("free", "pro", "premium").map { t ->
            var c = base(t, input.filesStorage).copy(current = t == tier)

            if (c.current) {
                val status = when {
                    tier != "free" && endsOn.isNotEmpty() -> "Your plan until $endsOn" + if (source == "crypto") " (prepaid)" else ""
                    tier != "free" && s?.renews == true -> "Your plan · renews monthly"
                    else -> "Your plan"
                }
                c = c.copy(status = status)
                if (ending) c = c.copy(action = Action.Keep, actionLabel = "Keep ${c.name}")
                return@map c
            }

            if (c.tier == "free") {
                return@map when {
                    s == null -> c // You're on a paid plan; until its status loads there's nothing to offer.
                    (ending || source == "crypto") && endsOn.isNotEmpty() -> c.copy(status = "Your plan after $endsOn")
                    card && s.canCancel -> c.copy(
                        action = Action.Cancel,
                        actionLabel = "Switch to Free",
                        confirm = "Your subscription stops renewing and you won't be charged again. You keep ${nameOf(tier)} " +
                            "until the end of the period you've paid for, then move to Free: messages, calls and files " +
                            "up to 25 MB each. Files you've already sent stay available until they expire.",
                    )
                    source == "granted" -> c.copy(note = "Your plan was set by an administrator.")
                    else -> c.copy(note = "This server can't change plans from the app.")
                }
            }

            // A paid plan you're not on.
            val checkout = if (c.tier == "pro") input.proCheckout else input.premiumCheckout
            when {
                tier == "free" ->
                    if (checkout) {
                        c.copy(
                            action = Action.Checkout,
                            actionLabel = "Choose ${c.name}",
                            confirm = "This opens Stripe's secure payment page in your browser, outside the app and outside Tor. " +
                                "Your plan changes to ${c.name} here as soon as the payment goes through.",
                        )
                    } else {
                        c.copy(note = "Not offered on this server yet.")
                    }
                s == null -> c
                card ->
                    if (s.canSwitchTo == c.tier) {
                        var confirm = if (c.tier == "premium") {
                            "You're charged the difference for the rest of this billing period now, then $15 a month. " +
                                "Premium starts as soon as that payment goes through."
                        } else {
                            "You move to Pro now, and the unused part of Premium is credited to your next payments. Then $8 a month."
                        }
                        if (ending) confirm += " Your subscription renews again: it no longer ends on $endsOn."
                        c.copy(action = Action.Switch, actionLabel = "Switch to ${c.name}", confirm = confirm)
                    } else {
                        c.copy(note = "This server can't change plans from the app.")
                    }
                source == "crypto" && c.tier == "premium" && checkout -> c.copy(
                    action = Action.Checkout,
                    actionLabel = "Choose Premium",
                    confirm = "This opens Stripe's secure payment page in your browser, outside the app and outside Tor, to " +
                        "start a Premium subscription. Your plan changes to Premium here as soon as the payment goes through.",
                )
                source == "granted" -> c.copy(note = "Your plan was set by an administrator.")
                else -> c.copy(note = "Not offered on this server yet.")
            }
        }
    }

    /**
     * The Stripe payment link with the account's id as client_reference_id
     * (Stripe hands it back to the server's webhook), like the desktop and web apps.
     */
    fun checkoutUrl(paymentLink: String, userId: String): String {
        val uri = android.net.Uri.parse(paymentLink)
        val builder = uri.buildUpon().clearQuery()
        uri.queryParameterNames.filter { it != "client_reference_id" }.forEach { name ->
            uri.getQueryParameters(name).forEach { builder.appendQueryParameter(name, it) }
        }
        return builder.appendQueryParameter("client_reference_id", userId).build().toString()
    }
}
