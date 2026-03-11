package com.loramesh.app.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.loramesh.app.data.RelayPin
import com.loramesh.app.data.SensorPin
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.theme.*

@Composable
fun ControlScreen(viewModel: MeshViewModel) {
    val relays by viewModel.relays.collectAsState()
    val sensors by viewModel.sensors.collectAsState()

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // Header
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    "Relay Control",
                    style = MaterialTheme.typography.headlineMedium,
                    color = MeshCyan
                )
                IconButton(onClick = { viewModel.refreshPins() }) {
                    Icon(Icons.Default.Refresh, contentDescription = "Refresh", tint = MeshCyan)
                }
            }
        }

        // Relay cards
        if (relays.isEmpty()) {
            item {
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(24.dp),
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        Icon(
                            Icons.Default.SettingsRemote,
                            contentDescription = null,
                            modifier = Modifier.size(48.dp),
                            tint = MeshGrey
                        )
                        Spacer(Modifier.height(8.dp))
                        Text("No relay pins configured", color = MeshGrey)
                        Spacer(Modifier.height(4.dp))
                        Text(
                            "Tap refresh to load pin list from node",
                            style = MaterialTheme.typography.bodySmall,
                            color = MeshGrey.copy(alpha = 0.6f)
                        )
                    }
                }
            }
        } else {
            items(relays) { relay ->
                RelayCard(relay, viewModel)
            }
        }

        // Sensor section
        if (sensors.isNotEmpty()) {
            item {
                Spacer(Modifier.height(8.dp))
                Text(
                    "Sensors",
                    style = MaterialTheme.typography.headlineMedium,
                    color = MeshGreen
                )
            }
            items(sensors) { sensor ->
                SensorCard(sensor, viewModel)
            }
        }
    }
}

@Composable
private fun RelayCard(relay: RelayPin, viewModel: MeshViewModel) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Status icon
            Icon(
                if (relay.state) Icons.Default.PowerSettingsNew else Icons.Default.PowerOff,
                contentDescription = null,
                tint = if (relay.state) MeshGreen else MeshGrey,
                modifier = Modifier.size(32.dp)
            )
            Spacer(Modifier.width(12.dp))

            // Label and pin number
            Column(modifier = Modifier.weight(1f)) {
                Text(relay.label, style = MaterialTheme.typography.titleMedium)
                Text(
                    "GPIO ${relay.pin} - ${if (relay.state) "ON" else "OFF"}",
                    style = MaterialTheme.typography.bodySmall,
                    color = if (relay.state) MeshGreen else MeshGrey
                )
            }

            // Pulse button
            OutlinedButton(
                onClick = { viewModel.pulseRelay(relay.pin) },
                modifier = Modifier.height(36.dp),
                contentPadding = PaddingValues(horizontal = 12.dp)
            ) {
                Text("Pulse", style = MaterialTheme.typography.labelSmall)
            }

            Spacer(Modifier.width(8.dp))

            // Toggle switch
            Switch(
                checked = relay.state,
                onCheckedChange = { viewModel.toggleRelay(relay.pin) },
                colors = SwitchDefaults.colors(
                    checkedThumbColor = MeshGreen,
                    checkedTrackColor = MeshGreen.copy(alpha = 0.3f)
                )
            )
        }
    }
}

@Composable
private fun SensorCard(sensor: SensorPin, viewModel: MeshViewModel) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                Icons.Default.Sensors,
                contentDescription = null,
                tint = MeshOrange,
                modifier = Modifier.size(32.dp)
            )
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(sensor.label, style = MaterialTheme.typography.titleMedium)
                Text(
                    "GPIO ${sensor.pin}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MeshGrey
                )
            }
            Text(
                "${sensor.value}",
                style = MaterialTheme.typography.headlineSmall,
                color = MeshOrange
            )
            Spacer(Modifier.width(8.dp))
            IconButton(onClick = { viewModel.readSensor(sensor.pin) }) {
                Icon(Icons.Default.Refresh, contentDescription = "Read", tint = MeshCyan)
            }
        }
    }
}
