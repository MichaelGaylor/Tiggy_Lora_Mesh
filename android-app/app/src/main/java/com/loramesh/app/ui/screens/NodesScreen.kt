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
import com.loramesh.app.data.MeshNode
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.theme.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NodesScreen(viewModel: MeshViewModel) {
    val nodes by viewModel.nodes.collectAsState()

    // Request nodes on entry and periodically
    LaunchedEffect(Unit) {
        viewModel.requestNodes()
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Header
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "Mesh Nodes",
                style = MaterialTheme.typography.headlineMedium,
                color = MeshCyan,
                modifier = Modifier.weight(1f)
            )
            // Refresh button
            IconButton(onClick = { viewModel.requestNodes() }) {
                Icon(
                    Icons.Default.Refresh,
                    contentDescription = "Refresh",
                    tint = MeshCyan
                )
            }
        }

        if (nodes.isEmpty()) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(
                        Icons.Default.CellTower,
                        contentDescription = null,
                        modifier = Modifier.size(64.dp),
                        tint = MeshGrey.copy(alpha = 0.4f)
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "No nodes discovered yet",
                        color = MeshGrey
                    )
                    Text(
                        "Nodes appear as they send heartbeats",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey.copy(alpha = 0.6f)
                    )
                }
            }
        } else {
            // Summary
            val active = nodes.count { it.active }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 4.dp)
            ) {
                Text(
                    "$active active",
                    style = MaterialTheme.typography.bodySmall,
                    color = MeshGreen
                )
                Spacer(Modifier.width(12.dp))
                if (nodes.size - active > 0) {
                    Text(
                        "${nodes.size - active} stale",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                }
            }
            Spacer(Modifier.height(4.dp))

            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(horizontal = 16.dp, vertical = 4.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                // Active nodes first, then stale
                val sorted = nodes.sortedWith(
                    compareByDescending<MeshNode> { it.active }
                        .thenBy { it.hops }
                        .thenByDescending { it.rssi }
                )
                items(sorted) { node ->
                    NodeCard(node)
                }
            }
        }
    }
}

@Composable
private fun NodeCard(node: MeshNode) {
    val statusColor = if (node.active) MeshGreen else MeshGrey
    val rssiColor = when {
        !node.active -> MeshGrey
        node.rssi > -80 -> MeshGreen
        node.rssi > -100 -> MeshOrange
        else -> MeshRed
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (node.active)
                MaterialTheme.colorScheme.surface
            else
                MaterialTheme.colorScheme.surface.copy(alpha = 0.5f)
        )
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Status icon
            Icon(
                if (node.active) Icons.Default.CellTower else Icons.Default.SignalCellularOff,
                contentDescription = null,
                tint = statusColor,
                modifier = Modifier.size(28.dp)
            )
            Spacer(Modifier.width(12.dp))

            // Node info
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    node.id,
                    style = MaterialTheme.typography.titleMedium,
                    color = if (node.active) MaterialTheme.colorScheme.onSurface else MeshGrey
                )
                Row {
                    if (node.hops > 0) {
                        Text(
                            "${node.hops} hop${if (node.hops > 1) "s" else ""}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MeshGrey
                        )
                        Spacer(Modifier.width(8.dp))
                    }
                    Text(
                        formatAge(node.age),
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                }
            }

            // Signal strength
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    "${node.rssi} dBm",
                    style = MaterialTheme.typography.bodyMedium,
                    color = rssiColor
                )
                // Signal bar visual
                SignalBars(node.rssi, node.active)
            }
        }
    }
}

@Composable
private fun SignalBars(rssi: Int, active: Boolean) {
    val bars = when {
        !active -> 0
        rssi > -60 -> 4
        rssi > -75 -> 3
        rssi > -90 -> 2
        rssi > -105 -> 1
        else -> 0
    }
    Row(
        horizontalArrangement = Arrangement.spacedBy(2.dp),
        verticalAlignment = Alignment.Bottom,
        modifier = Modifier.height(16.dp)
    ) {
        for (i in 1..4) {
            val height = (4 + i * 3).dp
            val color = if (i <= bars) {
                when (bars) {
                    4 -> MeshGreen
                    3 -> MeshGreen
                    2 -> MeshOrange
                    else -> MeshRed
                }
            } else {
                MeshGrey.copy(alpha = 0.2f)
            }
            Surface(
                modifier = Modifier
                    .width(4.dp)
                    .height(height),
                color = color,
                shape = MaterialTheme.shapes.extraSmall
            ) {}
        }
    }
}

private fun formatAge(seconds: Int): String = when {
    seconds < 60 -> "${seconds}s ago"
    seconds < 3600 -> "${seconds / 60}m ago"
    else -> "${seconds / 3600}h ago"
}
