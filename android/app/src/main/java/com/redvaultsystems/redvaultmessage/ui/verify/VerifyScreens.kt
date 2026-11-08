// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.verify

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.os.PersistableBundle
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.crypto.SafetyNumberQr
import com.redvaultsystems.redvaultmessage.data.api.TransparencyEventDto
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.VerificationStatus
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.NoticeTone
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay

/**
 * Port of ui/src/screens/Verify.tsx, plus QR codes so two phones can verify
 * in person by scanning each other instead of reading 60 digits aloud.
 */
@Composable
fun VerifyScreen(
    conversation: Conversation,
    onBack: () -> Unit,
    onSetVerified: (Boolean) -> Unit,
    top: @Composable () -> Unit = {},
    extra: @Composable () -> Unit = {},
) {
    val colors = RedvaultTheme.colors
    val safetyNumber = conversation.meta.safetyNumber
    val qr = remember(safetyNumber) { qrBitmap(SafetyNumberQr.encode(safetyNumber)) }
    var scanResult by remember { mutableStateOf<SafetyNumberQr.Result?>(null) }
    var scannedText by remember { mutableStateOf("") }
    val context = LocalContext.current
    var copied by remember(safetyNumber) { mutableStateOf(false) }

    val scan = rememberLauncherForActivityResult(ScanContract()) { result ->
        val contents = result.contents ?: return@rememberLauncherForActivityResult
        scannedText = contents
        val outcome = SafetyNumberQr.compare(contents, safetyNumber)
        scanResult = outcome
        if (outcome == SafetyNumberQr.Result.Match) onSetVerified(true)
    }

    SubScreen(title = "Verify safety number", onBack = onBack) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Avatar(avatarUrl = conversation.avatarUrl, seed = conversation.handle, size = 64.dp)
            Text(conversation.handle, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.titleMedium, color = colors.text)
            Text(
                when {
                    conversation.keyChanged -> "Safety number changed"
                    conversation.verified -> "Verified on this phone"
                    else -> "Not verified"
                },
                color = if (conversation.verified) colors.accent else colors.warn,
                fontWeight = FontWeight.SemiBold,
            )
        }

        // Their key is different from the one this phone first saw.
        if (conversation.keyChanged) {
            Notice(
                "Safety number changed. ${conversation.handle} has a new key: a new device, a reinstall, or someone in the middle. " +
                    "Compare the new number before you trust this chat.",
            )
        }

        top()

        when (scanResult) {
            SafetyNumberQr.Result.Match -> Notice("It matches. ${conversation.handle} is verified on this phone.", NoticeTone.Success)
            SafetyNumberQr.Result.Mismatch -> Notice(
                "Those numbers don't match. Someone may be intercepting this chat, or one of you has a new key. " +
                    "Don't send anything sensitive until you've worked out why.",
            )
            SafetyNumberQr.Result.NotASafetyCode -> Notice(
                if (scannedText.trim().startsWith("RVDK1:")) {
                    "That's their device-key code. Scan it with the button under Device keys, further down."
                } else {
                    "That isn't a safety-number code. Scan the code on their Verify screen."
                },
            )
            null -> {}
        }

        Text(
            "Meet ${conversation.handle} in person and scan the code on their Verify screen, or let them scan yours. " +
                "Or compare the 60 digits below over a call you trust.",
            color = colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )

        // Dark on white regardless of theme, so any camera can read it.
        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
            Image(
                bitmap = qr,
                contentDescription = "Your safety-number QR code for this chat",
                modifier = Modifier.size(220.dp).background(androidx.compose.ui.graphics.Color.White, RoundedCornerShape(12.dp)).padding(8.dp),
            )
        }
        PrimaryButton(text = "Scan their code", enabled = true) {
            scan.launch(
                ScanOptions()
                    .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                    .setPrompt("Scan the code on ${conversation.handle}'s Verify screen")
                    .setBeepEnabled(false)
                    .setOrientationLocked(false),
            )
        }

        SafetyNumberGrid(safetyNumber)

        PillButton(
            if (copied) "Copied" else "Copy number",
            {
                copySafetyNumber(context, safetyNumber.joinToString(" "))
                copied = true
            },
            Modifier.fillMaxWidth(),
            style = PillStyle.Secondary,
            icon = if (copied) R.drawable.ui_check else R.drawable.ui_copy,
        )

        if (conversation.verified) {
            TextButton(onClick = { onSetVerified(false) }) { Text("Mark as not verified", color = colors.warn) }
        } else {
            OutlinedButton(onClick = { onSetVerified(true) }, shape = MaterialTheme.shapes.small, modifier = Modifier.fillMaxWidth()) {
                Text("The digits match, mark as verified")
            }
        }

        // Other numbers (device keys) come after this one and its buttons.
        extra()
    }
}

@Composable
private fun SafetyNumberGrid(groups: List<String>) {
    Column(
        Modifier.fillMaxWidth().background(RedvaultTheme.colors.surface, RoundedCornerShape(12.dp)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        groups.chunked(4).forEach { row ->
            Text(
                row.joinToString("   "),
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.titleMedium,
                textAlign = TextAlign.Center,
                color = RedvaultTheme.colors.text,
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

/**
 * Copies the 60 digits, as one line of 12 groups like the desktop. Not a secret, but it's marked sensitive like
 * the desktop's copy (kept out of clipboard history), so the preview and keyboards don't keep who you compare with.
 */
private fun copySafetyNumber(context: Context, digits: String) {
    val clip = ClipData.newPlainText("Safety number", digits)
    // ClipDescription.EXTRA_IS_SENSITIVE (API 33); the same key is honored on earlier versions too.
    clip.description.extras = PersistableBundle().apply { putBoolean("android.content.extra.IS_SENSITIVE", true) }
    context.getSystemService(ClipboardManager::class.java).setPrimaryClip(clip)
}

internal fun qrBitmap(content: String, size: Int = 512): ImageBitmap {
    val matrix = QRCodeWriter().encode(content, BarcodeFormat.QR_CODE, size, size, mapOf(EncodeHintType.MARGIN to 1))
    val pixels = IntArray(size * size) { i -> if (matrix[i % size, i / size]) 0xFF000000.toInt() else 0xFFFFFFFF.toInt() }
    return Bitmap.createBitmap(pixels, size, size, Bitmap.Config.ARGB_8888).asImageBitmap()
}

/** Port of ui/src/screens/VerificationDetails.tsx: public hashes and ids only, never keys. */
@Composable
fun MessageDetailsScreen(message: Message?, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    SubScreen(title = "Verification details", onBack = onBack) {
        val security = message?.security
        if (security == null) {
            Text("No verification record for this message yet.", color = colors.text2)
            return@SubScreen
        }
        DetailRow("Public transaction", security.publicTransactionId ?: "—")
        DetailRow("Protocol", security.protocolVersion?.let { "E2EE-v$it" } ?: "—")
        DetailRow("Local commitment", security.localCommitment ?: "—", mono = true)
        DetailRow("Network commitment", security.networkCommitment ?: "—", mono = true)
        when (security.verification) {
            VerificationStatus.Verified -> Notice("MATCH ✓", NoticeTone.Success)
            VerificationStatus.Failed -> Notice("MISMATCH ✗: don't trust this message's delivery status.")
            VerificationStatus.Unavailable -> Notice("Unavailable")
        }
        Text(
            "A matching commitment proves the ciphertext this app relayed is byte-identical to what your phone encrypted, " +
                "and that the server independently recomputed the same hash from what it actually stored. It does not prove " +
                "the encryption itself has no vulnerabilities, that either device is uncompromised, that the recipient is who " +
                "you believe, or that this server is running unmodified published source.",
            color = colors.text3,
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun DetailRow(label: String, value: String, mono: Boolean = false) {
    Column(Modifier.fillMaxWidth().background(RedvaultTheme.colors.surface, RoundedCornerShape(12.dp)).padding(12.dp)) {
        Text(label.uppercase(), color = RedvaultTheme.colors.text3, style = MaterialTheme.typography.labelSmall)
        Text(value, color = RedvaultTheme.colors.text, fontFamily = if (mono) FontFamily.Monospace else FontFamily.Default)
    }
}

/**
 * Port of ui/src/screens/TransparencyFeed.tsx. Public, no account needed.
 * The web streams updates; here the list refreshes every few seconds.
 */
@Composable
fun TransparencyFeedScreen(loadFeed: suspend (limit: Int) -> List<TransparencyEventDto>, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    var events by remember { mutableStateOf<List<TransparencyEventDto>>(emptyList()) }
    var loaded by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        while (true) {
            try {
                val latest = loadFeed(20)
                val known = events.mapTo(HashSet()) { it.publicTransactionId }
                events = (latest.filter { it.publicTransactionId !in known } + events).take(MAX_EVENTS)
                error = null
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                error = "Couldn't load the feed. Retrying…"
            }
            loaded = true
            delay(FEED_REFRESH_MS)
        }
    }

    SubScreen(title = "Live encrypted network", onBack = onBack) {
        Text(
            "A public record of messages this server actually relayed. Each entry shows a commitment the server computed, " +
                "proof the ciphertext wasn't altered, and only for senders who opted in, the ciphertext itself. " +
                "Never plaintext. Never who sent it, who received it, or which chat it belongs to.",
            color = colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Box(Modifier.size(8.dp).background(if (error == null && loaded) colors.accent else colors.text3, CircleShape))
            Text(if (error == null && loaded) "Updating every few seconds" else "Connecting…", color = colors.text3, style = MaterialTheme.typography.labelSmall)
        }
        error?.let { Notice(it) }
        if (loaded && error == null && events.isEmpty()) {
            Text("No public transactions yet.", color = colors.text3)
        }
        events.forEach { FeedCard(it) }
    }
}

@Composable
private fun FeedCard(event: TransparencyEventDto) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Row {
            Text(
                "TX-" + event.publicTransactionId.removePrefix("tx_").take(6).uppercase(),
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.SemiBold,
                color = colors.text,
                modifier = Modifier.weight(1f),
            )
            Text(if (event.relayStatus == "relayed") "Relayed ✓" else event.relayStatus, color = colors.accent, style = MaterialTheme.typography.labelMedium)
        }
        FeedLine("Public tx", event.publicTransactionId)
        FeedLine("Protocol", "E2EE-v${event.protocolVersion}")
        FeedLine("Commitment", event.commitment)
        when {
            event.expired -> Text("Message expired. Ciphertext no longer stored.", color = colors.text3, style = MaterialTheme.typography.labelSmall)
            event.ciphertext != null -> {
                Text("Encrypted content (unreadable without the recipient's key)", color = colors.text3, style = MaterialTheme.typography.labelSmall)
                Text(event.ciphertext, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.labelSmall, color = colors.text2)
            }
        }
    }
}

@Composable
private fun FeedLine(label: String, value: String) {
    Column {
        Text(label, color = RedvaultTheme.colors.text3, style = MaterialTheme.typography.labelSmall)
        Text(value, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.labelSmall, color = RedvaultTheme.colors.text2)
    }
}

private const val MAX_EVENTS = 50
private const val FEED_REFRESH_MS = 5_000L
