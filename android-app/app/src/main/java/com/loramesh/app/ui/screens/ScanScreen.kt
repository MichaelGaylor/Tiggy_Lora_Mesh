package com.loramesh.app.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.BluetoothSearching
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.loramesh.app.ble.BleManager
import com.loramesh.app.ble.ConnectionState
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.theme.MeshCyan
import com.loramesh.app.ui.theme.MeshGrey
import com.loramesh.app.ui.theme.MeshRed

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ScanScreen(viewModel: MeshViewModel) {
    val state by viewModel.connectionState.collectAsState()
    val devices by viewModel.discoveredDevices.collectAsState()
    val autoConnectFailed by viewModel.autoConnectFailed.collectAsState()
    val isScanning = state == ConnectionState.SCANNING
    val isConnecting = state == ConnectionState.CONNECTING

    // Auto-connect on first composition if we have a saved device
    var autoConnectAttempted by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        if (!autoConnectAttempted && viewModel.hasSavedDevice()) {
            autoConnectAttempted = true
            viewModel.autoConnect()
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        // Header
        Text(
            "Connect to Node",
            style = MaterialTheme.typography.headlineMedium,
            color = MeshCyan
        )
        Spacer(Modifier.height(8.dp))
        Text(
            "Scan for nearby LoRa mesh nodes with BLE",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)
        )
        Spacer(Modifier.height(16.dp))

        // Auto-connecting state
        if (isConnecting && viewModel.hasSavedDevice() && !autoConnectFailed) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator(color = MeshCyan)
                    Spacer(Modifier.height(16.dp))
                    Text(
                        "Connecting to ${viewModel.savedDeviceName()}...",
                        style = MaterialTheme.typography.titleMedium,
                        color = MeshCyan
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "Auto-reconnecting to last device",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                    Spacer(Modifier.height(24.dp))
                    TextButton(onClick = {
                        viewModel.disconnect()
                    }) {
                        Text("Cancel", color = MeshRed)
                    }
                }
            }
            return
        }

        // Auto-connect failed message
        if (autoConnectFailed) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = MeshRed.copy(alpha = 0.1f)
                )
            ) {
                Row(
                    modifier = Modifier.padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        "Could not reconnect to ${viewModel.savedDeviceName()}",
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshRed
                    )
                    TextButton(onClick = { viewModel.autoConnect() }) {
                        Text("Retry", color = MeshCyan)
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
        }

        // Scan button
        Button(
            onClick = { if (isScanning) viewModel.stopScan() else viewModel.startScan() },
            modifier = Modifier.fillMaxWidth(),
            colors = ButtonDefaults.buttonColors(
                containerColor = if (isScanning) MaterialTheme.colorScheme.error else MeshCyan
            )
        ) {
            Icon(
                if (isScanning) Icons.Default.Stop else Icons.Default.BluetoothSearching,
                contentDescription = null,
                modifier = Modifier.size(20.dp)
            )
            Spacer(Modifier.width(8.dp))
            Text(if (isScanning) "Stop Scanning" else "Scan for Nodes")
        }

        if (isScanning) {
            Spacer(Modifier.height(8.dp))
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth(), color = MeshCyan)
        }

        Spacer(Modifier.height(16.dp))

        // Device list
        if (devices.isEmpty() && !isScanning) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(
                        Icons.Default.Bluetooth,
                        contentDescription = null,
                        modifier = Modifier.size(64.dp),
                        tint = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.3f)
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "No devices found",
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
                    )
                    Text(
                        "Make sure your node is powered on",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.3f)
                    )
                }
            }
        } else {
            LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(devices) { device ->
                    DeviceCard(device) {
                        viewModel.connect(device.address)
                    }
                }
            }
        }
    }
}

@Composable
private fun DeviceCard(device: BleManager.ScannedDevice, onClick: () -> Unit) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                Icons.Default.Bluetooth,
                contentDescription = null,
                tint = MeshCyan,
                modifier = Modifier.size(32.dp)
            )
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(device.name, style = MaterialTheme.typography.titleMedium)
                Text(
                    device.address,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
                )
            }
            Text(
                "${device.rssi} dBm",
                style = MaterialTheme.typography.bodySmall,
                color = when {
                    device.rssi > -60 -> MaterialTheme.colorScheme.secondary
                    device.rssi > -80 -> MaterialTheme.colorScheme.primary
                    else -> MaterialTheme.colorScheme.error
                }
            )
        }
    }
}
