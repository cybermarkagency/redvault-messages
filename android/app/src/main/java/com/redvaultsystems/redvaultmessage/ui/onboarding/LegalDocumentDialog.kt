// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.onboarding

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.redvaultsystems.redvaultmessage.data.model.LegalBlock
import com.redvaultsystems.redvaultmessage.data.model.LegalDocument
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** The Terms of Service or Privacy Policy, full screen; closing returns to the form as it was. */
@Composable
internal fun LegalDocumentDialog(document: LegalDocument, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val blocks by produceState<List<LegalBlock>?>(null, document) {
        value = withContext(Dispatchers.IO) {
            runCatching { context.assets.open(document.assetName).bufferedReader().use { it.readText() } }
                .map(LegalDocument::parse)
                .getOrDefault(emptyList())
        }
    }
    val title = (blocks?.firstOrNull { it is LegalBlock.Title } as? LegalBlock.Title)?.text.orEmpty()

    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.fillMaxSize().background(colors.bg).systemBarsPadding().padding(horizontal = 20.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    title,
                    fontFamily = RedvaultFonts.display,
                    fontSize = 22.sp,
                    fontWeight = FontWeight.Bold,
                    color = colors.text,
                    modifier = Modifier.weight(1f),
                )
                Box(
                    Modifier
                        .size(36.dp)
                        .clip(CircleShape)
                        .background(colors.surface2)
                        .clickable(role = Role.Button, onClick = onDismiss)
                        .semantics { contentDescription = "Close" },
                    contentAlignment = Alignment.Center,
                ) { Text("×", color = colors.text2, fontSize = 20.sp) }
            }
            LazyColumn(Modifier.weight(1f).fillMaxWidth().padding(top = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                itemsIndexed(blocks.orEmpty()) { _, block ->
                    when (block) {
                        is LegalBlock.Title -> Unit
                        is LegalBlock.Heading -> Text(
                            block.text,
                            color = colors.text,
                            fontSize = 16.sp,
                            fontWeight = FontWeight.Bold,
                            modifier = Modifier.padding(top = 14.dp),
                        )
                        is LegalBlock.Subheading -> Text(
                            block.text,
                            color = colors.text,
                            fontSize = 15.sp,
                            fontWeight = FontWeight.SemiBold,
                            modifier = Modifier.padding(top = 6.dp),
                        )
                        is LegalBlock.Paragraph -> Text(block.text, color = colors.text2, fontSize = 14.sp, lineHeight = 21.sp)
                        is LegalBlock.Bullets -> ListItems(block.items) { "•" }
                        is LegalBlock.Numbered -> ListItems(block.items) { "${it + 1}." }
                    }
                }
            }
            PillButton("Done", onDismiss, Modifier.fillMaxWidth().padding(top = 8.dp).clip(RoundedCornerShape(50)))
        }
    }
}

@Composable
private fun ListItems(items: List<String>, marker: (Int) -> String) {
    val colors = RedvaultTheme.colors
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        items.forEachIndexed { index, item ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(marker(index), color = colors.text3, fontSize = 14.sp, modifier = Modifier.padding(start = 4.dp))
                Text(item, color = colors.text2, fontSize = 14.sp, lineHeight = 21.sp, modifier = Modifier.weight(1f))
            }
        }
    }
}
