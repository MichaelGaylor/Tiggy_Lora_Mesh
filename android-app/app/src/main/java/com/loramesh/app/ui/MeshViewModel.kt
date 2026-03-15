package com.loramesh.app.ui

import android.app.Application
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

    val connectionState = ble.connectionState
    val connectedDeviceName = ble.connectedDeviceName
    val discoveredDevices = ble.discoveredDevices

    init {
        // Parse incoming data lines from BLE
        viewModelScope.launch {
            ble.incomingData.collect { line ->
                parseLine(line)
            }
        }
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

    // ─── Relay Control ───────────────────────────────────────
    fun toggleRelay(pin: Int) {
        val current = _relays.value.find { it.pin == pin }
        val newState = !(current?.state ?: false)
        ble.setRelay(pin, newState)
    }

    fun pulseRelay(pin: Int, ms: Int = 1000) = ble.pulseRelay(pin, ms)
    fun refreshPins() = ble.listPins()
    fun readSensor(pin: Int) = ble.getPin(pin)

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
            // Incoming message: RX,<from>,<text>,<rssi>
            line.startsWith("RX,") -> {
                val parts = line.split(",", limit = 4)
                if (parts.size >= 3) {
                    val from = parts[1]
                    val text = parts.getOrElse(2) { "" }
                    val rssi = parts.getOrElse(3) { "0" }.toIntOrNull() ?: 0

                    _messages.value = _messages.value + ChatMessage(
                        from = from,
                        text = text,
                        rssi = rssi
                    )

                    // Forward to Telegram (group broadcasts + SOS only)
                    // Direct messages stay private
                    val isSos = text.startsWith("SOS,")
                    val isGroupBroadcast = !text.startsWith("POS,") // exclude position-only pings
                    if (isSos || isGroupBroadcast) {
                        viewModelScope.launch {
                            telegram.forward(from, text, rssi)
                        }
                    }
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

            // Status response: STATUS,ID:<id>,BOARD:<name>,FREQ:<freq>,...
            line.startsWith("STATUS,") -> {
                parseStatus(line.substring(7))
            }

            // Node discovered: NODE,<id>,<rssi>,<hops>
            line.startsWith("NODE,") -> {
                val parts = line.split(",")
                if (parts.size >= 3) {
                    val id = parts[1]
                    val rssi = parts.getOrElse(2) { "0" }.toIntOrNull() ?: 0
                    val hops = parts.getOrElse(3) { "0" }.toIntOrNull() ?: 0
                    addOrUpdateNode(id, rssi, hops)
                }
            }

            // ─── Message delivery tracking ────────────────────
            // Firmware confirmed LoRa transmit: SENT,<target>,<mid>
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

            // ─── ID Conflict alert ────────────────────────────
            // Firmware detected another node with same ID: CONFLICT,<id>,<rssi>
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
            // SF change initiated: CFGSTART,SF,<value>,<changeId>
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

            // Node ACKed the SF change: CFGACK,SF,<value>,<changeId>,<nodeId>
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

            // CFGGO acknowledged by firmware: OK,SFGO,<value>
            line.startsWith("OK,SFGO,") -> {
                val newSF = line.substringAfter("OK,SFGO,").toIntOrNull()
                _sfChange.value = _sfChange.value.copy(phase = SfChangePhase.COMMITTED)
                viewModelScope.launch {
                    delay(3000) // wait for all nodes to switch
                    _sfChange.value = _sfChange.value.copy(phase = SfChangePhase.COMPLETE)
                    if (newSF != null) {
                        _config.value = _config.value.copy(spreadingFactor = newSF)
                    }
                }
            }
        }
    }

    private fun scheduleAckTimeout(mid: String) {
        viewModelScope.launch {
            delay(3000) // ACK_TIMEOUT = 3s, matches firmware
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
        // Format: R:2,4,12,15|S:34,36,39
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

    override fun onCleared() {
        ble.disconnect()
        super.onCleared()
    }
}
