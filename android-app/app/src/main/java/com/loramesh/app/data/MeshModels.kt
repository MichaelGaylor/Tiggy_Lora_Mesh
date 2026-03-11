package com.loramesh.app.data

// ═══════════════════════════════════════════════════════════════
// Data models shared across the app
// ═══════════════════════════════════════════════════════════════

data class ChatMessage(
    val from: String,
    val text: String,
    val rssi: Int = 0,
    val isOutgoing: Boolean = false,
    val timestamp: Long = System.currentTimeMillis()
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
    var boardName: String = ""
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
