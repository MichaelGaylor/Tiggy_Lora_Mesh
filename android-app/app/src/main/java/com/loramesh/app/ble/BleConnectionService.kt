package com.loramesh.app.ble

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.loramesh.app.LoRaMeshApp
import com.loramesh.app.MainActivity
import com.loramesh.app.R

// ═══════════════════════════════════════════════════════════════
// Foreground Service — keeps BLE connection alive in background
// ═══════════════════════════════════════════════════════════════
// Android kills background processes aggressively. This service
// shows a persistent notification and prevents the OS from
// killing the BLE connection when the app is backgrounded.

class BleConnectionService : Service() {

    companion object {
        const val CHANNEL_ID = "ble_connection"
        const val NOTIFICATION_ID = 1
        const val ACTION_STOP = "com.loramesh.app.STOP_SERVICE"
        const val EXTRA_DEVICE_NAME = "device_name"
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            // Disconnect BLE when user taps "Disconnect" in notification
            (application as LoRaMeshApp).bleManager.disconnect()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }

        val deviceName = intent?.getStringExtra(EXTRA_DEVICE_NAME) ?: "mesh node"
        startForeground(NOTIFICATION_ID, buildNotification(deviceName))
        return START_STICKY
    }

    fun updateNotification(deviceName: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIFICATION_ID, buildNotification(deviceName))
    }

    private fun buildNotification(deviceName: String): Notification {
        // Tap notification → open app
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
        }
        val openPending = PendingIntent.getActivity(
            this, 0, openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        // Stop action
        val stopIntent = Intent(this, BleConnectionService::class.java).apply {
            action = ACTION_STOP
        }
        val stopPending = PendingIntent.getService(
            this, 1, stopIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("TiggyOpenMesh")
            .setContentText("Connected to $deviceName")
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setOngoing(true)
            .setContentIntent(openPending)
            .addAction(0, "Disconnect", stopPending)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "BLE Connection",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Keeps BLE connection alive in background"
            }
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }
}
