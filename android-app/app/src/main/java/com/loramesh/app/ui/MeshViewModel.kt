package com.loramesh.app.ui

import android.Manifest
import android.app.Application
import android.content.Intent
import android.content.pm.PackageManager
import android.location.LocationManager
import android.media.RingtoneManager
import android.net.Uri
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.loramesh.app.LoRaMeshApp
import com.loramesh.app.ble.BleConnectionService
import com.loramesh.app.ble.BleManager
import com.loramesh.app.ble.ConnectionState
import com.loramesh.app.data.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.security.MessageDigest

// ─── OTA upload state (Phase 3) ──────────────────────────────────
// Top-level enum + data class so they're reachable from
// OtaUploadScreen without importing ViewModel internals.
enum class OtaState { IDLE, HASHING, UPLOADING, VERIFYING, REBOOTING, DONE, ERROR }
data class OtaProgress(
    val currentChunk: Int,
    val totalChunks: Int,
    val bytesSent: Long,
    val totalBytes: Long,
)

// ═══════════════════════════════════════════════════════════════
// ViewModel - bridges BLE manager with UI state
// ═══════════════════════════════════════════════════════════════

private const val MAX_SENSOR_HISTORY = 60 // readings per sensor key

class MeshViewModel(app: Application) : AndroidViewModel(app) {

    // BleManager lives in Application — survives Activity recreation + backgrounding
    val ble: BleManager = (app as LoRaMeshApp).bleManager
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

    // ─── Remote control target ────────────────────────────
    // Empty string = local (BLE-connected) node
    private val _controlTarget = MutableStateFlow("")
    val controlTarget: StateFlow<String> = _controlTarget

    // Pin configs for remote nodes: nodeId → (relayPins, sensorPins)
    private val _remotePinConfigs = MutableStateFlow<Map<String, Pair<List<Int>, List<Int>>>>(emptyMap())
    val remotePinConfigs: StateFlow<Map<String, Pair<List<Int>, List<Int>>>> = _remotePinConfigs

    // ─── OTA firmware upload (Phase 3) ───────────────────────
    private val _otaState = MutableStateFlow(OtaState.IDLE)
    val otaState: StateFlow<OtaState> = _otaState

    private val _otaProgress = MutableStateFlow<OtaProgress?>(null)
    val otaProgress: StateFlow<OtaProgress?> = _otaProgress

    private val _otaLastError = MutableStateFlow<String?>(null)
    val otaLastError: StateFlow<String?> = _otaLastError

    // OTA-side metadata: filename + computed SHA256 prefix for display
    private val _otaSelectedFileName = MutableStateFlow<String?>(null)
    val otaSelectedFileName: StateFlow<String?> = _otaSelectedFileName

    private val _otaSelectedFileSha = MutableStateFlow<String?>(null)
    val otaSelectedFileSha: StateFlow<String?> = _otaSelectedFileSha

    private val _otaSelectedFileUri = MutableStateFlow<Uri?>(null)
    val otaSelectedFileUri: StateFlow<Uri?> = _otaSelectedFileUri

    private var otaJob: Job? = null

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

        // Start/stop foreground service based on BLE connection state
        viewModelScope.launch {
            ble.connectionState.collect { state ->
                val ctx = getApplication<Application>()
                when (state) {
                    ConnectionState.CONNECTED -> {
                        val intent = Intent(ctx, BleConnectionService::class.java).apply {
                            putExtra(BleConnectionService.EXTRA_DEVICE_NAME,
                                ble.connectedDeviceName.value)
                        }
                        ctx.startForegroundService(intent)
                    }
                    ConnectionState.DISCONNECTED -> {
                        ctx.stopService(Intent(ctx, BleConnectionService::class.java))
                        clearSessionState()
                    }
                    else -> { /* scanning/connecting — no action */ }
                }
            }
        }
    }

    // ─── Session state — cleared on disconnect so stale data from
    //     a previous node doesn't persist when connecting to a different one
    private fun clearSessionState() {
        _messages.value = emptyList()
        _nodes.value = emptyList()
        _relays.value = emptyList()
        _sensors.value = emptyList()
        _config.value = NodeConfig()
        _positions.value = emptyMap()
        _sensorHistory.value = emptyMap()
        _timers.value = emptyList()
        _setpoints.value = emptyList()
        _autoPoll.value = AutoPollConfig()
        _sfChange.value = SfChangeState()
        _statusLine.value = ""
        pendingAcks.clear()
        // Reset OTA state too so a half-finished upload doesn't show
        // stale progress after a reconnect to a different node.
        otaJob?.cancel()
        otaJob = null
        _otaState.value = OtaState.IDLE
        _otaProgress.value = null
        _otaLastError.value = null
        _otaSelectedFileName.value = null
        _otaSelectedFileSha.value = null
        _otaSelectedFileUri.value = null
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

    // ─── Relay Control (local or remote) ─────────────────────
    private fun isRemote(): Boolean {
        val target = _controlTarget.value
        return target.isNotEmpty() && target != _config.value.nodeId
    }

    fun setControlTarget(nodeId: String) {
        _controlTarget.value = nodeId
        if (isRemote()) {
            // Query remote node's pin config
            ble.listRemotePins(nodeId)
        } else {
            ble.listPins()
        }
    }

    fun toggleRelay(pin: Int) {
        val current = _relays.value.find { it.pin == pin }
        val newState = !(current?.state ?: false)
        if (isRemote()) {
            ble.setRemoteRelay(_controlTarget.value, pin, newState)
        } else {
            ble.setRelay(pin, newState)
        }
    }

    fun pulseRelay(pin: Int, ms: Int = 1000) {
        if (isRemote()) ble.pulseRemoteRelay(_controlTarget.value, pin, ms)
        else ble.pulseRelay(pin, ms)
    }

    fun refreshPins() {
        if (isRemote()) ble.listRemotePins(_controlTarget.value)
        else ble.listPins()
    }

    fun readSensor(pin: Int) {
        if (isRemote()) ble.getRemotePin(_controlTarget.value, pin)
        else ble.getPin(pin)
    }

    // ─── Sensor polling ──────────────────────────────────────
    fun pollSensors() {
        if (isRemote()) ble.pollRemote(_controlTarget.value)
        else ble.pollSensors()
    }
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

    // ─── OTA firmware upload ─────────────────────────────────
    // The user picks a .bin file via the OtaUploadScreen's
    // ActivityResultContracts.OpenDocument launcher; the screen calls
    // selectOtaFile(uri) to register it + compute SHA256 for display.
    // Then the Upload button triggers startOtaUpload() which streams
    // the file in OTA_CHUNK_SIZE chunks to the firmware via the BLE
    // NUS pipe. Progress flows through otaProgress; final state
    // (DONE / ERROR) lives in otaState.
    //
    // Coroutine runs on viewModelScope (survives config changes) +
    // Dispatchers.IO (file I/O off the main thread). The existing
    // BleConnectionService foreground service keeps BLE alive if the
    // user briefly backgrounds the app. Cancellation via cancelOta()
    // sends FW,ABORT to the firmware so the partition is released.
    private val OTA_CHUNK_SIZE = 256

    /** User picked a file. Open it, compute SHA256, store for display. */
    fun selectOtaFile(uri: Uri) {
        viewModelScope.launch(Dispatchers.IO) {
            val ctx = getApplication<Application>()
            try {
                _otaState.value = OtaState.HASHING
                _otaLastError.value = null
                val md = MessageDigest.getInstance("SHA-256")
                var bytes = 0L
                ctx.contentResolver.openInputStream(uri)?.use { input ->
                    val buf = ByteArray(8192)
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        md.update(buf, 0, n)
                        bytes += n
                    }
                } ?: throw java.io.IOException("Cannot open selected file")
                val sha = md.digest().joinToString("") { "%02x".format(it) }

                // Try to extract filename from the URI; fall back to "firmware.bin"
                val name = ctx.contentResolver.query(uri, null, null, null, null)?.use { c ->
                    val idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0 && c.moveToFirst()) c.getString(idx) else null
                } ?: "firmware.bin"

                _otaSelectedFileUri.value = uri
                _otaSelectedFileName.value = "$name (${bytes / 1024} KB)"
                _otaSelectedFileSha.value = sha
                _otaState.value = OtaState.IDLE   // ready to upload
            } catch (e: Exception) {
                _otaLastError.value = e.message ?: "Failed to read file"
                _otaState.value = OtaState.ERROR
            }
        }
    }

    /** Upload the previously-selected file to the connected node. */
    fun startOtaUpload() {
        val uri = _otaSelectedFileUri.value ?: return
        val sha = _otaSelectedFileSha.value ?: return
        if (_otaState.value !in setOf(OtaState.IDLE, OtaState.DONE, OtaState.ERROR)) return

        otaJob = viewModelScope.launch(Dispatchers.IO) {
            val ctx = getApplication<Application>()
            try {
                _otaLastError.value = null
                _otaProgress.value = null

                // Determine total bytes (we hashed already; re-open and stream)
                val totalBytes = ctx.contentResolver.openInputStream(uri)?.use { it.available().toLong() } ?: 0L
                // openInputStream(uri).available() isn't always accurate on Android;
                // fall back to a counted read if zero.
                val byteCount = if (totalBytes > 0) totalBytes else countBytes(ctx, uri)
                if (byteCount <= 0) throw java.io.IOException("File is empty or unreadable")

                _otaState.value = OtaState.UPLOADING

                // FW,BEGIN — firmware erases the next slot here, takes a moment
                val beginReply = ble.otaBegin(byteCount.toInt(), sha, timeoutMs = 10000L)
                if (!beginReply.startsWith("FW,BEGIN,OK,")) {
                    fail("BEGIN rejected: $beginReply")
                    return@launch
                }
                val chunkSize = beginReply.removePrefix("FW,BEGIN,OK,").toIntOrNull() ?: OTA_CHUNK_SIZE

                val totalChunks = ((byteCount + chunkSize - 1) / chunkSize).toInt()

                // FW,CHUNK loop — strict in-order, one ACK per chunk
                ctx.contentResolver.openInputStream(uri)?.use { input ->
                    val buf = ByteArray(chunkSize)
                    var seq = 0
                    var bytesSent = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        val hex = StringBuilder(n * 2)
                        for (i in 0 until n) hex.append("%02x".format(buf[i]))
                        val ack = ble.otaChunk(seq, hex.toString(), timeoutMs = 8000L)
                        if (!ack.startsWith("FW,ACK,$seq")) {
                            fail("Chunk $seq rejected: $ack")
                            return@launch
                        }
                        bytesSent += n
                        _otaProgress.value = OtaProgress(
                            currentChunk = seq + 1,
                            totalChunks = totalChunks,
                            bytesSent = bytesSent,
                            totalBytes = byteCount,
                        )
                        seq++
                    }
                } ?: run { fail("Cannot reopen file for upload"); return@launch }

                // FW,END — firmware verifies SHA, commits partition, schedules reboot.
                // 30s timeout because firmware spends ~500ms+ on the partition
                // bookkeeping then disconnects BLE before we get the reply.
                _otaState.value = OtaState.VERIFYING
                val endReply = ble.otaEnd(sha, timeoutMs = 30000L)
                if (!endReply.startsWith("FW,END,OK,")) {
                    fail("END rejected: $endReply")
                    return@launch
                }

                _otaState.value = OtaState.REBOOTING
                // Node reboots ~500ms after sending the OK. BLE will drop, then
                // the foreground service's auto-reconnect kicks in. We don't
                // actively reconnect — leave that to the existing flow. Just
                // wait long enough that "DONE" reflects when the user could
                // expect to see the node come back.
                delay(20_000)   // 20s — boot + BLE rediscovery typically ~10-15s

                _otaState.value = OtaState.DONE
            } catch (e: TimeoutCancellationException) {
                fail("Timeout waiting for reply from node")
            } catch (e: kotlinx.coroutines.CancellationException) {
                // Cancelled (user pressed Cancel). Don't overwrite the state — cancelOta() handled it.
                throw e
            } catch (e: Exception) {
                fail(e.message ?: "Unknown error")
            }
        }
    }

    /** Cancel an in-flight upload — fires FW,ABORT to release the partition. */
    fun cancelOta() {
        otaJob?.cancel()
        otaJob = null
        // Best-effort ABORT — if BLE is dead this will throw, which we swallow
        viewModelScope.launch {
            try { ble.otaAbort() } catch (_: Exception) { /* ignore */ }
        }
        _otaState.value = OtaState.IDLE
        _otaProgress.value = null
    }

    /** Clear an error so the user can retry. */
    fun clearOtaError() {
        if (_otaState.value == OtaState.ERROR) {
            _otaState.value = OtaState.IDLE
            _otaLastError.value = null
        }
    }

    private fun fail(msg: String) {
        _otaLastError.value = msg
        _otaState.value = OtaState.ERROR
    }

    private suspend fun countBytes(ctx: android.content.Context, uri: Uri): Long =
        withContext(Dispatchers.IO) {
            var n = 0L
            ctx.contentResolver.openInputStream(uri)?.use { input ->
                val buf = ByteArray(8192)
                while (true) {
                    val r = input.read(buf)
                    if (r <= 0) break
                    n += r
                }
            }
            n
        }

    // ─── Nodes request ─────────────────────────────────────
    fun requestNodes() = ble.send("NODES")

    fun toggleSound(enabled: Boolean) { _soundEnabled.value = enabled }

    // ─── Settings ────────────────────────────────────────────
    fun setNodeId(id: String) {
        ble.setNodeId(id)
        ble.send("SAVE")  // Persist to EEPROM immediately
        _config.value = _config.value.copy(nodeId = id)
    }

    fun setAesKey(key: String) {
        ble.setAesKey(key)
        ble.send("SAVE")  // Persist to EEPROM immediately
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

                    // Remote PINS response: PINS,R:2,3,4|S:33,34
                    if (text.startsWith("PINS,")) {
                        parseRemotePins(from, text)
                        return
                    }

                    // Remote CMD response: CMD,RSP,<pin>,<value>
                    if (text.startsWith("CMD,RSP,")) {
                        val rspParts = text.split(",")
                        if (rspParts.size >= 4) {
                            val pin = rspParts[2].toIntOrNull() ?: return
                            val value = rspParts[3].toIntOrNull() ?: return
                            updatePinState(pin, value)
                        }
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

    private fun parseRemotePins(nodeId: String, text: String) {
        val data = text.substring(5) // strip "PINS,"
        val relays = mutableListOf<Int>()
        val sensors = mutableListOf<Int>()
        for (part in data.split("|")) {
            if (part.startsWith("R:")) {
                relays.addAll(part.substring(2).split(",").mapNotNull { it.trim().toIntOrNull() })
            } else if (part.startsWith("S:")) {
                sensors.addAll(part.substring(2).split(",").mapNotNull { it.trim().toIntOrNull() })
            }
        }
        _remotePinConfigs.value = _remotePinConfigs.value + (nodeId to Pair(relays, sensors))
        // If this is the currently selected control target, update the relay/sensor lists
        if (_controlTarget.value == nodeId) {
            _relays.value = relays.map { RelayPin(it) }
            _sensors.value = sensors.map { SensorPin(it) }
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
                "GPS" -> cfg.gpsStatus = kv[1]
                "AUTOPOLL" -> parseAutoPollFromStatus(kv[1])
                "SETUP" -> cfg.setupMode = (kv[1] == "1")
            }
        }
        _config.value = cfg
    }

    fun setGpsEnabled(enabled: Boolean) {
        val cfg = _config.value.copy()
        cfg.gpsStatus = if (enabled) "NOFIX" else "OFF"
        _config.value = cfg
        ble.send(if (enabled) "GPS,ON" else "GPS,OFF")
    }

    fun setPowerMode(solar: Boolean) {
        // Optimistically update state so switch doesn't snap back while waiting for BLE response
        val cfg = _config.value.copy()
        cfg.powerMode = if (solar) "SOLAR" else "NORMAL"
        _config.value = cfg
        ble.send(if (solar) "POWER,SOLAR" else "POWER,NORMAL")
    }

    fun sendCommand(cmd: String) {
        ble.send(cmd)
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
        // Do NOT disconnect BLE here — the foreground service keeps it alive
        // User explicitly disconnects via UI or notification action
        super.onCleared()
    }
}
