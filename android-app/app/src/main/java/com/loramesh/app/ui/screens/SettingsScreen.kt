package com.loramesh.app.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.loramesh.app.data.SfChangePhase
import com.loramesh.app.data.TelegramConfig
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.theme.*
import kotlin.math.roundToInt

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(viewModel: MeshViewModel) {
    val config by viewModel.config.collectAsState()
    val nodes by viewModel.nodes.collectAsState()
    val telegramConfig by viewModel.telegramConfig.collectAsState()
    val sfChange by viewModel.sfChange.collectAsState()
    val soundEnabled by viewModel.soundEnabled.collectAsState()
    val sharePosition by viewModel.sharePosition.collectAsState()

    var editId by remember { mutableStateOf(config.nodeId) }
    var editKey by remember { mutableStateOf(config.aesKey) }
    var showKeyDialog by remember { mutableStateOf(false) }
    var showPinDialog by remember { mutableStateOf(false) }
    var newPin by remember { mutableStateOf("") }

    // Show PIN dialog automatically if node is in setup mode
    LaunchedEffect(config.setupMode) {
        if (config.setupMode) showPinDialog = true
    }

    // Telegram state
    var tgEnabled by remember { mutableStateOf(telegramConfig.enabled) }
    var tgToken by remember { mutableStateOf(telegramConfig.botToken) }
    var tgChatId by remember { mutableStateOf(telegramConfig.chatId) }
    var tgTokenVisible by remember { mutableStateOf(false) }
    var tgTestResult by remember { mutableStateOf<Boolean?>(null) }
    var tgTesting by remember { mutableStateOf(false) }
    var tgExpanded by remember { mutableStateOf(telegramConfig.enabled) }

    // Sync when config updates from node
    LaunchedEffect(config.nodeId) { editId = config.nodeId }
    LaunchedEffect(telegramConfig) {
        tgEnabled = telegramConfig.enabled
        tgToken = telegramConfig.botToken
        tgChatId = telegramConfig.chatId
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            "Settings",
            style = MaterialTheme.typography.headlineMedium,
            color = MeshCyan
        )

        // ── Setup Mode Banner ─────────────────────────────────
        if (config.setupMode) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = Color.Red.copy(alpha = 0.15f))
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        "Setup Required",
                        style = MaterialTheme.typography.titleMedium,
                        color = Color.Red,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        "Set a new BLE PIN before configuring this node. Default PIN (123456) must be changed.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = newPin,
                        onValueChange = { if (it.length <= 6 && it.all { c -> c.isDigit() }) newPin = it },
                        label = { Text("New 6-digit PIN") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                    Spacer(Modifier.height(8.dp))
                    Button(
                        onClick = {
                            if (newPin.length == 6 && newPin != "123456") {
                                viewModel.sendCommand("BLEPIN,$newPin")
                                newPin = ""
                            }
                        },
                        enabled = newPin.length == 6 && newPin != "123456",
                        colors = ButtonDefaults.buttonColors(containerColor = MeshGreen)
                    ) {
                        Text("Set PIN")
                    }
                    if (newPin == "123456") {
                        Text(
                            "Cannot use the default PIN",
                            style = MaterialTheme.typography.bodySmall,
                            color = Color.Red
                        )
                    }
                }
            }
        }

        // ── Node Info ─────────────────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("Node Info", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
                Spacer(Modifier.height(8.dp))
                if (config.boardName.isNotEmpty()) {
                    Row {
                        Text("Board: ", color = MeshGrey)
                        Text(config.boardName)
                    }
                }
                Row {
                    Text("Frequency: ", color = MeshGrey)
                    Text("${config.frequency} MHz")
                }
            }
        }

        // ── Spreading Factor ─────────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text(
                    "Spreading Factor",
                    style = MaterialTheme.typography.titleMedium,
                    color = MeshCyan
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    "Current SF: ${config.spreadingFactor}  " +
                        "(Higher = longer range but slower, Lower = faster but shorter range)",
                    style = MaterialTheme.typography.bodySmall,
                    color = MeshGrey
                )
                Spacer(Modifier.height(12.dp))

                when (sfChange.phase) {
                    SfChangePhase.IDLE -> {
                        // SF slider + initiate button
                        var sliderSF by remember { mutableStateOf(config.spreadingFactor.toFloat()) }
                        LaunchedEffect(config.spreadingFactor) {
                            sliderSF = config.spreadingFactor.toFloat()
                        }

                        Text(
                            "New SF: ${sliderSF.roundToInt()}",
                            style = MaterialTheme.typography.bodyMedium
                        )
                        Slider(
                            value = sliderSF,
                            onValueChange = { sliderSF = it },
                            valueRange = 7f..12f,
                            steps = 4,
                            colors = SliderDefaults.colors(
                                thumbColor = MeshCyan,
                                activeTrackColor = MeshCyan
                            )
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween
                        ) {
                            Text("SF7", style = MaterialTheme.typography.labelSmall, color = MeshGrey)
                            Text("SF12", style = MaterialTheme.typography.labelSmall, color = MeshGrey)
                        }
                        Spacer(Modifier.height(8.dp))
                        Button(
                            onClick = { viewModel.initiateSpreadingFactorChange(sliderSF.roundToInt()) },
                            modifier = Modifier.fillMaxWidth(),
                            enabled = sliderSF.roundToInt() != config.spreadingFactor,
                            colors = ButtonDefaults.buttonColors(containerColor = MeshOrange)
                        ) {
                            Icon(Icons.Default.CellTower, contentDescription = null)
                            Spacer(Modifier.width(8.dp))
                            Text("Change SF on All Nodes")
                        }
                        if (sliderSF.roundToInt() == config.spreadingFactor) {
                            Text(
                                "Select a different SF to begin the change",
                                style = MaterialTheme.typography.labelSmall,
                                color = MeshGrey
                            )
                        }
                    }

                    SfChangePhase.COLLECTING -> {
                        // Phase 1: Show ACK progress
                        Text(
                            "Changing to SF${sfChange.targetSF} — waiting for node responses...",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MeshOrange
                        )
                        Spacer(Modifier.height(8.dp))

                        val acked = sfChange.ackedNodes.size
                        val expected = sfChange.expectedNodes.size
                        if (expected > 0) {
                            LinearProgressIndicator(
                                progress = { acked.toFloat() / expected.toFloat() },
                                modifier = Modifier.fillMaxWidth(),
                                color = MeshGreen,
                                trackColor = MeshGrey.copy(alpha = 0.3f)
                            )
                            Spacer(Modifier.height(4.dp))
                            Text(
                                "$acked / $expected nodes ready",
                                style = MaterialTheme.typography.bodySmall,
                                color = if (acked == expected) MeshGreen else MeshOrange
                            )
                        } else {
                            Text(
                                "$acked nodes responded",
                                style = MaterialTheme.typography.bodySmall,
                                color = MeshOrange
                            )
                        }

                        Spacer(Modifier.height(8.dp))

                        // Per-node status
                        for (nodeId in sfChange.expectedNodes) {
                            val responded = sfChange.ackedNodes.contains(nodeId)
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(vertical = 2.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Icon(
                                    if (responded) Icons.Default.CheckCircle else Icons.Default.Schedule,
                                    contentDescription = null,
                                    tint = if (responded) MeshGreen else MeshGrey,
                                    modifier = Modifier.size(16.dp)
                                )
                                Spacer(Modifier.width(8.dp))
                                Text(
                                    nodeId,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = if (responded) MeshGreen else MeshGrey
                                )
                            }
                        }

                        // Show any unexpected responders
                        val unexpected = sfChange.ackedNodes.filter { it !in sfChange.expectedNodes }
                        for (nodeId in unexpected) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(vertical = 2.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Icon(
                                    Icons.Default.CheckCircle,
                                    contentDescription = null,
                                    tint = MeshGreen,
                                    modifier = Modifier.size(16.dp)
                                )
                                Spacer(Modifier.width(8.dp))
                                Text(
                                    "$nodeId (new)",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MeshGreen
                                )
                            }
                        }

                        Spacer(Modifier.height(12.dp))

                        // Warning if not all responded
                        if (expected > 0 && acked < expected) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                modifier = Modifier.padding(bottom = 8.dp)
                            ) {
                                Icon(
                                    Icons.Default.Warning,
                                    contentDescription = null,
                                    tint = MeshOrange,
                                    modifier = Modifier.size(16.dp)
                                )
                                Spacer(Modifier.width(6.dp))
                                Text(
                                    "Not all nodes responded. Committing now may strand unresponsive nodes.",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MeshOrange
                                )
                            }
                        }

                        // Commit / Cancel buttons
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            Button(
                                onClick = { viewModel.commitSpreadingFactorChange() },
                                modifier = Modifier.weight(1f),
                                enabled = acked > 0,
                                colors = ButtonDefaults.buttonColors(containerColor = MeshGreen)
                            ) {
                                Icon(Icons.Default.Check, contentDescription = null)
                                Spacer(Modifier.width(4.dp))
                                Text("Commit")
                            }
                            OutlinedButton(
                                onClick = { viewModel.cancelSpreadingFactorChange() },
                                modifier = Modifier.weight(1f)
                            ) {
                                Text("Cancel", color = MeshRed)
                            }
                        }
                    }

                    SfChangePhase.COMMITTED -> {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            CircularProgressIndicator(
                                modifier = Modifier.size(20.dp),
                                strokeWidth = 2.dp,
                                color = MeshCyan
                            )
                            Spacer(Modifier.width(12.dp))
                            Text(
                                "Switching all nodes to SF${sfChange.targetSF}...",
                                color = MeshCyan
                            )
                        }
                    }

                    SfChangePhase.COMPLETE -> {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Icon(
                                Icons.Default.CheckCircle,
                                contentDescription = null,
                                tint = MeshGreen,
                                modifier = Modifier.size(20.dp)
                            )
                            Spacer(Modifier.width(8.dp))
                            Text(
                                "All nodes switched to SF${sfChange.targetSF}",
                                color = MeshGreen
                            )
                        }
                        Spacer(Modifier.height(8.dp))
                        TextButton(
                            onClick = { viewModel.cancelSpreadingFactorChange() }
                        ) { Text("Dismiss", color = MeshCyan) }
                    }

                    SfChangePhase.FAILED -> {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Icon(
                                Icons.Default.Error,
                                contentDescription = null,
                                tint = MeshRed,
                                modifier = Modifier.size(20.dp)
                            )
                            Spacer(Modifier.width(8.dp))
                            Text("SF change failed", color = MeshRed)
                        }
                        Spacer(Modifier.height(8.dp))
                        TextButton(
                            onClick = { viewModel.cancelSpreadingFactorChange() }
                        ) { Text("Dismiss", color = MeshCyan) }
                    }
                }
            }
        }

        // ── GPS Position Sharing ──────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("GPS Position", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
                Spacer(Modifier.height(8.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("Share Position", style = MaterialTheme.typography.bodyMedium)
                        Text(
                            "Broadcast your phone's GPS location to the mesh every 60 seconds.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MeshGrey
                        )
                    }
                    Switch(
                        checked = sharePosition,
                        onCheckedChange = { viewModel.setSharePosition(it) },
                        colors = SwitchDefaults.colors(checkedThumbColor = MeshGreen)
                    )
                }
            }
        }
        Spacer(Modifier.height(8.dp))

        // ── Power Mode (always show — don't hide based on boardName) ──
        if (config.boardName != "T-Deck") {
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        "Power Mode",
                        style = MaterialTheme.typography.titleMedium,
                        color = MeshCyan
                    )
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "Current: ${config.powerMode}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                    Spacer(Modifier.height(8.dp))
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text("Solar Mode", style = MaterialTheme.typography.bodyMedium)
                            Text(
                                "Turns off OLED, enables light sleep between packets. BLE and radio stay active.",
                                style = MaterialTheme.typography.bodySmall,
                                color = MeshGrey
                            )
                        }
                        Switch(
                            checked = config.powerMode == "SOLAR",
                            onCheckedChange = {
                                if (it) viewModel.setPowerMode(true)
                                else viewModel.setPowerMode(false)
                            },
                            colors = SwitchDefaults.colors(checkedThumbColor = MeshOrange)
                        )
                    }
                    Spacer(Modifier.height(12.dp))
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text("GPS", style = MaterialTheme.typography.bodyMedium)
                            Text(
                                if (config.gpsStatus == "OFF") "GPS module disabled. Enable to broadcast position."
                                else if (config.gpsStatus == "FIX") "GPS has satellite fix."
                                else "GPS enabled, searching for satellites...",
                                style = MaterialTheme.typography.bodySmall,
                                color = MeshGrey
                            )
                        }
                        Switch(
                            checked = config.gpsStatus != "OFF",
                            onCheckedChange = { viewModel.setGpsEnabled(it) },
                            colors = SwitchDefaults.colors(checkedThumbColor = MeshGreen)
                        )
                    }
                }
            }
        }

        // ── Device Management ────────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("Device Management", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
                Spacer(Modifier.height(8.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Button(
                        onClick = { viewModel.sendCommand("REBOOT") },
                        colors = ButtonDefaults.buttonColors(containerColor = MeshOrange),
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("Reboot")
                    }
                    Button(
                        onClick = { viewModel.sendCommand("EEPROM,RESET") },
                        colors = ButtonDefaults.buttonColors(containerColor = Color.Red),
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("Factory Reset")
                    }
                }
                Text(
                    "Reboot restarts the node. Factory Reset wipes all settings and restarts.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MeshGrey
                )
            }
        }

        // ── Node ID ───────────────────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("Node ID", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = editId,
                        onValueChange = { if (it.length <= 4) editId = it.uppercase() },
                        modifier = Modifier.weight(1f),
                        label = { Text("4-char hex ID") },
                        singleLine = true,
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedBorderColor = MeshCyan,
                            cursorColor = MeshCyan
                        )
                    )
                    Spacer(Modifier.width(8.dp))
                    Button(
                        onClick = { viewModel.setNodeId(editId) },
                        colors = ButtonDefaults.buttonColors(containerColor = MeshCyan)
                    ) { Text("Set") }
                }
            }
        }

        // ── AES Key ───────────────────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("Encryption Key", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
                Spacer(Modifier.height(4.dp))
                Text(
                    "Everyone in your group must use the same 16-character key. " +
                    "Messages from other groups with different keys cannot be read.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MeshGrey
                )
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = editKey,
                        onValueChange = { if (it.length <= 16) editKey = it },
                        modifier = Modifier.weight(1f),
                        label = { Text("16-char key") },
                        singleLine = true,
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedBorderColor = MeshCyan,
                            cursorColor = MeshCyan
                        )
                    )
                    Spacer(Modifier.width(8.dp))
                    Button(
                        onClick = { if (editKey.length == 16) showKeyDialog = true },
                        enabled = editKey.length == 16,
                        colors = ButtonDefaults.buttonColors(containerColor = MeshOrange)
                    ) { Text("Set") }
                }
                if (editKey.isNotEmpty() && editKey.length != 16) {
                    Text(
                        "${editKey.length}/16 characters",
                        style = MaterialTheme.typography.labelSmall,
                        color = MeshRed
                    )
                }
            }
        }

        // ── Save / Refresh ────────────────────────────────────
        Button(
            onClick = { viewModel.saveConfig() },
            modifier = Modifier.fillMaxWidth(),
            colors = ButtonDefaults.buttonColors(containerColor = MeshGreen)
        ) {
            Icon(Icons.Default.Save, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Save to EEPROM")
        }

        OutlinedButton(
            onClick = { viewModel.requestStatus() },
            modifier = Modifier.fillMaxWidth()
        ) {
            Icon(Icons.Default.Refresh, contentDescription = null, tint = MeshCyan)
            Spacer(Modifier.width(8.dp))
            Text("Refresh Status from Node")
        }

        // ── Telegram Bridge ───────────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(
                containerColor = if (tgEnabled)
                    MeshCyan.copy(alpha = 0.08f)
                else
                    MaterialTheme.colorScheme.surface
            )
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                // Header row with enable toggle
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        Icons.Default.Send,
                        contentDescription = null,
                        tint = if (tgEnabled) MeshCyan else MeshGrey
                    )
                    Spacer(Modifier.width(8.dp))
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            "Telegram Alerts",
                            style = MaterialTheme.typography.titleMedium,
                            color = if (tgEnabled) MeshCyan else MeshGrey
                        )
                        Text(
                            "Forward group messages & SOS to a Telegram chat",
                            style = MaterialTheme.typography.bodySmall,
                            color = MeshGrey
                        )
                    }
                    Switch(
                        checked = tgEnabled,
                        onCheckedChange = {
                            tgEnabled = it
                            tgExpanded = it
                            tgTestResult = null
                        },
                        colors = SwitchDefaults.colors(checkedThumbColor = MeshCyan)
                    )
                }

                if (tgEnabled) {
                    Spacer(Modifier.height(16.dp))
                    Divider(color = MeshCyan.copy(alpha = 0.2f))
                    Spacer(Modifier.height(16.dp))

                    // Step 1
                    StepHeader(1, "Create a Telegram Bot")
                    Text(
                        "Open Telegram and search for @BotFather\n" +
                        "Send /newbot and follow the instructions.\n" +
                        "Copy the bot token it gives you (looks like 123456789:ABCdef...).",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                    Spacer(Modifier.height(12.dp))

                    OutlinedTextField(
                        value = tgToken,
                        onValueChange = { tgToken = it.trim() },
                        modifier = Modifier.fillMaxWidth(),
                        label = { Text("Bot Token") },
                        placeholder = { Text("123456789:ABCdefGHI...") },
                        singleLine = true,
                        visualTransformation = if (tgTokenVisible)
                            VisualTransformation.None
                        else
                            PasswordVisualTransformation(),
                        trailingIcon = {
                            IconButton(onClick = { tgTokenVisible = !tgTokenVisible }) {
                                Icon(
                                    if (tgTokenVisible) Icons.Default.VisibilityOff
                                    else Icons.Default.Visibility,
                                    contentDescription = "Toggle visibility",
                                    tint = MeshGrey
                                )
                            }
                        },
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedBorderColor = MeshCyan,
                            cursorColor = MeshCyan
                        )
                    )

                    Spacer(Modifier.height(16.dp))

                    // Step 2
                    StepHeader(2, "Get your Chat ID")
                    Text(
                        "Add your bot to a Telegram group (or use it in a private chat).\n" +
                        "Then open a browser and go to:\n" +
                        "https://api.telegram.org/bot<YOUR_TOKEN>/getUpdates\n" +
                        "Send a message in the chat, refresh the page, and look for " +
                        "\"chat\":{\"id\": — that number is your Chat ID.\n" +
                        "For a group it will be negative e.g. -1001234567890",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                    Spacer(Modifier.height(8.dp))

                    OutlinedTextField(
                        value = tgChatId,
                        onValueChange = { tgChatId = it.trim() },
                        modifier = Modifier.fillMaxWidth(),
                        label = { Text("Chat ID") },
                        placeholder = { Text("-1001234567890") },
                        singleLine = true,
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedBorderColor = MeshCyan,
                            cursorColor = MeshCyan
                        )
                    )

                    Spacer(Modifier.height(16.dp))

                    // Step 3
                    StepHeader(3, "Save and Test")
                    Text(
                        "Tap Save below, then Test Connection to send a test message " +
                        "to your Telegram chat. If it arrives, you're all set.\n\n" +
                        "What gets forwarded:\n" +
                        "  • Group broadcasts from the mesh\n" +
                        "  • SOS emergencies (with maps link if GPS available)\n\n" +
                        "What stays private:\n" +
                        "  • Direct messages between two nodes",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )

                    Spacer(Modifier.height(12.dp))

                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Button(
                            onClick = {
                                val cfg = TelegramConfig(
                                    enabled = tgEnabled,
                                    botToken = tgToken,
                                    chatId = tgChatId
                                )
                                viewModel.saveTelegramConfig(cfg)
                            },
                            modifier = Modifier.weight(1f),
                            colors = ButtonDefaults.buttonColors(containerColor = MeshGreen)
                        ) {
                            Icon(Icons.Default.Save, contentDescription = null)
                            Spacer(Modifier.width(4.dp))
                            Text("Save")
                        }

                        OutlinedButton(
                            onClick = {
                                tgTesting = true
                                tgTestResult = null
                                val cfg = TelegramConfig(
                                    enabled = true,
                                    botToken = tgToken,
                                    chatId = tgChatId
                                )
                                viewModel.saveTelegramConfig(cfg)
                                viewModel.testTelegram { ok ->
                                    tgTesting = false
                                    tgTestResult = ok
                                }
                            },
                            modifier = Modifier.weight(1f),
                            enabled = tgToken.isNotBlank() && tgChatId.isNotBlank() && !tgTesting
                        ) {
                            if (tgTesting) {
                                CircularProgressIndicator(
                                    modifier = Modifier.size(16.dp),
                                    strokeWidth = 2.dp,
                                    color = MeshCyan
                                )
                            } else {
                                Icon(
                                    Icons.Default.Send,
                                    contentDescription = null,
                                    tint = MeshCyan
                                )
                            }
                            Spacer(Modifier.width(4.dp))
                            Text("Test", color = MeshCyan)
                        }
                    }

                    // Test result feedback
                    tgTestResult?.let { ok ->
                        Spacer(Modifier.height(8.dp))
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.Center,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Icon(
                                if (ok) Icons.Default.CheckCircle else Icons.Default.Error,
                                contentDescription = null,
                                tint = if (ok) MeshGreen else MeshRed,
                                modifier = Modifier.size(18.dp)
                            )
                            Spacer(Modifier.width(6.dp))
                            Text(
                                if (ok)
                                    "Test message sent! Check your Telegram chat."
                                else
                                    "Failed — check your bot token and chat ID.",
                                color = if (ok) MeshGreen else MeshRed,
                                style = MaterialTheme.typography.bodySmall
                            )
                        }
                    }
                } else {
                    // Disabled state hint
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "Enable to set up Telegram alerts for your group.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                }
            }
        }

        // ── Notifications ────────────────────────────────────────
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("Notifications", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
                Spacer(Modifier.height(8.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("Message Sound", style = MaterialTheme.typography.bodyMedium)
                        Text(
                            "Play notification sound on incoming messages",
                            style = MaterialTheme.typography.bodySmall,
                            color = MeshGrey
                        )
                    }
                    Switch(
                        checked = soundEnabled,
                        onCheckedChange = { viewModel.toggleSound(it) },
                        colors = SwitchDefaults.colors(checkedThumbColor = MeshCyan)
                    )
                }
            }
        }

        // ── Connection ──────────────────────────────────────────
        if (viewModel.hasSavedDevice()) {
            var showForgetDialog by remember { mutableStateOf(false) }

            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text("Connection", style = MaterialTheme.typography.titleMedium, color = MeshCyan)
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "Auto-reconnects to ${viewModel.savedDeviceName()} on launch",
                        style = MaterialTheme.typography.bodySmall,
                        color = MeshGrey
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedButton(
                        onClick = { showForgetDialog = true },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.Default.BluetoothDisabled, contentDescription = null, tint = MeshRed)
                        Spacer(Modifier.width(8.dp))
                        Text("Forget Device", color = MeshRed)
                    }
                }
            }

            if (showForgetDialog) {
                AlertDialog(
                    onDismissRequest = { showForgetDialog = false },
                    title = { Text("Forget Device?") },
                    text = {
                        Text(
                            "The app will no longer auto-connect to ${viewModel.savedDeviceName()}. " +
                            "You will need to scan and select a device next time."
                        )
                    },
                    confirmButton = {
                        TextButton(onClick = {
                            viewModel.forgetDevice()
                            showForgetDialog = false
                        }) { Text("Forget", color = MeshRed) }
                    },
                    dismissButton = {
                        TextButton(onClick = { showForgetDialog = false }) { Text("Cancel") }
                    }
                )
            }
        }
    }

    // Confirm key change dialog
    if (showKeyDialog) {
        AlertDialog(
            onDismissRequest = { showKeyDialog = false },
            title = { Text("Change Encryption Key?") },
            text = {
                Text(
                    "All nodes in your group must use the same key. " +
                    "Changing it will prevent communication with nodes using the old key."
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.setAesKey(editKey)
                    showKeyDialog = false
                }) { Text("Change Key", color = MeshOrange) }
            },
            dismissButton = {
                TextButton(onClick = { showKeyDialog = false }) { Text("Cancel") }
            }
        )
    }
}

@Composable
private fun StepHeader(number: Int, title: String) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(bottom = 4.dp)
    ) {
        Surface(
            shape = MaterialTheme.shapes.small,
            color = MeshCyan,
            modifier = Modifier.size(22.dp)
        ) {
            Box(contentAlignment = Alignment.Center) {
                Text(
                    "$number",
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold,
                    color = androidx.compose.ui.graphics.Color.Black
                )
            }
        }
        Spacer(Modifier.width(8.dp))
        Text(title, style = MaterialTheme.typography.titleSmall, color = MeshCyan)
    }
    Spacer(Modifier.height(4.dp))
}
