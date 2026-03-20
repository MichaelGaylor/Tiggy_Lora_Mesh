package com.loramesh.app.data

// ═══════════════════════════════════════════════════════════════
// Data models shared across the app
// ═══════════════════════════════════════════════════════════════

enum class DeliveryStatus {
    SENDING,    // Message sent to node via BLE, waiting for LoRa transmit confirmation
    SENT,       // Node transmitted over LoRa, waiting for remote ACK
    DELIVERED,  // ACK received from remote node
    FAILED      // Timeout: no ACK received
}

data class ChatMessage(
    val from: String,
    val text: String,
    val rssi: Int = 0,
    val isOutgoing: Boolean = false,
    val timestamp: Long = System.currentTimeMillis(),
    val messageId: String? = null,
    val deliveryStatus: DeliveryStatus? = null
)

data class MeshNode(
    val id: String,
    val rssi: Int = 0,
    val lastSeen: Long = System.currentTimeMillis(),
    val hops: Int = 0,
    val age: Int = 0,       // seconds since last heard (from firmware)
    val active: Boolean = true,
    val nextHop: String = "",  // next hop node ID for route chain
    val lat: Double = 0.0,
    val lon: Double = 0.0,
    val hasPosition: Boolean = false
)

// Position received from a mesh node (via POS broadcast)
data class NodePosition(
    val nodeId: String,
    val lat: Double,
    val lon: Double,
    val timestamp: Long = System.currentTimeMillis()
)

data class RelayPin(
    val pin: Int,
    val label: String = "Relay $pin",
    val state: Boolean = false
)

data class SensorPin(
    val pin: Int,
    val label: String = "Sensor $pin",
    val value: Int = 0
)

data class NodeConfig(
    var nodeId: String = "0010",
    var aesKey: String = "",
    val relayPins: MutableList<RelayPin> = mutableListOf(),
    val sensorPins: MutableList<SensorPin> = mutableListOf(),
    var frequency: Float = 868.0f,
    var boardName: String = "",
    var spreadingFactor: Int = 9,
    var powerMode: String = "NORMAL",
    var setupMode: Boolean = false,
    var gpsStatus: String = "OFF"  // OFF, FIX, NOFIX
)

// Sensor reading history (for sparkline charts)
data class SensorReading(
    val value: Int,
    val timestamp: Long = System.currentTimeMillis()
)

// Timer info (from TIMER,LIST response)
data class TimerInfo(
    val pin: Int,
    val isPulse: Boolean = false,
    val action: String = "",       // ON, OFF, or PULSE
    val detail: String = ""        // "123s" remaining, or "3/5x ON" for pulse
)

// Setpoint rule (from SETPOINT,LIST response)
data class SetpointInfo(
    val sensorPin: Int,
    val op: String,                // GT, LT, EQ
    val threshold: Int,
    val targetNode: String,
    val relayPin: Int,
    val action: Int                // 0=OFF, 1=ON
)

// Auto-poll configuration
data class AutoPollConfig(
    val enabled: Boolean = false,
    val target: String = "",
    val interval: Int = 300        // seconds
)

// Spreading factor change — two-phase commit protocol
enum class SfChangePhase {
    IDLE,           // No change in progress
    COLLECTING,     // Phase 1: waiting for node ACKs
    COMMITTED,      // Phase 2: CFGGO sent, nodes switching
    COMPLETE,       // All done
    FAILED          // Timeout or error
}

data class SfChangeState(
    val targetSF: Int = 9,
    val changeId: String = "",
    val expectedNodes: List<String> = emptyList(),
    val ackedNodes: List<String> = emptyList(),
    val phase: SfChangePhase = SfChangePhase.IDLE
)

// Telegram bridge configuration (stored in SharedPreferences, not on device)
data class TelegramConfig(
    val enabled: Boolean = false,
    val botToken: String = "",
    val chatId: String = ""
)

// BLE discovered device
data class BleDevice(
    val name: String,
    val address: String,
    val rssi: Int = 0
)
