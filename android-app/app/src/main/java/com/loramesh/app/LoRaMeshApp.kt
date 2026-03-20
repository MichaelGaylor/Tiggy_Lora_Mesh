package com.loramesh.app

import android.app.Application
import com.loramesh.app.ble.BleManager

class LoRaMeshApp : Application() {

    // Singleton BleManager — survives Activity recreation and backgrounding.
    // Foreground service keeps the process alive so BLE stays connected.
    lateinit var bleManager: BleManager
        private set

    override fun onCreate() {
        super.onCreate()
        bleManager = BleManager(applicationContext)
    }
}
