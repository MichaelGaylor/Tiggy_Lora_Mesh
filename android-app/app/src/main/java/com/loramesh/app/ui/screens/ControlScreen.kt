package com.loramesh.app.ui.screens

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.loramesh.app.data.*
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.theme.*

@Composable
fun ControlScreen(viewModel: MeshViewModel) {
    val relays by viewModel.relays.collectAsState()
    val sensors by viewModel.sensors.collectAsState()
    val sensorHistory by viewModel.sensorHistory.collectAsState()
    val timers by viewModel.timers.collectAsState()
    val setpoints by viewModel.setpoints.collectAsState()
    val autoPoll by viewModel.autoPoll.collectAsState()
    val config by viewModel.config.collectAsState()
    val nodes by viewModel.nodes.collectAsState()
    val statusLine by viewModel.statusLine.collectAsState()

    // Refresh timers/setpoints on entry
    LaunchedEffect(Unit) {
        viewModel.listTimers()
        viewModel.listSetpoints()
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // ─── Status feedback ──────────────────────────────
        if (statusLine.isNotEmpty()) {
            item {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = if (statusLine.startsWith("ERR")) MeshRed.copy(alpha = 0.15f)
                        else MeshGreen.copy(alpha = 0.15f)
                    )
                ) {
                    Text(
                        statusLine,
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodySmall,
                        color = if (statusLine.startsWith("ERR")) MeshRed else MeshGreen
                    )
                }
            }
        }

        // ─── Relay Header ─────────────────────────────────
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("Relay Control", style = MaterialTheme.typography.headlineMedium, color = MeshCyan)
                IconButton(onClick = { viewModel.refreshPins() }) {
                    Icon(Icons.Default.Refresh, contentDescription = "Refresh", tint = MeshCyan)
                }
            }
        }

        // ─── Relay Cards ──────────────────────────────────
        if (relays.isEmpty()) {
            item { EmptyCard("No relay pins configured", "Tap refresh to load pin list from node") }
        } else {
            items(relays) { relay ->
                RelayCard(relay, timers, viewModel)
            }
        }

        // ─── Active Timers ────────────────────────────────
        if (timers.isNotEmpty()) {
            item {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text("Active Timers", style = MaterialTheme.typography.titleLarge, color = MeshOrange)
                    TextButton(onClick = { viewModel.clearTimers() }) {
                        Text("Clear All", color = MeshRed)
                    }
                }
            }
            items(timers) { timer ->
                TimerCard(timer)
            }
        }

        // ─── Sensors Header ──────────────────────────────
        if (sensors.isNotEmpty()) {
            item {
                Spacer(Modifier.height(4.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text("Sensors", style = MaterialTheme.typography.headlineMedium, color = MeshGreen)
                    IconButton(onClick = { viewModel.pollSensors() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Poll", tint = MeshGreen)
                    }
                }
            }
            items(sensors) { sensor ->
                SensorCard(sensor, sensorHistory[config.nodeId + ":" + sensor.pin], viewModel)
            }
        }

        // ─── Remote Poll ──────────────────────────────────
        if (nodes.isNotEmpty()) {
            item {
                RemotePollCard(nodes, viewModel)
            }
        }

        // ─── Auto-Poll ────────────────────────────────────
        item {
            AutoPollCard(autoPoll, nodes, viewModel)
        }

        // ─── Setpoints ───────────────────────────────────
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("Setpoints", style = MaterialTheme.typography.headlineMedium, color = MeshBlue)
                if (setpoints.isNotEmpty()) {
                    TextButton(onClick = { viewModel.clearSetpoints() }) {
                        Text("Clear All", color = MeshRed)
                    }
                }
            }
        }

        if (setpoints.isNotEmpty()) {
            items(setpoints) { sp -> SetpointCard(sp) }
        }

        item {
            AddSetpointCard(sensors, relays, nodes, config, viewModel)
        }

        // Bottom padding
        item { Spacer(Modifier.height(16.dp)) }
    }
}

// ═══════════════════════════════════════════════════════════════
// Sparkline — Canvas-based mini line chart
// ═══════════════════════════════════════════════════════════════

@Composable
fun Sparkline(
    values: List<Int>,
    modifier: Modifier = Modifier,
    lineColor: Color = MeshCyan,
    fillColor: Color = MeshCyan.copy(alpha = 0.1f)
) {
    Canvas(modifier = modifier) {
        if (values.size < 2) return@Canvas
        val maxVal = values.max().toFloat()
        val minVal = values.min().toFloat()
        val range = (maxVal - minVal).coerceAtLeast(1f)
        val step = size.width / (values.size - 1)

        val path = Path()
        values.forEachIndexed { i, v ->
            val x = i * step
            val y = size.height - ((v - minVal) / range * size.height * 0.9f + size.height * 0.05f)
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }

        // Gradient fill
        val fillPath = Path().apply {
            addPath(path)
            lineTo(size.width, size.height)
            lineTo(0f, size.height)
            close()
        }
        drawPath(fillPath, Brush.verticalGradient(listOf(fillColor, Color.Transparent)))

        // Line
        drawPath(path, lineColor, style = Stroke(width = 2f, cap = StrokeCap.Round))

        // Latest value dot
        val lastX = (values.size - 1) * step
        val lastY = size.height - ((values.last() - minVal) / range * size.height * 0.9f + size.height * 0.05f)
        drawCircle(lineColor, radius = 3f, center = Offset(lastX, lastY))
    }
}

// ═══════════════════════════════════════════════════════════════
// Relay Card (with timer quick-set)
// ═══════════════════════════════════════════════════════════════

@Composable
private fun RelayCard(relay: RelayPin, timers: List<TimerInfo>, viewModel: MeshViewModel) {
    var showTimerDialog by remember { mutableStateOf(false) }
    val hasTimer = timers.any { it.pin == relay.pin }

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
                if (relay.state) Icons.Default.PowerSettingsNew else Icons.Default.PowerOff,
                contentDescription = null,
                tint = if (relay.state) MeshGreen else MeshGrey,
                modifier = Modifier.size(32.dp)
            )
            Spacer(Modifier.width(12.dp))

            Column(modifier = Modifier.weight(1f)) {
                Text(relay.label, style = MaterialTheme.typography.titleMedium)
                Text(
                    "GPIO ${relay.pin} - ${if (relay.state) "ON" else "OFF"}",
                    style = MaterialTheme.typography.bodySmall,
                    color = if (relay.state) MeshGreen else MeshGrey
                )
                if (hasTimer) {
                    Text("Timer active", style = MaterialTheme.typography.labelSmall, color = MeshOrange)
                }
            }

            // Timer button
            IconButton(onClick = { showTimerDialog = true }) {
                Icon(Icons.Default.Timer, contentDescription = "Timer",
                    tint = if (hasTimer) MeshOrange else MeshGrey)
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

    if (showTimerDialog) {
        TimerDialog(
            pin = relay.pin,
            onDismiss = { showTimerDialog = false },
            onSetTimer = { action, seconds ->
                viewModel.setTimer(relay.pin, action, seconds)
                showTimerDialog = false
            },
            onSetPulse = { onSec, offSec, repeats ->
                viewModel.setTimerPulse(relay.pin, onSec, offSec, repeats)
                showTimerDialog = false
            }
        )
    }
}

// ═══════════════════════════════════════════════════════════════
// Timer Dialog
// ═══════════════════════════════════════════════════════════════

@Composable
private fun TimerDialog(
    pin: Int,
    onDismiss: () -> Unit,
    onSetTimer: (String, Int) -> Unit,
    onSetPulse: (Int, Int, Int) -> Unit
) {
    var mode by remember { mutableStateOf("ON") }  // ON, OFF, PULSE
    var seconds by remember { mutableStateOf("60") }
    var onSec by remember { mutableStateOf("5") }
    var offSec by remember { mutableStateOf("5") }
    var repeats by remember { mutableStateOf("10") }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Timer — GPIO $pin") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                // Mode selector
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (m in listOf("ON", "OFF", "PULSE")) {
                        FilterChip(
                            selected = mode == m,
                            onClick = { mode = m },
                            label = { Text(m) }
                        )
                    }
                }

                if (mode == "PULSE") {
                    OutlinedTextField(
                        value = onSec, onValueChange = { onSec = it },
                        label = { Text("ON seconds") }, modifier = Modifier.fillMaxWidth()
                    )
                    OutlinedTextField(
                        value = offSec, onValueChange = { offSec = it },
                        label = { Text("OFF seconds") }, modifier = Modifier.fillMaxWidth()
                    )
                    OutlinedTextField(
                        value = repeats, onValueChange = { repeats = it },
                        label = { Text("Repeats (0=forever)") }, modifier = Modifier.fillMaxWidth()
                    )
                } else {
                    OutlinedTextField(
                        value = seconds, onValueChange = { seconds = it },
                        label = { Text("Delay (seconds)") }, modifier = Modifier.fillMaxWidth()
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                if (mode == "PULSE") {
                    onSetPulse(
                        onSec.toIntOrNull() ?: 5,
                        offSec.toIntOrNull() ?: 5,
                        repeats.toIntOrNull() ?: 10
                    )
                } else {
                    onSetTimer(mode, seconds.toIntOrNull() ?: 60)
                }
            }) { Text("Set") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

// ═══════════════════════════════════════════════════════════════
// Timer Card (active timer display)
// ═══════════════════════════════════════════════════════════════

@Composable
private fun TimerCard(timer: TimerInfo) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MeshOrange.copy(alpha = 0.08f))
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(Icons.Default.Timer, contentDescription = null, tint = MeshOrange, modifier = Modifier.size(24.dp))
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text("GPIO ${timer.pin}", style = MaterialTheme.typography.titleSmall)
                Text(
                    if (timer.isPulse) "Pulse: ${timer.detail}" else "${timer.action} in ${timer.detail}",
                    style = MaterialTheme.typography.bodySmall, color = MeshOrange
                )
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Sensor Card (with sparkline)
// ═══════════════════════════════════════════════════════════════

@Composable
private fun SensorCard(sensor: SensorPin, history: List<SensorReading>?, viewModel: MeshViewModel) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Default.Sensors, contentDescription = null, tint = MeshOrange,
                    modifier = Modifier.size(32.dp))
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(sensor.label, style = MaterialTheme.typography.titleMedium)
                    Text("GPIO ${sensor.pin}", style = MaterialTheme.typography.bodySmall, color = MeshGrey)
                }
                Text("${sensor.value}", style = MaterialTheme.typography.headlineSmall, color = MeshOrange)
                Spacer(Modifier.width(8.dp))
                IconButton(onClick = { viewModel.readSensor(sensor.pin) }) {
                    Icon(Icons.Default.Refresh, contentDescription = "Read", tint = MeshCyan)
                }
            }

            // Sparkline
            if (history != null && history.size >= 2) {
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "${history.minOf { it.value }}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MeshGrey
                    )
                    Sparkline(
                        values = history.map { it.value },
                        modifier = Modifier
                            .weight(1f)
                            .height(40.dp)
                            .padding(horizontal = 4.dp),
                        lineColor = MeshGreen,
                        fillColor = MeshGreen.copy(alpha = 0.1f)
                    )
                    Text(
                        "${history.maxOf { it.value }}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MeshGrey
                    )
                }
                Text(
                    "${history.size} readings",
                    style = MaterialTheme.typography.labelSmall,
                    color = MeshGrey.copy(alpha = 0.6f),
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center
                )
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Remote Poll Card
// ═══════════════════════════════════════════════════════════════

@Composable
private fun RemotePollCard(nodes: List<MeshNode>, viewModel: MeshViewModel) {
    var expanded by remember { mutableStateOf(false) }
    var selectedNode by remember { mutableStateOf("") }

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp)) {
            Text("Remote Sensor Poll", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(modifier = Modifier.weight(1f)) {
                    OutlinedButton(onClick = { expanded = true }, modifier = Modifier.fillMaxWidth()) {
                        Text(if (selectedNode.isEmpty()) "Select node..." else selectedNode)
                    }
                    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                        nodes.filter { it.active }.forEach { node ->
                            DropdownMenuItem(
                                text = { Text("${node.id} (${node.rssi} dBm)") },
                                onClick = { selectedNode = node.id; expanded = false }
                            )
                        }
                    }
                }
                Spacer(Modifier.width(8.dp))
                Button(
                    onClick = { if (selectedNode.isNotEmpty()) viewModel.pollRemote(selectedNode) },
                    enabled = selectedNode.isNotEmpty()
                ) {
                    Text("Poll")
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Auto-Poll Card
// ═══════════════════════════════════════════════════════════════

@Composable
private fun AutoPollCard(autoPoll: AutoPollConfig, nodes: List<MeshNode>, viewModel: MeshViewModel) {
    var expanded by remember { mutableStateOf(false) }
    var target by remember { mutableStateOf(autoPoll.target) }
    var interval by remember { mutableStateOf(autoPoll.interval.toString()) }

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (autoPoll.enabled) MeshGreen.copy(alpha = 0.08f)
            else MaterialTheme.colorScheme.surface
        )
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Default.Autorenew, contentDescription = null,
                    tint = if (autoPoll.enabled) MeshGreen else MeshGrey, modifier = Modifier.size(24.dp))
                Spacer(Modifier.width(8.dp))
                Text("Auto-Poll", style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.weight(1f))
                if (autoPoll.enabled) {
                    Text("${autoPoll.target} / ${autoPoll.interval}s",
                        style = MaterialTheme.typography.bodySmall, color = MeshGreen)
                    Spacer(Modifier.width(8.dp))
                    TextButton(onClick = { viewModel.stopAutoPoll() }) {
                        Text("Stop", color = MeshRed)
                    }
                }
            }

            if (!autoPoll.enabled) {
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(modifier = Modifier.weight(1f)) {
                        OutlinedButton(onClick = { expanded = true }, modifier = Modifier.fillMaxWidth()) {
                            Text(if (target.isEmpty()) "Target node..." else target)
                        }
                        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                            nodes.filter { it.active }.forEach { node ->
                                DropdownMenuItem(
                                    text = { Text(node.id) },
                                    onClick = { target = node.id; expanded = false }
                                )
                            }
                        }
                    }
                    Spacer(Modifier.width(8.dp))
                    OutlinedTextField(
                        value = interval, onValueChange = { interval = it },
                        label = { Text("sec") },
                        modifier = Modifier.width(80.dp)
                    )
                    Spacer(Modifier.width(8.dp))
                    Button(
                        onClick = {
                            val sec = interval.toIntOrNull() ?: 300
                            if (target.isNotEmpty()) viewModel.setAutoPoll(target, sec)
                        },
                        enabled = target.isNotEmpty()
                    ) {
                        Text("Start")
                    }
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Setpoint Card (display existing)
// ═══════════════════════════════════════════════════════════════

@Composable
private fun SetpointCard(sp: SetpointInfo) {
    val opSymbol = when (sp.op) { "GT" -> ">"; "LT" -> "<"; "EQ" -> "="; else -> "?" }

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MeshBlue.copy(alpha = 0.08f))
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(Icons.Default.TrendingUp, contentDescription = null, tint = MeshBlue, modifier = Modifier.size(24.dp))
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    "Sensor GPIO ${sp.sensorPin} $opSymbol ${sp.threshold}",
                    style = MaterialTheme.typography.titleSmall
                )
                Text(
                    "-> Node ${sp.targetNode} GPIO ${sp.relayPin} = ${if (sp.action == 1) "ON" else "OFF"}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MeshBlue
                )
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Add Setpoint Card
// ═══════════════════════════════════════════════════════════════

@Composable
private fun AddSetpointCard(
    sensors: List<SensorPin>,
    relays: List<RelayPin>,
    nodes: List<MeshNode>,
    config: NodeConfig,
    viewModel: MeshViewModel
) {
    var showForm by remember { mutableStateOf(false) }

    if (!showForm) {
        OutlinedButton(
            onClick = { showForm = true },
            modifier = Modifier.fillMaxWidth()
        ) {
            Icon(Icons.Default.Add, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Add Setpoint Rule")
        }
        return
    }

    var sensorPin by remember { mutableStateOf(sensors.firstOrNull()?.pin?.toString() ?: "") }
    var op by remember { mutableStateOf("GT") }
    var threshold by remember { mutableStateOf("2000") }
    var targetNode by remember { mutableStateOf(config.nodeId) }
    var relayPin by remember { mutableStateOf(relays.firstOrNull()?.pin?.toString() ?: "") }
    var action by remember { mutableStateOf("1") }

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("New Setpoint", style = MaterialTheme.typography.titleMedium, color = MeshBlue)

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = sensorPin, onValueChange = { sensorPin = it },
                    label = { Text("Sensor Pin") }, modifier = Modifier.weight(1f)
                )
                // Op selector
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp),
                    modifier = Modifier.weight(1f)) {
                    for (o in listOf("GT", "LT", "EQ")) {
                        FilterChip(
                            selected = op == o,
                            onClick = { op = o },
                            label = { Text(when (o) { "GT" -> ">"; "LT" -> "<"; else -> "=" }) }
                        )
                    }
                }
                OutlinedTextField(
                    value = threshold, onValueChange = { threshold = it },
                    label = { Text("Threshold") }, modifier = Modifier.weight(1f)
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = targetNode, onValueChange = { targetNode = it },
                    label = { Text("Target Node") }, modifier = Modifier.weight(1f)
                )
                OutlinedTextField(
                    value = relayPin, onValueChange = { relayPin = it },
                    label = { Text("Relay Pin") }, modifier = Modifier.weight(1f)
                )
                // Action selector
                Row(modifier = Modifier.weight(1f)) {
                    FilterChip(selected = action == "1", onClick = { action = "1" },
                        label = { Text("ON") })
                    Spacer(Modifier.width(4.dp))
                    FilterChip(selected = action == "0", onClick = { action = "0" },
                        label = { Text("OFF") })
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { showForm = false }, modifier = Modifier.weight(1f)) {
                    Text("Cancel")
                }
                Button(
                    onClick = {
                        viewModel.setSetpoint(
                            sensorPin.toIntOrNull() ?: return@Button,
                            op,
                            threshold.toIntOrNull() ?: return@Button,
                            targetNode,
                            relayPin.toIntOrNull() ?: return@Button,
                            action.toIntOrNull() ?: 1
                        )
                        showForm = false
                    },
                    modifier = Modifier.weight(1f)
                ) {
                    Text("Create")
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Empty state card
// ═══════════════════════════════════════════════════════════════

@Composable
private fun EmptyCard(title: String, subtitle: String) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Icon(Icons.Default.SettingsRemote, contentDescription = null,
                modifier = Modifier.size(48.dp), tint = MeshGrey)
            Spacer(Modifier.height(8.dp))
            Text(title, color = MeshGrey)
            Spacer(Modifier.height(4.dp))
            Text(subtitle, style = MaterialTheme.typography.bodySmall,
                color = MeshGrey.copy(alpha = 0.6f))
        }
    }
}
