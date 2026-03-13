package com.loramesh.app.data

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

// ═══════════════════════════════════════════════════════════════
// TelegramBridge — forwards group mesh messages to a Telegram bot
//
// Only group broadcasts (all nodes) and SOS alerts are forwarded.
// Direct node-to-node messages stay private.
//
// Deduplication: tracks the last 60 message fingerprints so that
// if multiple phones receive the same packet and all have signal,
// only the first one to fire actually sends to Telegram.
// ═══════════════════════════════════════════════════════════════

class TelegramBridge(context: Context) {

    private val prefs = context.getSharedPreferences("telegram_bridge", Context.MODE_PRIVATE)

    // Ring buffer of recently forwarded fingerprints (from+text hash)
    private val recentFingerprints = ArrayDeque<String>(60)

    // ─── Persist config ──────────────────────────────────────
    var config: TelegramConfig
        get() = TelegramConfig(
            enabled  = prefs.getBoolean("enabled", false),
            botToken = prefs.getString("bot_token", "") ?: "",
            chatId   = prefs.getString("chat_id", "") ?: ""
        )
        set(value) {
            prefs.edit()
                .putBoolean("enabled", value.enabled)
                .putString("bot_token", value.botToken)
                .putString("chat_id", value.chatId)
                .apply()
        }

    // ─── Forward a received message ──────────────────────────
    // Call this for every incoming RX message.
    // Returns true if the message was actually sent to Telegram.
    suspend fun forward(from: String, text: String, rssi: Int): Boolean {
        val cfg = config
        if (!cfg.enabled) return false
        if (cfg.botToken.isBlank() || cfg.chatId.isBlank()) return false

        // Deduplicate — same sender + same text = same packet arriving via multiple routes
        val fingerprint = "${from}:${text.trim().hashCode()}"
        if (recentFingerprints.contains(fingerprint)) return false
        recentFingerprints.addLast(fingerprint)
        if (recentFingerprints.size > 60) recentFingerprints.removeFirst()

        val isSos = text.startsWith("SOS,")
        val messageText = buildTelegramMessage(from, text, rssi, isSos)

        return sendToTelegram(cfg.botToken, cfg.chatId, messageText)
    }

    // ─── Build the Telegram message text ─────────────────────
    private fun buildTelegramMessage(from: String, text: String, rssi: Int, isSos: Boolean): String {
        return if (isSos) {
            val parts = text.split(",")
            if (parts.size >= 3) {
                val lat = parts[1].trim()
                val lon = parts[2].trim()
                "\uD83D\uDEA8 *SOS EMERGENCY* from *$from*\n" +
                "\uD83D\uDCCD Location: https://maps.google.com/?q=$lat,$lon\n" +
                "_Signal strength: ${rssi}dBm_"
            } else {
                "\uD83D\uDEA8 *SOS EMERGENCY* from *$from* (no GPS fix)\n" +
                "_Signal strength: ${rssi}dBm_"
            }
        } else {
            "\uD83D\uDCE1 *[$from]* $text\n" +
            "_via TiggyOpenMesh \u2022 ${rssi}dBm_"
        }
    }

    // ─── HTTP POST to Telegram bot API ───────────────────────
    private suspend fun sendToTelegram(token: String, chatId: String, text: String): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val url = URL("https://api.telegram.org/bot$token/sendMessage")
                val conn = url.openConnection() as HttpURLConnection
                conn.requestMethod = "POST"
                conn.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
                conn.doOutput = true
                conn.connectTimeout = 8000
                conn.readTimeout = 8000

                val params = "chat_id=${URLEncoder.encode(chatId, "UTF-8")}" +
                    "&text=${URLEncoder.encode(text, "UTF-8")}" +
                    "&parse_mode=Markdown"

                OutputStreamWriter(conn.outputStream).use { it.write(params) }

                val code = conn.responseCode
                if (code != 200) {
                    Log.w("TelegramBridge", "Telegram returned HTTP $code")
                }
                conn.disconnect()
                code == 200
            } catch (e: Exception) {
                Log.e("TelegramBridge", "Failed to send to Telegram: ${e.message}")
                false
            }
        }
    }

    // ─── Test the connection with a sample message ───────────
    suspend fun sendTestMessage(): Boolean {
        val cfg = config
        if (cfg.botToken.isBlank() || cfg.chatId.isBlank()) return false
        return sendToTelegram(
            cfg.botToken, cfg.chatId,
            "\u2705 *TiggyOpenMesh connected!*\nTelegram alerts are working. " +
            "Group broadcasts and SOS emergencies will appear here."
        )
    }
}
