package com.loramesh.app.ui

import android.Manifest
import android.app.Application
import android.content.pm.PackageManager
import android.location.LocationManager
import android.media.RingtoneManager
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.loramesh.app.ble.BleManager
import com.loramesh.app.ble.ConnectionState
import com.loramesh.app.data.*
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch

// ═══════════════════════════════════════════════════════════════
// ViewModel - bridges BLE manager with UI state
// ═══════════════════════════════════════════════════════════════

private const val MAX_SENSOR_HISTORY = 60 // readings per sensor key

class MeshViewModel(app: Application) : AndroidViewModel(app) {

    val ble = BleManager(app.applicationContext)
    val telegram = TelegramBridge(app.applicationContext)

    // ─── UI State ────────────────────────────────────────────
    private val _messages = MutableStateFlow<List<ChatMessage>>(emptyList())
    val messages: StateFlow<List<ChatMessage>> = _messages

    private val _nodes = MutableStateFlow<List<MeshNode>>(emptyList())
    val nodes: StateFlow<List<MeshNode>> = _nodes

    private val _relays = MutableStateFlow<List<RelayPin>>(emptyList())
    val relays: StateFlow<List<RelayPin>> = _relays

    private val _sensors = MutableStateFlow<List<SensorPin>>(emptyList())
    val sensors: StateFlow<List<SensorPin>> = _sensors

    private val _config = MutableStateFlow(NodeConfig())
    val config: StateFlow<NodeConfig> = _config

    // Telegram config state (so UI reacts to changes)
    private val _telegramConfig = MutableStateFlow(telegram.config)
    val telegramConfig: StateFlow<TelegramConfig> = _telegramConfig

    // ─── Message ACK tracking ─────────────────────────────
    private val pendingAcks = mutableMapOf<String, Long>() // mid → timestamp

    // ─── Spreading Factor change state ──────────────────
    private val _sfChange = MutableStateFlow(SfChangeState())
    val sfChange: StateFlow<SfChangeState> = _sfChange

    // ─── NODES bulk response accumulator ────────────────
    private var nodeListBuffer: MutableList<MeshNode>? = null

    // ─── Message sound ──────────────────────────────────
    private val _soundEnabled = MutableStateFlow(true)
    val soundEnabled: StateFlow<Boolean> = _soundEnabled

    // ─── Sensor history (sparklines) ─────────────────────
    // Key: "nodeId:pin", Value: last N readings
    private val _sensorHistory = MutableStateFlow<Map<String, List<SensorReading>>>(emptyMap())
    val sensorHistory: StateFlow<Map<String, List<SensorReading>>> = _sensorHistory

    // ─── Timers ──────────────────────────────────────────
    private val _timers = MutableStateFlow<List<TimerInfo>>(emptyList())
    val timers: StateFlow<List<TimerInfo>> = _timers

    // ─── Setpoints ───────────────────────────────────────
    private val _setpoints = MutableStateFlow<List<SetpointInfo>>(emptyList())
    val setpoints: StateFlow<List<SetpointInfo>> = _setpoints

    // ─── Auto-poll ───────────────────────────────────────
    private val _autoPoll = MutableStateFlow(AutoPollConfig())
    val autoPoll: StateFlow<AutoPollConfig> = _autoPoll

    // ─── Node positions (from POS broadcasts) ────────────
    private val _positions = MutableStateFlow<Map<String, NodePosition>>(emptyMap())
    val positions: StateFlow<Map<String, NodePosition>> = _positions

    // ─── Position sharing (phone GPS → mesh) ───────────
    private val _sharePosition = MutableStateFlow(false)
    val sharePosition: StateFlow<Boolean> = _sharePosition
    private var positionSharingJob: kotlinx.coroutines.Job? = null

    // ─── Tracking target ────────────────────────────────
    private val _trackingTarget = MutableStateFlow<String?>(null)
    val trackingTarget: StateFlow<String?> = _trackingTarget

    // ─── Status feedback line ────────────────────────────
    private val _statusLine = MutableStateFlow("")
    val statusLine: StateFlow<String> = _statusLine

    val connectionState = ble.connectionState
    val connectedDeviceName = ble.connectedDeviceName
    val discoveredDevices = ble.discoveredDevices
    val autoConnectFailed = ble.autoConnectFailed

    init {
        // Parse incoming data lines from BLE
        viewModelScope.launch {
            ble.incomingData.collect { line ->
                parseLine(line)
            }
        }
    }

    // ─── Auto-reconnect ───────────────────────────────────
    fun hasSavedDevice(): Boolean = ble.getSavedDeviceMac() != null
    fun savedDeviceName(): String = ble.getSavedDeviceName()

    fun autoConnect(): Boolean = ble.autoConnect()

    fun forgetDevice() {
        ble.forgetDevice()
        ble.disconnect()
    }

    // ─── BLE Actions ─────────────────────────────────────────
    fun startScan() = ble.startScan()
    fun stopScan() = ble.stopScan()
    fun connect(address: String) = ble.connect(address)
    fun disconnect() = ble.disconnect()

    // ─── Messaging ───────────────────────────────────────────
    fun sendMessage(target: String, text: String) {
        ble.sendMessage(target, text)
        _messages.value = _messages.value + ChatMessage(
            from = _config.value.nodeId,
            text = text,
            isOutgoing = true,
            deliveryStatus = DeliveryStatus.SENDING
        )
    }

    fun sendBroadcast(text: String) {
        ble.sendBroadcast(text)
        _messages.value = _messages.value + ChatMessage(
            from = _config.value.nodeId,
            text = text,
            isOutgoing = true
            // broadcasts don't ACK — no deliveryStatus
        )
    }

    // ─── SOS Emergency ────────────────────────────────────────
    fun sendSOS() {
        val ctx = getApplication<Application>()
        var sosText = "SOS"
        // Try to get last known GPS location
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION)
            == PackageManager.PERMISSION_GRANTED) {
            try {
                val lm = ctx.getSystemService(android.content.Context.LOCATION_SERVICE) as LocationManager
                val loc = lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                    ?: lm.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
                if (loc != null) {
                    sosText = "SOS,%.6f,%.6f".format(loc.latitude, loc.longitude)
                }
            } catch (_: Exception) { }
        }
        ble.sendBroadcast(sosText)
        _messages.value = _messages.value + ChatMessage(
            from = _config.value.nodeId,
            text = sosText,
            isOutgoing = true
        )
        // Also forward to Telegram immediately
        viewModelScope.launch {
            telegram.forward(_config.value.nodeId, sosText, 0)
        }
    }

    // ─── Relay Control ───────────────────────────────────────
    fun toggleRelay(pin: Int) {
        val current = _relays.value.find { it.pin == pin }
        val newState = !(current?.state ?: false)
        ble.setRelay(pin, newState)
    }

    fun pulseRelay(pin: Int, ms: Int = 1000) = ble.pulseRelay(pin, ms)
    fun refreshPins() = ble.listPins()
    fun readSensor(pin: Int) = ble.getPin(pin)

    // ─── Sensor polling ──────────────────────────────────────
    fun pollSensors() = ble.pollSensors()
    fun pollRemote(targetId: String) = ble.pollRemote(targetId)

    // ─── Timers ──────────────────────────────────────────────
    fun setTimer(pin: Int, action: String, seconds: Int) = ble.timerSet(pin, action, seconds)
    fun setTimerPulse(pin: Int, onSec: Int, offSec: Int, repeats: Int) =
        ble.timerPulse(pin, onSec, offSec, repeats)
    fun clearTimers() = ble.timerClear()
    fun listTimers() = ble.timerList()

    // ─── Setpoints ───────────────────────────────────────────
    fun setSetpoint(sensorPin: Int, op: String, threshold: Int,
                    targetNode: String, relayPin: Int, action: Int) =
        ble.setpointSet(sensorPin, op, threshold, targetNode, relayPin, action)
    fun clearSetpoints() = ble.setpointClear()
    fun listSetpoints() = ble.setpointList()

    // ─── Auto-poll ───────────────────────────────────────────
    fun setAutoPoll(target: String, interval: Int) {
        ble.autoPollSet(target, interval)
    }
    fun stopAutoPoll() = ble.autoPollOff()

    // ─── Nodes request ─────────────────────────────────────
    fun requestNodes() = ble.send("NODES")

    fun toggleSound(enabled: Boolean) { _soundEnabled.value = enabled }

    // ─── Settings ────────────────────────────────────────────
    fun setNodeId(id: String) {
        ble.setNodeId(id)
        _config.value = _config.value.copy(nodeId = id)
    }

    fun setAesKey(key: String) {
        ble.setAesKey(key)
        _config.value = _config.value.copy(aesKey = key)
    }

    fun saveConfig() = ble.send("SAVE")
    fun requestStatus() = ble.requestStatus()

    // ─── Spreading Factor ──────────────────────────────────
    fun initiateSpreadingFactorChange(newSF: Int) {
        ble.send("SF,$newSF")
    }

    fun commitSpreadingFactorChange() {
        val state = _sfChange.value
        if (state.phase != SfChangePhase.COLLECTING) return
        ble.send("SFGO,${state.targetSF},${state.changeId}")
    }

    fun cancelSpreadingFactorChange() {
        _sfChange.value = SfChangeState()
    }

    // ─── Telegram ────────────────────────────────────────────
    fun saveTelegramConfig(cfg: TelegramConfig) {
        telegram.config = cfg
        _telegramConfig.value = cfg
    }

    fun testTelegram(onResult: (Boolean) -> Unit) {
        viewModelScope.launch {
            val ok = telegram.sendTestMessage()
            onResult(ok)
        }
    }

    // ─── Parse incoming lines from node ──────────────────────
    private fun parseLine(line: String) {
        when {
            // Incoming message: RX,<from>,<text...>,<rssi>
            // Text may contain commas (e.g. SDATA payloads), RSSI is always last field
            line.startsWith("RX,") -> {
                val parts = line.split(",")
                if (parts.size >= 3) {
                    val from = parts[1]
                    // RSSI is the last field (always negative int from firmware)
                    val lastField = parts.last()
                    val rssi = lastField.toIntOrNull()
                    val text = if (rssi != null && parts.size > 3) {
                        parts.subList(2, parts.size - 1).joinToString(",")
                    } else {
                        parts.subList(2, parts.size).joinToString(",")
                    }
                    val finalRssi = rssi ?: 0

                    // Check if this is an SDATA response from a remote POLL
                    if (text.startsWith("SDATA,")) {
                        parseSensorData(text)
                        return
                    }

                    // Position broadcast: POS,<lat>,<lon>
                    if (text.startsWith("POS,")) {
                        val posParts = text.split(",")
                        if (posParts.size >= 3) {
                            val lat = posParts[1].toDoubleOrNull()
                            val lon = posParts[2].toDoubleOrNull()
                            if (lat != null && lon != null) {
                                _positions.value = _positions.value +
                                    (from to NodePosition(from, lat, lon))
                            }
                        }
                        return  // Don't show POS as a chat message
                    }

                    _messages.value = _messages.value + ChatMessage(
                        from = from,
                        text = text,
                        rssi = finalRssi
                    )

                    // Play notification sound
                    if (_soundEnabled.value) {
                        try {
                            val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
                            RingtoneManager.getRingtone(getApplication(), uri)?.play()
                        } catch (_: Exception) { }
                    }

                    // Forward to Telegram (group broadcasts + SOS only)
                    val isSos = text.startsWith("SOS,")
                    val isGroupBroadcast = !text.startsWith("POS,")
                    if (isSos || isGroupBroadcast) {
                        viewModelScope.launch {
                            telegram.forward(from, text, finalRssi)
                        }
                    }
                }
            }

            // ─── Sensor data: SDATA,<nodeId>,<pin>:<val>,... ────
            line.startsWith("SDATA,") -> parseSensorData(line)

            // GPS position from connected node: GPS,<lat>,<lon>
            line.startsWith("GPS,") && line.split(",").size >= 3 -> {
                val parts = line.split(",")
                val lat = parts[1].toDoubleOrNull()
                val lon = parts[2].toDoubleOrNull()
                val nodeId = _config.value.nodeId
                if (lat != null && lon != null && nodeId.isNotEmpty()) {
                    _positions.value = _positions.value +
                        (nodeId to NodePosition(nodeId, lat, lon))
                }
            }

            // Command response: CMD,RSP,<pin>,<value>
            line.startsWith("CMD,RSP,") -> {
                val parts = line.split(",")
                if (parts.size >= 4) {
                    val pin = parts[2].toIntOrNull() ?: return
                    val value = parts[3].toIntOrNull() ?: return
                    updatePinState(pin, value)
                }
            }

            // Pin list: PINS,R:<pin1>,<pin2>,...|S:<pin1>,<pin2>,...
            line.startsWith("PINS,") -> {
                parsePinList(line.substring(5))
            }

            // Power mode response: OK,POWER,SOLAR or OK,POWER,NORMAL
            line == "OK,POWER,SOLAR" -> {
                val cfg = _config.value.copy(); cfg.powerMode = "SOLAR"; _config.value = cfg
            }
            line == "OK,POWER,NORMAL" -> {
                val cfg = _config.value.copy(); cfg.powerMode = "NORMAL"; _config.value = cfg
            }

            // Status response: STATUS,ID:<id>,BOARD:<name>,FREQ:<freq>,...
            line.startsWith("STATUS,") -> {
                parseStatus(line.substring(7))
            }

            // NODELIST bulk response start: NODELIST,<count>
            line.startsWith("NODELIST,") -> {
                nodeListBuffer = mutableListOf()
            }

            // NODEEND: bulk response complete — swap into state
            line == "NODEEND" -> {
                nodeListBuffer?.let { _nodes.value = it }
                nodeListBuffer = null
            }

            // Node discovered: NODE,<id>,<rssi>,<hops>[,<age>,<active>,<nextHop>]
            line.startsWith("NODE,") -> {
                val parts = line.split(",")
                if (parts.size >= 3) {
                    val id = parts[1]
                    val rssi = parts.getOrElse(2) { "0" }.toIntOrNull() ?: 0
                    val hops = parts.getOrElse(3) { "0" }.toIntOrNull() ?: 0
                    val age = parts.getOrElse(4) { "0" }.toIntOrNull() ?: 0
                    val active = parts.getOrElse(5) { "1" } == "1"
                    val nextHop = parts.getOrElse(6) { "" }
                    val node = MeshNode(id, rssi, System.currentTimeMillis(), hops, age, active, nextHop)

                    if (nodeListBuffer != null) {
                        nodeListBuffer!!.add(node)
                    } else {
                        addOrUpdateNode(id, rssi, hops)
                    }
                }
            }

            // ─── Message delivery tracking ────────────────────
            line.startsWith("SENT,") -> {
                val parts = line.split(",", limit = 3)
                if (parts.size >= 3) {
                    val mid = parts[2]
                    val msgs = _messages.value.toMutableList()
                    val idx = msgs.indexOfLast {
                        it.isOutgoing && it.deliveryStatus == DeliveryStatus.SENDING && it.messageId == null
                    }
                    if (idx >= 0) {
                        msgs[idx] = msgs[idx].copy(messageId = mid, deliveryStatus = DeliveryStatus.SENT)
                        _messages.value = msgs
                        pendingAcks[mid] = System.currentTimeMillis()
                        scheduleAckTimeout(mid)
                    }
                }
            }

            // ACK received from remote node: ACK,<from>,<mid>
            line.startsWith("ACK,") -> {
                val parts = line.split(",", limit = 3)
                if (parts.size >= 3) {
                    val mid = parts[2]
                    pendingAcks.remove(mid)
                    val msgs = _messages.value.toMutableList()
                    val idx = msgs.indexOfFirst { it.messageId == mid }
                    if (idx >= 0) {
                        msgs[idx] = msgs[idx].copy(deliveryStatus = DeliveryStatus.DELIVERED)
                        _messages.value = msgs
                    }
                }
            }

            // Backward compat: old firmware sends OK,SENT,<target> (no mid)
            line.startsWith("OK,SENT,") -> {
                val msgs = _messages.value.toMutableList()
                val idx = msgs.indexOfLast {
                    it.isOutgoing && it.deliveryStatus == DeliveryStatus.SENDING
                }
                if (idx >= 0) {
                    msgs[idx] = msgs[idx].copy(deliveryStatus = DeliveryStatus.SENT)
                    _messages.value = msgs
                }
            }

            // ─── Timer responses ────────────────────────────────
            // Timer list: TIMERS,<pin>:ON:123s,<pin>:PULSE:5/10x3,...  or  TIMERS,NONE
            line.startsWith("TIMERS") -> parseTimerList(line)

            // Timer confirmed: OK,TIMER,<pin>,ON,<seconds>  etc.
            line.startsWith("OK,TIMER,") -> {
                _statusLine.value = line
                listTimers() // refresh list
            }

            // Timer fired: TIMER,FIRED,<pin>,ON|OFF
            line.startsWith("TIMER,FIRED,") || line.startsWith("TIMER,DONE,") -> {
                _statusLine.value = line
                refreshPins()
                listTimers()
            }

            // ─── Setpoint responses ─────────────────────────────
            // SETPOINTS,<sensor>:GT:2000->0010:4:1,...  or  SETPOINTS,NONE
            line.startsWith("SETPOINTS") -> parseSetpointList(line)

            // Setpoint confirmed: OK,SETPOINT,...
            line.startsWith("OK,SETPOINT,") -> {
                _statusLine.value = line
                listSetpoints()
            }

            // Setpoint fired: SETPOINT,FIRED,<sensorPin>,<value>,<target>
            line.startsWith("SETPOINT,FIRED,") -> {
                _statusLine.value = line
            }

            // ─── Auto-poll responses ────────────────────────────
            line.startsWith("OK,AUTOPOLL,OFF") -> {
                _autoPoll.value = AutoPollConfig(enabled = false)
                _statusLine.value = "Auto-poll disabled"
            }

            line.startsWith("OK,AUTOPOLL,") -> {
                val parts = line.split(",")
                if (parts.size >= 4) {
                    val target = parts[2]
                    val interval = parts[3].toIntOrNull() ?: 300
                    _autoPoll.value = AutoPollConfig(true, target, interval)
                    _statusLine.value = "Auto-poll: $target every ${interval}s"
                }
            }

            // ─── Error responses ────────────────────────────────
            line.startsWith("ERR,") -> {
                _statusLine.value = line
            }

            // ─── ID Conflict alert ────────────────────────────
            line.startsWith("CONFLICT,") -> {
                val parts = line.split(",")
                if (parts.size >= 2) {
                    _messages.value = _messages.value + ChatMessage(
                        from = "SYSTEM",
                        text = "WARNING: ID collision! Node ${parts[1]} is used by another device.",
                        rssi = parts.getOrElse(2) { "0" }.toIntOrNull() ?: 0
                    )
                }
            }

            // ─── Spreading Factor change protocol ─────────────
            line.startsWith("CFGSTART,") -> {
                val parts = line.split(",")
                if (parts.size >= 4 && parts[1] == "SF") {
                    val newSF = parts[2].toIntOrNull() ?: return
                    val changeId = parts[3]
                    _sfChange.value = SfChangeState(
                        targetSF = newSF,
                        changeId = changeId,
                        expectedNodes = _nodes.value.map { it.id },
                        phase = SfChangePhase.COLLECTING
                    )
                }
            }

            line.startsWith("CFGACK,") -> {
                val parts = line.split(",")
                if (parts.size >= 5 && parts[1] == "SF") {
                    val changeId = parts[3]
                    val nodeId = parts[4]
                    val current = _sfChange.value
                    if (current.changeId == changeId && current.phase == SfChangePhase.COLLECTING) {
                        if (!current.ackedNodes.contains(nodeId)) {
                            _sfChange.value = current.copy(ackedNodes = current.ackedNodes + nodeId)
                        }
                    }
                }
            }

            line.startsWith("OK,SFGO,") -> {
                val newSF = line.substringAfter("OK,SFGO,").toIntOrNull()
                _sfChange.value = _sfChange.value.copy(phase = SfChangePhase.COMMITTED)
                viewModelScope.launch {
                    delay(3000)
                    _sfChange.value = _sfChange.value.copy(phase = SfChangePhase.COMPLETE)
                    if (newSF != null) {
                        _config.value = _config.value.copy(spreadingFactor = newSF)
                    }
                }
            }
        }
    }

    // ─── SDATA parser ─────────────────────────────────────────
    // Format: SDATA,<nodeId>,<pin1>:<val1>,<pin2>:<val2>,...
    private fun parseSensorData(line: String) {
        val parts = line.split(",")
        if (parts.size < 3) return
        val nodeId = parts[1]

        val history = _sensorHistory.value.toMutableMap()
        for (i in 2 until parts.size) {
            val pv = parts[i].split(":")
            if (pv.size != 2) continue
            val pin = pv[0].toIntOrNull() ?: continue
            val value = pv[1].toIntOrNull() ?: continue

            val key = "$nodeId:$pin"
            val existing = history[key]?.toMutableList() ?: mutableListOf()
            existing.add(SensorReading(value))
            if (existing.size > MAX_SENSOR_HISTORY) {
                existing.removeAt(0)
            }
            history[key] = existing

            // Also update current sensor value if it's a local pin
            val sensors = _sensors.value.toMutableList()
            val sIdx = sensors.indexOfFirst { it.pin == pin }
            if (sIdx >= 0 && nodeId == _config.value.nodeId) {
                sensors[sIdx] = sensors[sIdx].copy(value = value)
                _sensors.value = sensors
            }
        }
        _sensorHistory.value = history
    }

    // ─── Timer list parser ────────────────────────────────────
    // Format: TIMERS,<pin>:ON:123s,<pin>:PULSE:5/10x3,...  or  TIMERS,NONE
    private fun parseTimerList(line: String) {
        val parts = line.split(",")
        if (parts.size < 2 || parts[1] == "NONE") {
            _timers.value = emptyList()
            return
        }
        val list = mutableListOf<TimerInfo>()
        for (i in 1 until parts.size) {
            val fields = parts[i].split(":", limit = 3)
            if (fields.size < 2) continue
            val pin = fields[0].toIntOrNull() ?: continue
            val action = fields[1]
            val detail = fields.getOrElse(2) { "" }
            list.add(TimerInfo(pin, isPulse = action == "PULSE", action = action, detail = detail))
        }
        _timers.value = list
    }

    // ─── Setpoint list parser ─────────────────────────────────
    // Format: SETPOINTS,<sensor>:GT:2000->0010:4:1,...  or  SETPOINTS,NONE
    private fun parseSetpointList(line: String) {
        val parts = line.split(",")
        if (parts.size < 2 || parts[1] == "NONE") {
            _setpoints.value = emptyList()
            return
        }
        val list = mutableListOf<SetpointInfo>()
        for (i in 1 until parts.size) {
            // Format: <sensorPin>:<op>:<threshold>-><targetNode>:<relayPin>:<action>
            val arrowIdx = parts[i].indexOf("->")
            if (arrowIdx < 0) continue
            val left = parts[i].substring(0, arrowIdx).split(":")
            val right = parts[i].substring(arrowIdx + 2).split(":")
            if (left.size < 3 || right.size < 3) continue
            list.add(SetpointInfo(
                sensorPin = left[0].toIntOrNull() ?: continue,
                op = left[1],
                threshold = left[2].toIntOrNull() ?: continue,
                targetNode = right[0],
                relayPin = right[1].toIntOrNull() ?: continue,
                action = right[2].toIntOrNull() ?: continue
            ))
        }
        _setpoints.value = list
    }

    // ─── Parse auto-poll from STATUS ─────────────────────────
    private fun parseAutoPollFromStatus(field: String) {
        // AUTOPOLL:OFF or AUTOPOLL:0010/300s
        if (field == "OFF") {
            _autoPoll.value = AutoPollConfig(enabled = false)
        } else {
            val slash = field.indexOf('/')
            if (slash > 0) {
                val target = field.substring(0, slash)
                val interval = field.substring(slash + 1).removeSuffix("s").toIntOrNull() ?: 300
                _autoPoll.value = AutoPollConfig(true, target, interval)
            }
        }
    }

    private fun scheduleAckTimeout(mid: String) {
        viewModelScope.launch {
            delay(3000)
            if (pendingAcks.containsKey(mid)) {
                pendingAcks.remove(mid)
                val msgs = _messages.value.toMutableList()
                val idx = msgs.indexOfFirst { it.messageId == mid }
                if (idx >= 0 && msgs[idx].deliveryStatus == DeliveryStatus.SENT) {
                    msgs[idx] = msgs[idx].copy(deliveryStatus = DeliveryStatus.FAILED)
                    _messages.value = msgs
                }
            }
        }
    }

    private fun updatePinState(pin: Int, value: Int) {
        val relays = _relays.value.toMutableList()
        val idx = relays.indexOfFirst { it.pin == pin }
        if (idx >= 0) {
            relays[idx] = relays[idx].copy(state = value > 0)
            _relays.value = relays
        } else {
            val sensors = _sensors.value.toMutableList()
            val sIdx = sensors.indexOfFirst { it.pin == pin }
            if (sIdx >= 0) {
                sensors[sIdx] = sensors[sIdx].copy(value = value)
                _sensors.value = sensors
            }
        }
    }

    private fun parsePinList(data: String) {
        val parts = data.split("|")
        for (part in parts) {
            if (part.startsWith("R:")) {
                val pins = part.substring(2).split(",").mapNotNull { it.trim().toIntOrNull() }
                _relays.value = pins.map { RelayPin(it) }
            } else if (part.startsWith("S:")) {
                val pins = part.substring(2).split(",").mapNotNull { it.trim().toIntOrNull() }
                _sensors.value = pins.map { SensorPin(it) }
            }
        }
    }

    private fun parseStatus(data: String) {
        val fields = data.split(",")
        val cfg = _config.value.copy()
        for (field in fields) {
            val kv = field.split(":", limit = 2)
            if (kv.size != 2) continue
            when (kv[0]) {
                "ID" -> cfg.nodeId = kv[1]
                "BOARD" -> cfg.boardName = kv[1]
                "FREQ" -> cfg.frequency = kv[1].toFloatOrNull() ?: 868.0f
                "SF" -> cfg.spreadingFactor = kv[1].toIntOrNull() ?: 9
                "POWER" -> cfg.powerMode = kv[1]
                "AUTOPOLL" -> parseAutoPollFromStatus(kv[1])
            }
        }
        _config.value = cfg
    }

    fun setPowerMode(solar: Boolean) {
        ble.send(if (solar) "POWER,SOLAR" else "POWER,NORMAL")
    }

    private fun addOrUpdateNode(id: String, rssi: Int, hops: Int) {
        val current = _nodes.value.toMutableList()
        val idx = current.indexOfFirst { it.id == id }
        val node = MeshNode(id, rssi, System.currentTimeMillis(), hops)
        if (idx >= 0) current[idx] = node else current.add(node)
        _nodes.value = current
    }

    // ─── Position Sharing ─────────────────────────────────

    fun setSharePosition(enabled: Boolean) {
        _sharePosition.value = enabled
        if (enabled) {
            positionSharingJob = viewModelScope.launch {
                while (_sharePosition.value) {
                    broadcastPhonePosition()
                    delay(60000)  // Every 60 seconds
                }
            }
        } else {
            positionSharingJob?.cancel()
            positionSharingJob = null
        }
    }

    private fun broadcastPhonePosition() {
        val ctx = getApplication<Application>()
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED) return
        try {
            val lm = ctx.getSystemService(android.content.Context.LOCATION_SERVICE) as LocationManager
            val loc = lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?: lm.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
            if (loc != null) {
                val pos = "POS,%.6f,%.6f".format(loc.latitude, loc.longitude)
                ble.sendBroadcast(pos)
            }
        } catch (_: Exception) { }
    }

    // ─── Tracking ────────────────────────────────────────

    fun setTrackingTarget(nodeId: String?) {
        _trackingTarget.value = nodeId
    }

    fun getDistanceTo(nodeId: String): Float? {
        val ctx = getApplication<Application>()
        val target = _positions.value[nodeId] ?: return null
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED) return null
        try {
            val lm = ctx.getSystemService(android.content.Context.LOCATION_SERVICE) as LocationManager
            val loc = lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?: lm.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
                ?: return null
            val results = FloatArray(1)
            android.location.Location.distanceBetween(
                loc.latitude, loc.longitude, target.lat, target.lon, results)
            return results[0]  // metres
        } catch (_: Exception) { return null }
    }

    fun getBearingTo(nodeId: String): Float? {
        val ctx = getApplication<Application>()
        val target = _positions.value[nodeId] ?: return null
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED) return null
        try {
            val lm = ctx.getSystemService(android.content.Context.LOCATION_SERVICE) as LocationManager
            val loc = lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?: lm.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
                ?: return null
            val myLoc = android.location.Location("").apply {
                latitude = loc.latitude; longitude = loc.longitude
            }
            val targetLoc = android.location.Location("").apply {
                latitude = target.lat; longitude = target.lon
            }
            return myLoc.bearingTo(targetLoc)
        } catch (_: Exception) { return null }
    }

    override fun onCleared() {
        positionSharingJob?.cancel()
        ble.disconnect()
        super.onCleared()
    }
}
