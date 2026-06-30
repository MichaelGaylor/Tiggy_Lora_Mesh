package com.loramesh.app.ble

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.onSubscription
import kotlinx.coroutines.withTimeout
import java.util.UUID

private const val PREFS_NAME = "mesh_ble_prefs"
private const val KEY_LAST_MAC = "last_device_mac"
private const val KEY_LAST_NAME = "last_device_name"

// ═══════════════════════════════════════════════════════════════
// BLE Manager - handles scanning, connecting, and data exchange
// with LoRa mesh nodes running BLE serial service
// ═══════════════════════════════════════════════════════════════

// Nordic UART Service UUIDs (standard BLE serial)
val BLE_SERVICE_UUID: UUID     = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
val BLE_TX_CHAR_UUID: UUID     = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E") // Write to node
val BLE_RX_CHAR_UUID: UUID     = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E") // Read from node
val CLIENT_CONFIG_UUID: UUID   = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

enum class ConnectionState { DISCONNECTED, SCANNING, CONNECTING, CONNECTED }

@SuppressLint("MissingPermission")
class BleManager(private val context: Context) {

    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var txCharacteristic: BluetoothGattCharacteristic? = null
    private val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    // ─── Observable state ────────────────────────────────────
    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState

    private val _discoveredDevices = MutableStateFlow<List<ScannedDevice>>(emptyList())
    val discoveredDevices: StateFlow<List<ScannedDevice>> = _discoveredDevices

    // SharedFlow (not StateFlow) so rapid back-to-back lines don't get
    // conflated. With StateFlow, _incomingData.value = X immediately followed
    // by .value = Y meant the collector only ever saw Y — burst notifications
    // (NODE bursts during mesh discovery, STATUS+PINS replies, etc.) were
    // silently dropped. SharedFlow with a generous buffer delivers every line.
    private val _incomingData = MutableSharedFlow<String>(
        extraBufferCapacity = 64,
        onBufferOverflow = BufferOverflow.DROP_OLDEST
    )
    val incomingData: SharedFlow<String> = _incomingData

    private val _connectedDeviceName = MutableStateFlow("")
    val connectedDeviceName: StateFlow<String> = _connectedDeviceName

    // Auto-reconnect: track whether we're doing an auto-connect attempt
    private var isAutoConnecting = false
    private val _autoConnectFailed = MutableStateFlow(false)
    val autoConnectFailed: StateFlow<Boolean> = _autoConnectFailed

    data class ScannedDevice(val name: String, val address: String, val rssi: Int)

    // Accumulates partial lines from BLE chunks
    private val rxBuffer = StringBuilder()

    // ─── Scan ────────────────────────────────────────────────
    fun startScan() {
        if (bluetoothAdapter?.isEnabled != true) return
        scanner = bluetoothAdapter.bluetoothLeScanner ?: return
        _connectionState.value = ConnectionState.SCANNING
        _discoveredDevices.value = emptyList()

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(BLE_SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner?.startScan(listOf(filter), settings, scanCallback)
    }

    fun stopScan() {
        scanner?.stopScan(scanCallback)
        if (_connectionState.value == ConnectionState.SCANNING) {
            _connectionState.value = ConnectionState.DISCONNECTED
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            val name = result.scanRecord?.deviceName ?: device.name ?: "Unknown"
            val entry = ScannedDevice(name, device.address, result.rssi)

            val current = _discoveredDevices.value.toMutableList()
            val idx = current.indexOfFirst { it.address == entry.address }
            if (idx >= 0) current[idx] = entry else current.add(entry)
            _discoveredDevices.value = current
        }
    }

    // ─── Saved device (auto-reconnect) ──────────────────────
    fun getSavedDeviceMac(): String? = prefs.getString(KEY_LAST_MAC, null)
    fun getSavedDeviceName(): String = prefs.getString(KEY_LAST_NAME, null) ?: ""

    fun saveDevice(address: String, name: String) {
        prefs.edit().putString(KEY_LAST_MAC, address).putString(KEY_LAST_NAME, name).apply()
    }

    fun forgetDevice() {
        prefs.edit().remove(KEY_LAST_MAC).remove(KEY_LAST_NAME).apply()
    }

    // ─── Connect ─────────────────────────────────────────────
    fun connect(address: String) {
        stopScan()
        isAutoConnecting = false
        _autoConnectFailed.value = false
        _connectionState.value = ConnectionState.CONNECTING
        val device = bluetoothAdapter?.getRemoteDevice(address) ?: return
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun autoConnect(): Boolean {
        val mac = getSavedDeviceMac() ?: return false
        if (bluetoothAdapter?.isEnabled != true) return false
        isAutoConnecting = true
        _autoConnectFailed.value = false
        _connectionState.value = ConnectionState.CONNECTING
        _connectedDeviceName.value = getSavedDeviceName()
        val device = bluetoothAdapter.getRemoteDevice(mac) ?: return false
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        return true
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        txCharacteristic = null
        isAutoConnecting = false
        _connectionState.value = ConnectionState.DISCONNECTED
        _connectedDeviceName.value = ""
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    val name = g.device.name ?: g.device.address
                    _connectedDeviceName.value = name
                    saveDevice(g.device.address, name)
                    g.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    if (isAutoConnecting) {
                        _autoConnectFailed.value = true
                        isAutoConnecting = false
                    }
                    _connectionState.value = ConnectionState.DISCONNECTED
                    _connectedDeviceName.value = ""
                    txCharacteristic = null
                    gatt?.close()
                    gatt = null
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                disconnect()
                return
            }
            val service = g.getService(BLE_SERVICE_UUID)
            if (service == null) {
                disconnect()
                return
            }

            // TX characteristic (phone → node)
            txCharacteristic = service.getCharacteristic(BLE_TX_CHAR_UUID)

            // RX characteristic (node → phone) - enable notifications
            val rxChar = service.getCharacteristic(BLE_RX_CHAR_UUID)
            if (rxChar != null) {
                g.setCharacteristicNotification(rxChar, true)
                val descriptor = rxChar.getDescriptor(CLIENT_CONFIG_UUID)
                if (descriptor != null) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        g.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    } else {
                        @Suppress("DEPRECATION")
                        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        @Suppress("DEPRECATION")
                        g.writeDescriptor(descriptor)
                    }
                }
            }

            _connectionState.value = ConnectionState.CONNECTED

            // Auto-request STATUS so the app knows the board type, power mode, etc.
            android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                send("STATUS")
            }, 500)
        }

        // Write completion — releases the writeInProgress flag so the next
        // chunk / next send() can issue its writeCharacteristic. On Android
        // 13+ the new writeCharacteristic API rejects overlapping writes
        // with ERROR_GATT_WRITE_REQUEST_BUSY (silent — int return value);
        // bluedroid firmware acked fast enough to dodge this, but NimBLE's
        // slower ack exposed it. We now block each chunk on this callback.
        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (characteristic.uuid == BLE_TX_CHAR_UUID) {
                synchronized(writeLock) {
                    writeInProgress = false
                    writeLock.notifyAll()
                }
            }
        }

        // Android 13+ uses new signature with value parameter
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == BLE_RX_CHAR_UUID) handleRxData(value)
        }

        // Android 12 and below uses old signature
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == BLE_RX_CHAR_UUID) {
                handleRxData(characteristic.value ?: return)
            }
        }
    }

    private fun handleRxData(data: ByteArray) {
        rxBuffer.append(data.toString(Charsets.UTF_8))
        while (rxBuffer.contains("\n")) {
            val nlIdx = rxBuffer.indexOf("\n")
            val line = rxBuffer.substring(0, nlIdx).trim()
            rxBuffer.delete(0, nlIdx + 1)
            if (line.isNotEmpty()) _incomingData.tryEmit(line)
        }
    }

    // Write serialization — Android 13+'s new writeCharacteristic API rejects
    // overlapping writes with ERROR_GATT_WRITE_REQUEST_BUSY (silently — the
    // return code is an int the old code threw away). The MainActivity
    // LaunchedEffect fires STATUS / CMD,LIST / NODES back-to-back on connect;
    // only STATUS won, the other two were silently rejected, so the Control
    // screen had no pins and the Nodes tab stayed empty. Worked on bluedroid
    // firmware only because writes completed fast enough to dodge the BUSY
    // window — NimBLE's slower write ack exposed the latent race.
    //
    // We now block each chunk until onCharacteristicWrite fires (real
    // completion, not an arbitrary delay), with a 500ms safety timeout
    // in case a write goes missing. @Synchronized serializes concurrent
    // send() calls — the second send() blocks on the intrinsic lock until
    // the first finishes its chunks.
    private val writeLock = Object()
    private var writeInProgress = false

    // ─── Send data to node ───────────────────────────────────
    @Synchronized
    fun send(data: String) {
        val char = txCharacteristic ?: return
        val g = gatt ?: return
        val bytes = (data + "\n").toByteArray(Charsets.UTF_8)
        val chunkSize = 20
        for (i in bytes.indices step chunkSize) {
            // Wait for previous write's completion callback before issuing the next
            synchronized(writeLock) {
                val deadline = System.currentTimeMillis() + 500
                while (writeInProgress) {
                    val remaining = deadline - System.currentTimeMillis()
                    if (remaining <= 0) break  // safety timeout — proceed
                    try { writeLock.wait(remaining) } catch (_: InterruptedException) { break }
                }
                writeInProgress = true
            }
            val chunk = bytes.sliceArray(i until minOf(i + chunkSize, bytes.size))
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(char, chunk, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
            } else {
                @Suppress("DEPRECATION")
                char.value = chunk
                @Suppress("DEPRECATION")
                g.writeCharacteristic(char)
            }
            // No Thread.sleep here — we now wait for the actual completion
            // callback at the top of the next iteration. Real pacing, not
            // arbitrary fixed delay.
        }
    }

    // ─── Convenience commands ────────────────────────────────
    fun sendMessage(targetId: String, message: String) = send("MSG,$targetId,$message")
    fun sendBroadcast(message: String) = send("MSG,FFFF,$message")
    fun setRelay(pin: Int, state: Boolean) = send("CMD,SET,$pin,${if (state) 1 else 0}")
    fun getPin(pin: Int) = send("CMD,GET,$pin")
    fun pulseRelay(pin: Int, ms: Int) = send("CMD,PULSE,$pin,$ms")
    fun listPins() = send("CMD,LIST")
    fun requestStatus() = send("STATUS")
    fun setNodeId(id: String) = send("ID $id")
    fun setAesKey(key: String) = send("KEY $key")

    // ─── Sensor polling ───────────────────────────────────────
    fun pollSensors() = send("POLL")
    fun pollRemote(targetId: String) = send("POLL,$targetId")

    // ─── Remote node control (via mesh) ─────────────────────
    fun setRemoteRelay(nodeId: String, pin: Int, state: Boolean) =
        send("MSG,$nodeId,CMD,SET,$pin,${if (state) 1 else 0}")
    fun pulseRemoteRelay(nodeId: String, pin: Int, ms: Int) =
        send("MSG,$nodeId,CMD,PULSE,$pin,$ms")
    fun getRemotePin(nodeId: String, pin: Int) =
        send("MSG,$nodeId,CMD,GET,$pin")
    fun listRemotePins(nodeId: String) =
        send("MSG,$nodeId,CMD,LIST")

    // ─── Timers ───────────────────────────────────────────────
    fun timerSet(pin: Int, action: String, seconds: Int) = send("TIMER,$pin,$action,$seconds")
    fun timerPulse(pin: Int, onSec: Int, offSec: Int, repeats: Int) =
        send("TIMER,$pin,PULSE,$onSec,$offSec,$repeats")
    fun timerClear() = send("TIMER,CLEAR")
    fun timerList() = send("TIMER,LIST")

    // ─── Setpoints ────────────────────────────────────────────
    fun setpointSet(sensorPin: Int, op: String, threshold: Int,
                    targetNode: String, relayPin: Int, action: Int) =
        send("SETPOINT,$sensorPin,$op,$threshold,$targetNode,$relayPin,$action")
    fun setpointClear() = send("SETPOINT,CLEAR")
    fun setpointList() = send("SETPOINT,LIST")

    // ─── Auto-poll ────────────────────────────────────────────
    fun autoPollSet(target: String, interval: Int) = send("AUTOPOLL,$target,$interval")
    fun autoPollOff() = send("AUTOPOLL,OFF")

    // ─── OTA firmware upload (Phase 3) ───────────────────────
    // Send a FW,* command and suspend until the matching reply lands
    // on the incomingData SharedFlow. We use onSubscription { send(...) }
    // so the subscriber is registered BEFORE the firmware can reply —
    // without that, a fast-replying firmware could emit the reply
    // before .first { ... } starts collecting, and we'd hang on the
    // timeout. onSubscription guarantees subscribe → send → collect
    // ordering on a single coroutine.
    //
    // Each method returns the raw reply line so the caller (ViewModel)
    // can parse it. Returns the reply on success, throws
    // TimeoutCancellationException via withTimeout on no reply.

    suspend fun otaVersion(timeoutMs: Long = 3000L): String =
        withTimeout(timeoutMs) {
            incomingData
                .onSubscription { send("FW,VERSION") }
                .first { it.startsWith("FW,VERSION,") }
        }

    suspend fun otaBegin(totalBytes: Int, sha256Hex: String,
                         timeoutMs: Long = 5000L): String =
        withTimeout(timeoutMs) {
            incomingData
                .onSubscription { send("FW,BEGIN,$totalBytes,$sha256Hex") }
                .first { it.startsWith("FW,BEGIN,") }
        }

    suspend fun otaChunk(seq: Int, payloadHex: String,
                         timeoutMs: Long = 5000L): String =
        withTimeout(timeoutMs) {
            incomingData
                .onSubscription { send("FW,CHUNK,$seq,$payloadHex") }
                .first {
                    it.startsWith("FW,ACK,") ||
                    it.startsWith("FW,NACK,")
                }
        }

    suspend fun otaEnd(sha256Hex: String,
                       timeoutMs: Long = 30000L): String =
        withTimeout(timeoutMs) {
            incomingData
                .onSubscription { send("FW,END,$sha256Hex") }
                .first { it.startsWith("FW,END,") }
        }

    suspend fun otaAbort(timeoutMs: Long = 3000L): String =
        withTimeout(timeoutMs) {
            incomingData
                .onSubscription { send("FW,ABORT") }
                .first { it.startsWith("FW,ABORT,") }
        }
}
