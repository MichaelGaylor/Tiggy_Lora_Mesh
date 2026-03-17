package com.loramesh.app.ui.screens

import android.graphics.Paint
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.unit.dp
import com.loramesh.app.data.NodePosition
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.theme.*
import kotlin.math.*

@Composable
fun MapScreen(viewModel: MeshViewModel) {
    val positions by viewModel.positions.collectAsState()
    val trackingTarget by viewModel.trackingTarget.collectAsState()
    val sharePosition by viewModel.sharePosition.collectAsState()

    Column(modifier = Modifier.fillMaxSize()) {
        // Header
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "Mesh Map",
                style = MaterialTheme.typography.headlineMedium,
                color = MeshCyan,
                modifier = Modifier.weight(1f)
            )
            // Share position toggle
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "Share GPS",
                    style = MaterialTheme.typography.bodySmall,
                    color = MeshGrey
                )
                Spacer(Modifier.width(4.dp))
                Switch(
                    checked = sharePosition,
                    onCheckedChange = { viewModel.setSharePosition(it) },
                    colors = SwitchDefaults.colors(checkedThumbColor = MeshGreen)
                )
            }
        }

        if (positions.isEmpty()) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(
                        Icons.Default.LocationOff,
                        contentDescription = null,
                        modifier = Modifier.size(64.dp),
                        tint = MeshGrey.copy(alpha = 0.4f)
                    )
                    Spacer(Modifier.height(8.dp))
                    Text("No positions received yet", color = MeshGrey)
                    Text(
                        "Nodes with GPS will appear when they broadcast POS",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey.copy(alpha = 0.6f)
                    )
                    if (!sharePosition) {
                        Spacer(Modifier.height(12.dp))
                        Text(
                            "Enable 'Share GPS' to broadcast your position",
                            style = MaterialTheme.typography.bodySmall,
                            color = MeshCyan
                        )
                    }
                }
            }
        } else {
            // Summary
            Text(
                "${positions.size} nodes with positions",
                style = MaterialTheme.typography.bodySmall,
                color = MeshGreen,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)
            )

            // Tracking info
            trackingTarget?.let { targetId ->
                val dist = viewModel.getDistanceTo(targetId)
                val bearing = viewModel.getBearingTo(targetId)
                if (dist != null && bearing != null) {
                    Card(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 16.dp, vertical = 4.dp),
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.surface
                        )
                    ) {
                        Row(
                            modifier = Modifier.padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(
                                Icons.Default.NearMe,
                                contentDescription = null,
                                tint = MeshCyan,
                                modifier = Modifier.size(32.dp)
                            )
                            Spacer(Modifier.width(12.dp))
                            Column {
                                Text(
                                    "Tracking: $targetId",
                                    style = MaterialTheme.typography.titleSmall,
                                    color = MeshCyan
                                )
                                Text(
                                    "Distance: ${formatDistance(dist)}  Bearing: ${bearing.toInt()}°",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MeshGrey
                                )
                            }
                            Spacer(Modifier.weight(1f))
                            IconButton(onClick = { viewModel.setTrackingTarget(null) }) {
                                Icon(Icons.Default.Close, "Stop tracking", tint = MeshGrey)
                            }
                        }
                    }
                }
            }

            // Map canvas
            Canvas(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(16.dp)
            ) {
                drawNodeMap(positions, trackingTarget)
            }
        }
    }
}

private fun DrawScope.drawNodeMap(
    positions: Map<String, NodePosition>,
    trackingTarget: String?
) {
    if (positions.isEmpty()) return

    // Calculate bounds
    val lats = positions.values.map { it.lat }
    val lons = positions.values.map { it.lon }
    val minLat = lats.min()
    val maxLat = lats.max()
    val minLon = lons.min()
    val maxLon = lons.max()

    // Add padding to bounds
    val latRange = max(maxLat - minLat, 0.001) * 1.3
    val lonRange = max(maxLon - minLon, 0.001) * 1.3
    val centerLat = (minLat + maxLat) / 2
    val centerLon = (minLon + maxLon) / 2

    val w = size.width
    val h = size.height
    val padding = 60f

    fun latLonToXY(lat: Double, lon: Double): Offset {
        val x = padding + ((lon - (centerLon - lonRange / 2)) / lonRange) * (w - 2 * padding)
        val y = padding + ((1 - (lat - (centerLat - latRange / 2)) / latRange)) * (h - 2 * padding)
        return Offset(x.toFloat(), y.toFloat())
    }

    // Draw grid lines
    val gridColor = Color(0xFF333333)
    for (i in 0..4) {
        val x = padding + i * (w - 2 * padding) / 4
        val y = padding + i * (h - 2 * padding) / 4
        drawLine(gridColor, Offset(x, padding), Offset(x, h - padding), 0.5f)
        drawLine(gridColor, Offset(padding, y), Offset(w - padding, y), 0.5f)
    }

    // Draw connections between nodes (simple lines)
    val nodeList = positions.values.toList()
    if (nodeList.size > 1) {
        for (i in nodeList.indices) {
            for (j in i + 1 until nodeList.size) {
                val p1 = latLonToXY(nodeList[i].lat, nodeList[i].lon)
                val p2 = latLonToXY(nodeList[j].lat, nodeList[j].lon)
                drawLine(
                    Color(0x40888888), p1, p2, 1f
                )
            }
        }
    }

    // Draw nodes
    val textPaint = Paint().apply {
        color = android.graphics.Color.WHITE
        textSize = 32f
        textAlign = Paint.Align.CENTER
        isAntiAlias = true
    }

    positions.forEach { (nodeId, pos) ->
        val pt = latLonToXY(pos.lat, pos.lon)
        val isTracked = nodeId == trackingTarget
        val nodeColor = if (isTracked) Color(MeshCyan.toArgb()) else Color(MeshGreen.toArgb())
        val radius = if (isTracked) 18f else 14f

        // Outer glow for tracked node
        if (isTracked) {
            drawCircle(nodeColor.copy(alpha = 0.2f), radius * 2.5f, pt)
        }

        // Node dot
        drawCircle(nodeColor, radius, pt)

        // Label
        drawIntoCanvas { canvas ->
            textPaint.color = if (isTracked) MeshCyan.toArgb() else android.graphics.Color.WHITE
            canvas.nativeCanvas.drawText(nodeId, pt.x, pt.y - radius - 8, textPaint)
            // Coordinates below
            textPaint.textSize = 22f
            textPaint.color = android.graphics.Color.GRAY
            canvas.nativeCanvas.drawText(
                "%.4f, %.4f".format(pos.lat, pos.lon),
                pt.x, pt.y + radius + 22, textPaint
            )
            textPaint.textSize = 32f
        }
    }
}

private fun formatDistance(metres: Float): String = when {
    metres < 1000 -> "${metres.toInt()}m"
    else -> "%.1fkm".format(metres / 1000)
}
