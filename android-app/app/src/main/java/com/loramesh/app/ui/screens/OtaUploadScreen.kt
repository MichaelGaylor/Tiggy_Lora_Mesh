package com.loramesh.app.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.OtaState

// ═══════════════════════════════════════════════════════════════
// OTA Upload Screen
// ═══════════════════════════════════════════════════════════════
// Drives the FW,* command set on the firmware (commits cf19463 +
// 25e0a0b on Lora-mesh dev) to flash a firmware.bin to the
// currently-connected node over BLE. Whole flow is in the
// MeshViewModel; this screen is a thin UI over its state flows.

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun OtaUploadScreen(viewModel: MeshViewModel) {
    val otaState by viewModel.otaState.collectAsState()
    val otaProgress by viewModel.otaProgress.collectAsState()
    val otaError by viewModel.otaLastError.collectAsState()
    val fileName by viewModel.otaSelectedFileName.collectAsState()
    val fileSha by viewModel.otaSelectedFileSha.collectAsState()

    // File picker — Storage Access Framework, no permission required on
    // modern Android. User picks any .bin file the system can read.
    val pickFileLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) viewModel.selectOtaFile(uri)
    }

    val canPickFile = otaState !in setOf(
        OtaState.HASHING, OtaState.UPLOADING, OtaState.VERIFYING, OtaState.REBOOTING
    )
    val canUpload = fileName != null && fileSha != null &&
        otaState in setOf(OtaState.IDLE, OtaState.DONE)
    val isInFlight = otaState in setOf(
        OtaState.UPLOADING, OtaState.VERIFYING, OtaState.REBOOTING
    )

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // Header
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.SystemUpdate, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text(
                "Firmware Update",
                style = MaterialTheme.typography.titleLarge
            )
        }

        // File-picker card
        ElevatedCard(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Firmware file", style = MaterialTheme.typography.titleMedium)
                Text(
                    fileName ?: "No file selected",
                    style = MaterialTheme.typography.bodyMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (fileSha != null) {
                    Text(
                        "SHA-256: ${fileSha!!.take(16)}…",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
                Button(
                    onClick = { pickFileLauncher.launch(arrayOf("*/*")) },
                    enabled = canPickFile,
                    modifier = Modifier.align(Alignment.End)
                ) {
                    Icon(Icons.Default.FolderOpen, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(if (fileName == null) "Pick .bin" else "Change file")
                }
            }
        }

        // Status card — shows what's happening right now
        ElevatedCard(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Status", style = MaterialTheme.typography.titleMedium)
                Text(
                    text = when (otaState) {
                        OtaState.IDLE      -> if (fileName == null) "Pick a firmware file to begin."
                                              else "Ready to upload."
                        OtaState.HASHING   -> "Computing SHA-256…"
                        OtaState.UPLOADING -> "Uploading…"
                        OtaState.VERIFYING -> "Verifying on the node…"
                        OtaState.REBOOTING -> "Node rebooting. Reconnect will happen automatically."
                        OtaState.DONE      -> "Update complete. The node is running the new firmware."
                        OtaState.ERROR     -> "Update failed."
                    },
                    style = MaterialTheme.typography.bodyLarge
                )

                if (isInFlight && otaProgress != null) {
                    val p = otaProgress!!
                    val fraction = if (p.totalChunks > 0) p.currentChunk.toFloat() / p.totalChunks else 0f
                    LinearProgressIndicator(
                        progress = { fraction },
                        modifier = Modifier.fillMaxWidth()
                    )
                    Text(
                        "Chunk ${p.currentChunk} / ${p.totalChunks}  " +
                            "(${p.bytesSent / 1024} KB / ${p.totalBytes / 1024} KB)",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }

                if (otaState == OtaState.ERROR && otaError != null) {
                    AssistChip(
                        onClick = { viewModel.clearOtaError() },
                        label = { Text(otaError!!, maxLines = 3, overflow = TextOverflow.Ellipsis) },
                        leadingIcon = { Icon(Icons.Default.Error, contentDescription = null) },
                    )
                }
            }
        }

        // Action buttons
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Button(
                onClick = { viewModel.startOtaUpload() },
                enabled = canUpload,
                modifier = Modifier.weight(1f),
            ) {
                Icon(Icons.Default.CloudUpload, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Upload")
            }
            OutlinedButton(
                onClick = { viewModel.cancelOta() },
                enabled = isInFlight,
                modifier = Modifier.weight(1f),
            ) {
                Icon(Icons.Default.Cancel, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Cancel")
            }
        }

        // Information footer — the firmware-side safety story so the
        // user understands what's about to happen
        ElevatedCard(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("What this does", style = MaterialTheme.typography.titleMedium)
                Text(
                    "• Uploads firmware via BLE to the connected node only.",
                    style = MaterialTheme.typography.bodySmall
                )
                Text(
                    "• The node verifies the SHA-256 hash before activating the new image.",
                    style = MaterialTheme.typography.bodySmall
                )
                Text(
                    "• If the new image fails to boot, the bootloader automatically reverts.",
                    style = MaterialTheme.typography.bodySmall
                )
                Text(
                    "• Mesh + BLE beacon scanning pause for the duration of the upload " +
                        "(~15-18 min for an 800 KB image) and resume after reboot.",
                    style = MaterialTheme.typography.bodySmall
                )
                Text(
                    "• Keep the phone within BLE range of the node throughout the upload.",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
    }
}
