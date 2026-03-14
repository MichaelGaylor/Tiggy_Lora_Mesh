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
    val hops: Int = 0
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
    var aesKey: String = "DONTSHARETHEKEY!",
    val relayPins: MutableList<RelayPin> = mutableListOf(),
    val sensorPins: MutableList<SensorPin> = mutableListOf(),
    var frequency: Float = 868.0f,
    var boardName: String = "",
    var spreadingFactor: Int = 9
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
