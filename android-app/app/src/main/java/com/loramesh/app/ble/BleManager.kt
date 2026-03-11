package com.loramesh.app.ble

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.os.ParcelUuid
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.UUID

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

    // ─── Observable state ────────────────────────────────────
    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState

    private val _discoveredDevices = MutableStateFlow<List<ScannedDevice>>(emptyList())
    val discoveredDevices: StateFlow<List<ScannedDevice>> = _discoveredDevices

    private val _incomingData = MutableStateFlow("")
    val incomingData: StateFlow<String> = _incomingData

    private val _connectedDeviceName = MutableStateFlow("")
    val connectedDeviceName: StateFlow<String> = _connectedDeviceName

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
            val name = device.name ?: "Unknown"
            val entry = ScannedDevice(name, device.address, result.rssi)

            val current = _discoveredDevices.value.toMutableList()
            val idx = current.indexOfFirst { it.address == entry.address }
            if (idx >= 0) current[idx] = entry else current.add(entry)
            _discoveredDevices.value = current
        }
    }

    // ─── Connect ─────────────────────────────────────────────
    fun connect(address: String) {
        stopScan()
        _connectionState.value = ConnectionState.CONNECTING
        val device = bluetoothAdapter?.getRemoteDevice(address) ?: return
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        txCharacteristic = null
        _connectionState.value = ConnectionState.DISCONNECTED
        _connectedDeviceName.value = ""
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _connectedDeviceName.value = g.device.name ?: g.device.address
                    g.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    _connectionState.value = ConnectionState.DISCONNECTED
                    _connectedDeviceName.value = ""
                    txCharacteristic = null
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
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(descriptor)
                }
            }

            _connectionState.value = ConnectionState.CONNECTED
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == BLE_RX_CHAR_UUID) {
                val data = characteristic.value?.toString(Charsets.UTF_8) ?: return
                rxBuffer.append(data)

                // Process complete lines
                while (rxBuffer.contains("\n")) {
                    val nlIdx = rxBuffer.indexOf("\n")
                    val line = rxBuffer.substring(0, nlIdx).trim()
                    rxBuffer.delete(0, nlIdx + 1)
                    if (line.isNotEmpty()) {
                        _incomingData.value = line
                    }
                }
            }
        }
    }

    // ─── Send data to node ───────────────────────────────────
    fun send(data: String) {
        val char = txCharacteristic ?: return
        val g = gatt ?: return
        // BLE max payload ~20 bytes, chunk if needed
        val bytes = (data + "\n").toByteArray(Charsets.UTF_8)
        val chunkSize = 20
        for (i in bytes.indices step chunkSize) {
            val chunk = bytes.sliceArray(i until minOf(i + chunkSize, bytes.size))
            char.value = chunk
            g.writeCharacteristic(char)
            if (i + chunkSize < bytes.size) Thread.sleep(30) // small delay between chunks
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
}
