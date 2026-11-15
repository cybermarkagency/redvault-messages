// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.compose.ui.res.stringResource
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.BillingConfigDto
import com.redvaultsystems.redvaultmessage.data.api.PlanStatusDto
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.account.AccountSession
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

/**
 * Settings › Plan: Free, Pro and Premium side by side, with what each
 * includes and the one thing you can do with each (PlanChoices). Buying
 * opens Stripe's payment page in the browser, like the desktop app; the
 * plan here changes once Stripe tells the server, so it's read again when
 * the app comes back to the front.
 */
@Composable
fun PlansScreen(account: AccountSession, tier: String, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    val colors = RedvaultTheme.colors
    var status by remember { mutableStateOf<PlanStatusDto?>(null) }
    var config by remember { mutableStateOf<BillingConfigDto?>(null) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var asking by remember { mutableStateOf<PlanChoices.Choice?>(null) }

    suspend fun load() {
        config = runCatching { account.billingConfig() }.getOrNull()
        // A Free account has no status to read on an older server; the cards work without it.
        status = runCatching { account.planStatus() }.getOrNull()
    }
    LaunchedEffect(tier) { load() }

    // Back from Stripe in the browser: read the plan again.
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) scope.launch {
                account.refreshPlan()
                load()
            }
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }

    val checkoutLink: (String) -> String? = { t ->
        val link = if (t == "premium") config?.premiumPaymentLink else config?.paymentLink
        // A Google Play build (SELL_PLANS off) never links to an outside payment page.
        link?.takeIf { com.redvaultsystems.redvaultmessage.BuildConfig.SELL_PLANS && it.startsWith("https://") }
    }
    val choices = PlanChoices.choices(
        PlanChoices.Inputs(
            tier = tier,
            status = status,
            filesStorage = config?.filesStorage,
            proCheckout = checkoutLink("pro") != null,
            premiumCheckout = checkoutLink("premium") != null,
        ),
    )

    fun perform(choice: PlanChoices.Choice) {
        asking = null
        error = null
        when (choice.action) {
            PlanChoices.Action.Checkout -> {
                val link = checkoutLink(choice.tier) ?: return
                appLock?.expectExternalReturn()
                try {
                    context.startActivity(
                        Intent(Intent.ACTION_VIEW, Uri.parse(PlanChoices.checkoutUrl(link, account.userId))).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                    )
                } catch (_: ActivityNotFoundException) {
                    error = "No browser is installed to open the payment page."
                }
            }
            PlanChoices.Action.Switch, PlanChoices.Action.Cancel, PlanChoices.Action.Keep -> {
                busy = true
                scope.launch {
                    try {
                        status = when (choice.action) {
                            PlanChoices.Action.Switch -> account.switchPlan(choice.tier)
                            PlanChoices.Action.Cancel -> account.cancelPlan()
                            else -> account.resumePlan()
                        }
                        account.refreshPlan()
                    } catch (e: Exception) {
                        error = describeError(e, "Couldn't change your plan. Try again.")
                    } finally {
                        busy = false
                    }
                }
            }
            PlanChoices.Action.None -> Unit
        }
    }

    asking?.let { choice ->
        AlertDialog(
            onDismissRequest = { asking = null },
            title = { Text(choice.actionLabel + "?") },
            text = { Text(choice.confirm) },
            confirmButton = {
                TextButton(onClick = { perform(choice) }) {
                    Text(choice.actionLabel, color = if (choice.action == PlanChoices.Action.Cancel) colors.danger else colors.accent)
                }
            },
            dismissButton = { TextButton(onClick = { asking = null }) { Text(stringResource(R.string.ui_not_now)) } },
        )
    }

    SubScreen(title = stringResource(R.string.ui_plans), onBack = onBack) {
        Text(
            stringResource(R.string.ui_every_plan_has_end_to_end),
            color = colors.text2,
            fontSize = 14.sp,
        )
        error?.let { Notice(it) }
        choices.forEach { choice ->
            PlanCard(choice, busy) {
                if (choice.confirm.isEmpty()) perform(choice) else asking = choice
            }
        }
        Text(
            stringResource(R.string.ui_payments_are_handled_by_stripe_in),
            color = colors.text3,
            fontSize = 12.5.sp,
        )
    }
}

@Composable
private fun PlanCard(choice: PlanChoices.Choice, busy: Boolean, onAction: () -> Unit) {
    val colors = RedvaultTheme.colors
    val shape = RoundedCornerShape(18.dp)
    Column(
        Modifier
            .fillMaxWidth()
            .background(if (choice.current) colors.accentSoft else colors.surface, shape)
            .border(1.dp, if (choice.current) colors.accentBorder else colors.border, shape)
            .padding(18.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(choice.name, fontFamily = RedvaultFonts.display, fontSize = 22.sp, fontWeight = FontWeight.Bold, color = colors.text, modifier = Modifier.weight(1f))
            Text(choice.price, fontSize = 20.sp, fontWeight = FontWeight.Bold, color = colors.text)
            if (choice.per.isNotEmpty()) Text(choice.per, fontSize = 12.sp, color = colors.text3, modifier = Modifier.padding(bottom = 3.dp))
        }
        if (choice.status.isNotEmpty()) Text(choice.status, color = colors.accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        choice.includes.forEach { line ->
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                DsIcon(R.drawable.ui_check, colors.accent, 16.dp)
                Text(line, color = colors.text, fontSize = 14.sp)
            }
        }
        if (choice.action != PlanChoices.Action.None) {
            PillButton(
                choice.actionLabel,
                onClick = onAction,
                modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
                style = when (choice.action) {
                    PlanChoices.Action.Cancel -> PillStyle.Danger
                    PlanChoices.Action.Keep -> PillStyle.Secondary
                    else -> PillStyle.Primary
                },
                enabled = !busy,
                height = 46.dp,
                icon = if (choice.action == PlanChoices.Action.Checkout) R.drawable.ui_external else null,
            )
        }
        if (choice.note.isNotEmpty()) Text(choice.note, color = colors.text3, fontSize = 12.5.sp)
    }
}
