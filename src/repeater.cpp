// ═══════════════════════════════════════════════════════════════
// LoRa Mesh Repeater v4.0
// ═══════════════════════════════════════════════════════════════
// Headless mesh repeater + relay/sensor node + BLE phone app
// Uses shared MeshCore library for all mesh protocol logic.
//
// Board-specific parts only:
//   - Radio init (SX1276 vs SX1262)
//   - OLED display
//   - BLE serial service
//   - GPIO relay/sensor control
//   - Serial configuration
//   - EEPROM load/save
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <EEPROM.h>
#include <RadioLib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "Pins.h"
#include "MeshCore.h"
#if defined(RADIO_SX1262)
#include "esp_sleep.h"
#include "driver/gpio.h"
#endif

// Only compile for repeater boards
#if defined(BOARD_LORA32) || defined(BOARD_HELTEC_V3) || defined(BOARD_HELTEC_V4) || defined(BOARD_CUSTOM)

// ─── OLED Display ────────────────────────────────────────────
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define OLED_W 128
#define OLED_H 64
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, OLED_RST);
bool oledAvailable = false;

// ─── Repeater-specific config ────────────────────────────────
#define EEPROM_GPIO_ADDR 410
#define EEPROM_SOLAR_ADDR 430
#define MAX_RELAY_PINS_CFG 8
#define MAX_SENSOR_PINS_CFG 6
#define SOLAR_OLED_WAKE_MS 10000

#ifndef DEBUG
#define DEBUG 1
#endif
#if DEBUG
  #define debugPrint(x) Serial.println(x)
#else
  #define debugPrint(x)
#endif

// ─── Hardware ────────────────────────────────────────────────
#ifdef CONFIG_IDF_TARGET_ESP32S3
SPIClass loraSPI(SPI);
#else
SPIClass loraSPI(VSPI);
#endif

#if defined(RADIO_SX1262)
SX1262 radio = new Module(RADIO_CS, RADIO_DIO1, RADIO_RST, RADIO_BUSY, loraSPI);
#elif defined(RADIO_SX1276)
SX1276 radio = new Module(RADIO_CS, RADIO_DIO0, RADIO_RST, -1, loraSPI);
#endif

// ─── GPIO State ──────────────────────────────────────────────
uint8_t relayPins[MAX_RELAY_PINS_CFG];
uint8_t relayCount = 0;
uint8_t sensorPins[MAX_SENSOR_PINS_CFG];
uint8_t sensorCount = 0;

// ─── Timing ──────────────────────────────────────────────────
unsigned long nextHeartbeatTime = 0;
unsigned long lastRouteClean = 0;
unsigned long lastOledRefresh = 0;

// ─── Gateway Mode ───────────────────────────────────────────
bool gatewayMode = false;

// ─── Solar Mode ─────────────────────────────────────────────
// Low-power mode: OLED off, BLE off, SX1262 duty-cycle RX, ESP32 light sleep.
// Persisted to EEPROM so it survives power cycles.
bool solarMode = false;
bool solarOledTemporary = false;
unsigned long solarOledWakeUntil = 0;

// ─── BLE State (declared early for solar mode access) ──────
bool bleConnected = false;

// ─── Forward Declarations ────────────────────────────────────
void bleSend(const String& line);
void processBleCommand(const String& line);
void saveConfig();
void loadConfig();
void setupGPIO();
void setupBLE();
bool isPinSafe(int pin);

// ═══════════════════════════════════════════════════════════════
// SECTION: Radio bridge — connects MeshCore to physical radio
// ═══════════════════════════════════════════════════════════════

void IRAM_ATTR onRadioRx() { mesh.rxFlag = true; }

// Start listening — uses duty-cycle RX in solar mode (SX1262 only)
void radioStartListening() {
#if defined(RADIO_SX1262)
    if (solarMode) {
        // Duty-cycle RX: radio sleeps between brief RX windows
        // Auto-calculates optimal timing from preamble length
        radio.startReceiveDutyCycleAuto(LORA_PREAMBLE, 2);
    } else {
        radio.startReceive();
    }
#else
    radio.startReceive();
#endif
}

// MeshCore calls this to transmit raw packets
void radioTransmit(uint8_t* pkt, size_t len) {
    radio.standby();
    radio.transmit(pkt, len);
    radioStartListening();
}

// MeshCore calls this to check if channel is free
bool radioChannelFree() {
    // Use RSSI scan — if below noise floor, channel is free
    float rssi = radio.getRSSI();
    return rssi < -120.0;
}

void setupRadio() {
#if defined(RADIO_SX1262)
    debugPrint("Initializing SX1262...");
    int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                            LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 0, false);
#elif defined(RADIO_SX1276)
    debugPrint("Initializing SX1276...");
    int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                            LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 0);
#endif
    if (state != RADIOLIB_ERR_NONE) {
        debugPrint("Radio FAILED: " + String(state));
        if (BOARD_LED >= 0) {
            while (1) {
                digitalWrite(BOARD_LED, HIGH); delay(100);
                digitalWrite(BOARD_LED, LOW); delay(100);
            }
        }
    }
#if defined(RADIO_SX1262)
    radio.setDio1Action(onRadioRx);
#elif defined(RADIO_SX1276)
    radio.setDio0Action(onRadioRx, RISING);
#endif
    radioStartListening();
    debugPrint("Radio OK: " + String(LORA_FREQ, 1) + " MHz");
}

void receiveCheck() {
    if (!mesh.rxFlag) return;
    mesh.rxFlag = false;

    uint8_t pkt[256];
    size_t len = radio.getPacketLength();
    if (len < 7 || len > sizeof(pkt)) { radioStartListening(); return; }

    int state = radio.readData(pkt, len);
    mesh.lastRSSI = radio.getRSSI();
    radioStartListening();
    if (state != RADIOLIB_ERR_NONE) return;

    MeshPacket mp;
    if (mesh.parseRawPacket(pkt, len, mp)) {
        mesh.processPacket(mp);

        // Gateway mode: forward raw packet over serial for bridge server
        if (gatewayMode) {
            Serial.print("PKT,");
            Serial.println(mesh.toHex(pkt, len));
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Solar Mode (SX1262 boards only)
// ═══════════════════════════════════════════════════════════════
#if defined(RADIO_SX1262)

void startSolarMode() {
    solarMode = true;
    debugPrint("SOLAR: Entering low-power mode");

    // Disable OLED
#ifdef VEXT_CTRL
    digitalWrite(VEXT_CTRL, HIGH);  // Vext OFF
#endif
    oledAvailable = false;

    // Disable BLE
    BLEDevice::deinit(true);
    bleConnected = false;
    debugPrint("SOLAR: BLE disabled");

    // Disable ADC (saves a tiny bit)
#ifdef ADC_CTRL
    digitalWrite(ADC_CTRL, LOW);
#endif

    // Switch radio to duty-cycle RX
    radio.standby();
    radioStartListening();
    debugPrint("SOLAR: Radio in duty-cycle RX");

    // Configure ESP32-S3 light sleep wake sources
    // Wake on DIO1 (radio packet detected)
    gpio_num_t dio1 = (gpio_num_t)RADIO_DIO1;
    esp_sleep_enable_gpio_wakeup();
    gpio_wakeup_enable(dio1, GPIO_INTR_HIGH_LEVEL);

    // Wake on timer (for heartbeat every 60s)
    esp_sleep_enable_timer_wakeup(10000000ULL);  // 10 seconds max sleep

#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    // Wake on PRG button press (for OLED status check)
    gpio_num_t btn = (gpio_num_t)BOARD_BUTTON;
    gpio_wakeup_enable(btn, GPIO_INTR_LOW_LEVEL);
#endif

    debugPrint("SOLAR: Light sleep configured");
}

void stopSolarMode() {
    solarMode = false;
    debugPrint("SOLAR: Restoring normal operation");

    // Disable light sleep wake sources
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    // Re-enable OLED
#ifdef VEXT_CTRL
    digitalWrite(VEXT_CTRL, LOW);  // Vext ON
    delay(100);
#endif

    // Reinit OLED
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        oledAvailable = true;
        oled.clearDisplay();
        oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0); oled.println("SOLAR MODE OFF");
        oled.println("Normal operation");
        oled.display();
    }

    // Re-enable ADC
#ifdef ADC_CTRL
    digitalWrite(ADC_CTRL, HIGH);
#endif

    // Switch radio back to continuous RX
    radio.standby();
    radioStartListening();
    debugPrint("SOLAR: Radio in continuous RX");

    // Re-init BLE
    setupBLE();
    debugPrint("SOLAR: BLE re-enabled");
}

void solarEnterLightSleep() {
    // Brief LED blink as sign-of-life before sleeping
    if (BOARD_LED >= 0) { digitalWrite(BOARD_LED, HIGH); delay(2); digitalWrite(BOARD_LED, LOW); }

    // Enter light sleep — CPU stops, wakes on DIO1/timer/button
    esp_light_sleep_start();

    // Woke up — check why
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    if (cause == ESP_SLEEP_WAKEUP_GPIO && !digitalRead(BOARD_BUTTON)) {
        // Button pressed — temporarily show OLED status
        if (!solarOledTemporary) {
            solarOledTemporary = true;
            solarOledWakeUntil = millis() + SOLAR_OLED_WAKE_MS;
#ifdef VEXT_CTRL
            digitalWrite(VEXT_CTRL, LOW);  // Vext ON
            delay(50);
#endif
            Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
            if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
                oledAvailable = true;
                oled.clearDisplay();
                oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
                oled.setCursor(0, 0);
                oled.println("SOLAR MODE");
                oled.print("ID: "); oled.println(mesh.localID);
                oled.print("N:"); oled.print(mesh.knownCount);
                oled.print(" RX:"); oled.println(mesh.packetsReceived);
                oled.print("FWD:"); oled.println(mesh.packetsForwarded);
                oled.print("RSSI:"); oled.print(mesh.lastRSSI); oled.println("dBm");
                unsigned long up = millis() / 1000;
                oled.print("Up:");
                if (up > 3600) { oled.print(up / 3600); oled.print("h"); }
                oled.print((up % 3600) / 60); oled.print("m");
                oled.display();
            }
        }
    }
#endif

    // Check if temporary OLED should be turned off
    if (solarOledTemporary && millis() > solarOledWakeUntil) {
        solarOledTemporary = false;
        oledAvailable = false;
#ifdef VEXT_CTRL
        digitalWrite(VEXT_CTRL, HIGH);  // Vext OFF
#endif
    }
}

#endif // RADIO_SX1262

// ═══════════════════════════════════════════════════════════════
// SECTION: GPIO Control
// ═══════════════════════════════════════════════════════════════

bool isPinSafe(int pin) {
    const int forbidden[] = {
        BOARD_SPI_MOSI, BOARD_SPI_MISO, BOARD_SPI_SCK,
        RADIO_CS, RADIO_RST, RADIO_DIO1,
#if defined(RADIO_SX1262)
        RADIO_BUSY,
#endif
        BOARD_I2C_SDA, BOARD_I2C_SCL,
        0  // boot
    };
    for (int f : forbidden) { if (f >= 0 && pin == f) return false; }
    if (pin < 0 || pin > 48) return false;
    return true;
}

void setupGPIO() {
    for (int i = 0; i < relayCount; i++) {
        if (isPinSafe(relayPins[i])) {
            pinMode(relayPins[i], OUTPUT);
            digitalWrite(relayPins[i], LOW);
        }
    }
    for (int i = 0; i < sensorCount; i++) {
        if (isPinSafe(sensorPins[i])) {
            pinMode(sensorPins[i], INPUT);
        }
    }
}

// CMD handler — called by MeshCore when a CMD arrives for us
void handleCmd(const String& from, const String& cmdBody) {
    int c1 = cmdBody.indexOf(',');
    String action = (c1 > 0) ? cmdBody.substring(0, c1) : cmdBody;
    String rest   = (c1 > 0) ? cmdBody.substring(c1 + 1) : "";

    if (action == "SET") {
        int c2 = rest.indexOf(',');
        if (c2 < 0) return;
        int pin = rest.substring(0, c2).toInt();
        int val = rest.substring(c2 + 1).toInt();
        if (!isPinSafe(pin)) return;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, val ? HIGH : LOW);
        mesh.cmdsExecuted++;
        // Send response back over mesh
        String mid = mesh.generateMsgID();
        String rsp = "CMD,RSP," + String(pin) + "," + String(val);
        String hex = mesh.encryptMsg(rsp, mid);
        String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
        bleSend("CMD,RSP," + String(pin) + "," + String(val));
    }
    else if (action == "GET") {
        int pin = rest.toInt();
        if (!isPinSafe(pin)) return;
        int value = (pin >= 34) ? analogRead(pin) : digitalRead(pin);
        mesh.cmdsExecuted++;
        String mid = mesh.generateMsgID();
        String rsp = "CMD,RSP," + String(pin) + "," + String(value);
        String hex = mesh.encryptMsg(rsp, mid);
        String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
        bleSend("CMD,RSP," + String(pin) + "," + String(value));
    }
    else if (action == "PULSE") {
        int c2 = rest.indexOf(',');
        if (c2 < 0) return;
        int pin = rest.substring(0, c2).toInt();
        int ms  = rest.substring(c2 + 1).toInt();
        if (!isPinSafe(pin) || ms > 30000) return;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH); delay(ms); digitalWrite(pin, LOW);
        mesh.cmdsExecuted++;
        String mid = mesh.generateMsgID();
        String rsp = "CMD,RSP," + String(pin) + ",0";
        String hex = mesh.encryptMsg(rsp, mid);
        String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
        bleSend("CMD,RSP," + String(pin) + ",0");
    }
    else if (action == "LIST") {
        String rsp = "PINS,R:";
        for (int i = 0; i < relayCount; i++) { if (i) rsp += ","; rsp += String(relayPins[i]); }
        rsp += "|S:";
        for (int i = 0; i < sensorCount; i++) { if (i) rsp += ","; rsp += String(sensorPins[i]); }
        bleSend(rsp);
    }
}

// Message handler — called by MeshCore when a message arrives for us
void handleMessage(const String& from, const String& text, int rssi) {
    debugPrint("MSG from " + from + ": " + text);
    bleSend("RX," + from + "," + text + "," + String(rssi));
}

// Node discovery handler
void handleNodeDiscovered(const String& id, int rssi) {
    debugPrint("New node: " + id);
    bleSend("NODE," + id + "," + String(rssi) + ",1");
}

// ═══════════════════════════════════════════════════════════════
// SECTION: BLE Serial (Nordic UART Service)
// ═══════════════════════════════════════════════════════════════

#define BLE_SERVICE_UUID   "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_TX_CHAR_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX_CHAR_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer* bleServer = nullptr;
BLECharacteristic* bleRxChar = nullptr;
// bleConnected declared earlier (near solar mode vars) for forward reference
String bleRxBuffer;

class BleServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer* s) override    { bleConnected = true;  debugPrint("BLE: connected"); }
    void onDisconnect(BLEServer* s) override { bleConnected = false; debugPrint("BLE: disconnected"); s->startAdvertising(); }
};

class BleTxCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        String data = c->getValue().c_str();
        bleRxBuffer += data;
        while (bleRxBuffer.indexOf('\n') >= 0) {
            int nl = bleRxBuffer.indexOf('\n');
            String line = bleRxBuffer.substring(0, nl); line.trim();
            bleRxBuffer = bleRxBuffer.substring(nl + 1);
            if (line.length() > 0) processBleCommand(line);
        }
    }
};

void setupBLE() {
    String bleName = "LoRaMesh-" + String(mesh.localID);
    BLEDevice::init(bleName.c_str());
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new BleServerCB());

    BLEService* service = bleServer->createService(BLE_SERVICE_UUID);
    BLECharacteristic* txChar = service->createCharacteristic(BLE_TX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    txChar->setCallbacks(new BleTxCB());

    bleRxChar = service->createCharacteristic(BLE_RX_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    bleRxChar->addDescriptor(new BLE2902());

    service->start();
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    BLEDevice::startAdvertising();
    debugPrint("BLE ready: " + bleName);
}

void bleSend(const String& line) {
    if (!bleConnected || !bleRxChar) return;
    String msg = line + "\n";
    for (unsigned int i = 0; i < msg.length(); i += 20) {
        String chunk = msg.substring(i, min((unsigned int)msg.length(), i + 20));
        bleRxChar->setValue(chunk.c_str());
        bleRxChar->notify();
        if (i + 20 < msg.length()) delay(10);
    }
}

void processBleCommand(const String& line) {
    debugPrint("BLE CMD: " + line);

    if (line.startsWith("MSG,")) {
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        if (c2 < 0) return;
        String target = line.substring(c1 + 1, c2);
        String text = line.substring(c2 + 1);
        uint16_t dest = strtol(target.c_str(), nullptr, 16);
        String mid = mesh.generateMsgID();
        String payload = String(mesh.localID) + "," + target + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," +
                         mesh.encryptMsg(text, mid);
        mesh.transmitPacket(dest, payload);
        bleSend("OK,SENT," + target);
    }
    else if (line.startsWith("CMD,")) {
        handleCmd("LOCAL", line.substring(4));
    }
    else if (line.startsWith("ID ")) {
        String id = line.substring(3); id.trim(); id.toUpperCase();
        if (mesh.isValidNodeID(id)) {
            strncpy(mesh.localID, id.c_str(), NODE_ID_LEN);
            mesh.localID[NODE_ID_LEN] = '\0';
            bleSend("OK,ID," + String(mesh.localID));
        }
    }
    else if (line.startsWith("KEY ")) {
        String key = line.substring(4); key.trim();
        if ((int)key.length() == AES_KEY_LEN) {
            strncpy(mesh.aes_key_string, key.c_str(), AES_KEY_LEN);
            mesh.aes_key_string[AES_KEY_LEN] = '\0';
            bleSend("OK,KEY");
        }
    }
    else if (line == "STATUS") {
        bleSend("STATUS,ID:" + String(mesh.localID) + ",BOARD:" + String(BOARD_NAME) +
                ",FREQ:" + String(LORA_FREQ, 1) + ",NODES:" + String(mesh.knownCount) +
                ",RX:" + String(mesh.packetsReceived) + ",FWD:" + String(mesh.packetsForwarded) +
                ",GW:" + String(gatewayMode ? "ON" : "OFF") +
                ",HEAP:" + String(ESP.getFreeHeap()));
    }
    else if (line == "SAVE") { saveConfig(); bleSend("OK,SAVED"); }
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Serial Configuration
// ═══════════════════════════════════════════════════════════════

void handleSerialConfig() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n'); line.trim();
    if (line.length() == 0) return;

    if (line.startsWith("ID ")) {
        String id = line.substring(3); id.trim(); id.toUpperCase();
        if (mesh.isValidNodeID(id)) {
            strncpy(mesh.localID, id.c_str(), NODE_ID_LEN);
            mesh.localID[NODE_ID_LEN] = '\0';
            Serial.println("OK: ID set to " + String(mesh.localID));
        } else Serial.println("ERROR: ID must be 4 hex chars");
    }
    else if (line.startsWith("KEY ")) {
        String key = line.substring(4); key.trim();
        if ((int)key.length() == AES_KEY_LEN) {
            strncpy(mesh.aes_key_string, key.c_str(), AES_KEY_LEN);
            mesh.aes_key_string[AES_KEY_LEN] = '\0';
            Serial.println("OK: AES key updated");
        } else Serial.println("ERROR: Key must be exactly 16 characters");
    }
    else if (line.startsWith("RELAY ")) {
        String pins = line.substring(6); relayCount = 0;
        int start = 0, end;
        while ((end = pins.indexOf(',', start)) != -1 && relayCount < MAX_RELAY_PINS_CFG) {
            relayPins[relayCount++] = pins.substring(start, end).toInt(); start = end + 1;
        }
        if (relayCount < MAX_RELAY_PINS_CFG) relayPins[relayCount++] = pins.substring(start).toInt();
        setupGPIO(); saveConfig();
        Serial.print("OK: Relay pins: ");
        for (int i = 0; i < relayCount; i++) { if (i) Serial.print(","); Serial.print(relayPins[i]); }
        Serial.println();
    }
    else if (line.startsWith("SENSOR ")) {
        String pins = line.substring(7); sensorCount = 0;
        int start = 0, end;
        while ((end = pins.indexOf(',', start)) != -1 && sensorCount < MAX_SENSOR_PINS_CFG) {
            sensorPins[sensorCount++] = pins.substring(start, end).toInt(); start = end + 1;
        }
        if (sensorCount < MAX_SENSOR_PINS_CFG) sensorPins[sensorCount++] = pins.substring(start).toInt();
        setupGPIO(); saveConfig();
        Serial.print("OK: Sensor pins: ");
        for (int i = 0; i < sensorCount; i++) { if (i) Serial.print(","); Serial.print(sensorPins[i]); }
        Serial.println();
    }
    else if (line == "STATUS") {
        Serial.println("═══ Mesh Repeater Status ═══");
        Serial.println("ID:       " + String(mesh.localID));
        Serial.println("Freq:     " + String(LORA_FREQ, 1) + " MHz");
        Serial.println("Nodes:    " + String(mesh.knownCount));
        Serial.println("RX:       " + String(mesh.packetsReceived));
        Serial.println("Fwd:      " + String(mesh.packetsForwarded));
        Serial.println("Routes:   " + String(mesh.routingTable.size()));
        Serial.println("Heap:     " + String(ESP.getFreeHeap()));
        Serial.println("Gateway:  " + String(gatewayMode ? "ON" : "OFF"));
        Serial.println("Solar:    " + String(solarMode ? "ON" : "OFF"));
    }
    else if (line == "SAVE") { saveConfig(); Serial.println("OK: Saved"); }
    else if (line == "RESET") {
        strncpy(mesh.localID, "0010", NODE_ID_LEN + 1);
        strncpy(mesh.aes_key_string, "DONTSHARETHEKEY!", AES_KEY_LEN + 1);
        uint8_t d[] = DEFAULT_RELAY_PINS; relayCount = sizeof(d)/sizeof(d[0]); memcpy(relayPins, d, relayCount);
        uint8_t s[] = DEFAULT_SENSOR_PINS; sensorCount = sizeof(s)/sizeof(s[0]); memcpy(sensorPins, s, sensorCount);
        saveConfig(); Serial.println("OK: Reset to defaults");
    }
    else if (line.startsWith("PKT,")) {
        // Gateway inject: receive hex-encoded raw packet from bridge server
        if (!gatewayMode) { Serial.println("ERROR: Gateway mode not enabled"); return; }
        String hexData = line.substring(4); hexData.trim();
        if (hexData.length() < 14 || hexData.length() > 512) { Serial.println("ERROR: Invalid PKT length"); return; }
        uint8_t pkt[256];
        int pktLen = 0;
        mesh.hexToBytes(hexData, pkt, pktLen);
        if (pktLen < 7) { Serial.println("ERROR: Packet too short"); return; }
        // Transmit into local mesh via radio
        if (mesh.canTransmit()) {
            radioTransmit(pkt, pktLen);
            Serial.println("OK,PKT,TX," + String(pktLen));
        } else {
            Serial.println("ERROR: Duty cycle limit");
        }
    }
    else if (line == "GATEWAY ON") {
        gatewayMode = true;
        Serial.println("OK: Gateway mode ON — raw packets will be forwarded over serial");
    }
    else if (line == "GATEWAY OFF") {
        gatewayMode = false;
        Serial.println("OK: Gateway mode OFF");
    }
#if defined(RADIO_SX1262)
    else if (line == "SOLAR ON") {
        solarMode = true;
        saveConfig();
        Serial.println("OK: Solar mode ON — entering low-power state");
        delay(100);
        startSolarMode();
    }
    else if (line == "SOLAR OFF") {
        stopSolarMode();
        saveConfig();
        Serial.println("OK: Solar mode OFF — normal operation restored");
    }
#endif
    else Serial.println("Commands: ID xxxx | KEY xxx... | RELAY 2,4,12 | SENSOR 34,36 | STATUS | SAVE | RESET | GATEWAY ON/OFF"
#if defined(RADIO_SX1262)
        " | SOLAR ON/OFF"
#endif
    );
}

// ═══════════════════════════════════════════════════════════════
// SECTION: EEPROM
// ═══════════════════════════════════════════════════════════════

void saveConfig() {
    EEPROM.put(0, mesh.localID);
    EEPROM.put(EEPROM_KEY_ADDR, mesh.aes_key_string);
    EEPROM.put(EEPROM_GPIO_ADDR, relayCount);
    EEPROM.put(EEPROM_GPIO_ADDR + 1, relayPins);
    EEPROM.put(EEPROM_GPIO_ADDR + 1 + MAX_RELAY_PINS_CFG, sensorCount);
    EEPROM.put(EEPROM_GPIO_ADDR + 2 + MAX_RELAY_PINS_CFG, sensorPins);
    EEPROM.write(EEPROM_SOLAR_ADDR, solarMode ? 1 : 0);
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
    EEPROM.commit();
}

void loadConfig() {
    if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
        debugPrint("First boot - using defaults");
        uint8_t d[] = DEFAULT_RELAY_PINS; relayCount = sizeof(d)/sizeof(d[0]); memcpy(relayPins, d, relayCount);
        uint8_t s[] = DEFAULT_SENSOR_PINS; sensorCount = sizeof(s)/sizeof(s[0]); memcpy(sensorPins, s, sensorCount);
        saveConfig();
        return;
    }
    EEPROM.get(0, mesh.localID);
    if (!mesh.isValidNodeID(String(mesh.localID))) strncpy(mesh.localID, "0010", NODE_ID_LEN + 1);

    EEPROM.get(EEPROM_KEY_ADDR, mesh.aes_key_string);
    bool keyValid = (strlen(mesh.aes_key_string) == AES_KEY_LEN);
    if (keyValid) for (int i = 0; i < AES_KEY_LEN; i++)
        if (mesh.aes_key_string[i] < 0x20 || mesh.aes_key_string[i] > 0x7E) { keyValid = false; break; }
    if (!keyValid) strncpy(mesh.aes_key_string, "DONTSHARETHEKEY!", AES_KEY_LEN + 1);

    EEPROM.get(EEPROM_GPIO_ADDR, relayCount);
    if (relayCount > MAX_RELAY_PINS_CFG) relayCount = 0;
    EEPROM.get(EEPROM_GPIO_ADDR + 1, relayPins);
    EEPROM.get(EEPROM_GPIO_ADDR + 1 + MAX_RELAY_PINS_CFG, sensorCount);
    if (sensorCount > MAX_SENSOR_PINS_CFG) sensorCount = 0;
    EEPROM.get(EEPROM_GPIO_ADDR + 2 + MAX_RELAY_PINS_CFG, sensorPins);

    // Load solar mode flag
    uint8_t solarByte = EEPROM.read(EEPROM_SOLAR_ADDR);
    solarMode = (solarByte == 1);
}

// ═══════════════════════════════════════════════════════════════
// SECTION: OLED Status Display
// ═══════════════════════════════════════════════════════════════

void updateOLED() {
    if (!oledAvailable) return;
    if (millis() - lastOledRefresh < 5000) return;
    lastOledRefresh = millis();

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.print("MESH RPT "); oled.print(mesh.localID);
    oled.drawLine(0, 10, OLED_W, 10, SSD1306_WHITE);

    oled.setCursor(0, 14);
    oled.print("RX:"); oled.print(mesh.packetsReceived);
    oled.setCursor(64, 14);
    oled.print("FWD:"); oled.print(mesh.packetsForwarded);

    oled.setCursor(0, 24);
    oled.print("N:"); oled.print(mesh.knownCount);
    oled.setCursor(40, 24);
    oled.print("R:"); oled.print(mesh.routingTable.size());
    oled.setCursor(80, 24);
    if (gatewayMode) oled.print("GW");
    else if (bleConnected) oled.print("BLE");

    oled.setCursor(0, 34);
    oled.print("RSSI:"); oled.print(mesh.lastRSSI); oled.print("dBm");

    oled.setCursor(0, 44);
    oled.print("Relay:");
    for (int i = 0; i < relayCount; i++) oled.print(digitalRead(relayPins[i]) ? "1" : "0");

    oled.setCursor(0, 54);
    unsigned long up = millis() / 1000;
    oled.print("Up:");
    if (up > 3600) { oled.print(up / 3600); oled.print("h"); }
    oled.print((up % 3600) / 60); oled.print("m");

    oled.display();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Setup & Loop
// ═══════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n═══════════════════════════════════");
    Serial.println("  LoRa Mesh Repeater v4.0");
    Serial.println("  Board: " + String(BOARD_NAME));
    Serial.println("═══════════════════════════════════");

    randomSeed(analogRead(0));

    if (BOARD_LED >= 0) { pinMode(BOARD_LED, OUTPUT); digitalWrite(BOARD_LED, LOW); }
#ifdef VEXT_CTRL
    pinMode(VEXT_CTRL, OUTPUT); digitalWrite(VEXT_CTRL, LOW);
#endif
#ifdef ADC_CTRL
    pinMode(ADC_CTRL, OUTPUT); digitalWrite(ADC_CTRL, HIGH);  // Enable battery voltage divider
#endif
#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    pinMode(BOARD_BUTTON, INPUT_PULLUP);
#endif

    EEPROM.begin(EEPROM_SIZE);
    loadConfig();

    // Wire up MeshCore callbacks
    mesh.onTransmitRaw = radioTransmit;
    mesh.onChannelFree = radioChannelFree;
    mesh.onMessage = handleMessage;
    mesh.onCmd = handleCmd;
    mesh.onNodeDiscovered = handleNodeDiscovered;

    // SPI + Radio
    loraSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, RADIO_CS);
    setupRadio();

    setupGPIO();

    // ─── Solar mode boot path ──────────────────────────────────
    // Skip BLE and OLED init — go straight to low-power operation
#if defined(RADIO_SX1262)
    if (solarMode) {
        // Brief OLED splash then enter solar mode
        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
        if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
            oled.clearDisplay();
            oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
            oled.setCursor(0, 0);
            oled.println("SOLAR MODE");
            oled.println("ID: " + String(mesh.localID));
            oled.println(String(LORA_FREQ, 1) + " MHz");
            oled.display();
            delay(2000);
        }
        nextHeartbeatTime = millis() + random(2000, 8000);
        startSolarMode();
        Serial.println("Solar mode active. Send SOLAR OFF to restore normal operation.");
        return;
    }
#endif

    // ─── Normal boot path ──────────────────────────────────────
    setupBLE();

    // OLED
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        oledAvailable = true;
        oled.clearDisplay();
        oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);  oled.println("MESH REPEATER v4.0");
        oled.println("ID: " + String(mesh.localID));
        oled.println(String(LORA_FREQ, 1) + " MHz");
        oled.println("Relays: " + String(relayCount));
        oled.println("BLE: Ready");
        oled.display();
    }

    nextHeartbeatTime = millis() + random(2000, 8000);

    if (BOARD_LED >= 0) {
        for (int i = 0; i < 3; i++) { digitalWrite(BOARD_LED, HIGH); delay(100); digitalWrite(BOARD_LED, LOW); delay(100); }
    }

    Serial.println("Ready. Type STATUS for info.");
    debugPrint("Free heap: " + String(ESP.getFreeHeap()));
}

void loop() {
    // ─── Solar mode fast-path ──────────────────────────────────
#if defined(RADIO_SX1262)
    if (solarMode) {
        // 1. Radio RX
        receiveCheck();

        // 2. Process jittered forwards
        mesh.processPendingForwards();

        // 3. Heartbeat (60s interval in solar mode)
        if (millis() > nextHeartbeatTime) {
            mesh.sendHeartbeat();
            nextHeartbeatTime = millis() + HB_INTERVAL_SOLAR + random(-5000, 5000);
            if (BOARD_LED >= 0) { digitalWrite(BOARD_LED, HIGH); delay(2); digitalWrite(BOARD_LED, LOW); }
        }

        // 4. Route cleanup (every 60s in solar mode)
        if (millis() - lastRouteClean > 60000) {
            mesh.pruneStale();
            lastRouteClean = millis();
        }

        // 5. Serial — only SOLAR OFF, STATUS, and gateway PKT commands
        if (Serial.available()) {
            String line = Serial.readStringUntil('\n'); line.trim();
            if (line == "SOLAR OFF") {
                stopSolarMode();
                saveConfig();
                Serial.println("OK: Solar mode OFF — normal operation restored");
            } else if (line == "STATUS") {
                Serial.println("═══ Solar Repeater Status ═══");
                Serial.println("ID:       " + String(mesh.localID));
                Serial.println("Nodes:    " + String(mesh.knownCount));
                Serial.println("RX:       " + String(mesh.packetsReceived));
                Serial.println("Fwd:      " + String(mesh.packetsForwarded));
                Serial.println("Solar:    ON");
                Serial.println("Heap:     " + String(ESP.getFreeHeap()));
            } else if (line.startsWith("PKT,") && gatewayMode) {
                // Gateway inject still works in solar mode
                String hexData = line.substring(4); hexData.trim();
                if (hexData.length() >= 14 && hexData.length() <= 512) {
                    uint8_t pkt[256]; int pktLen = 0;
                    mesh.hexToBytes(hexData, pkt, pktLen);
                    if (pktLen >= 7 && mesh.canTransmit()) {
                        radioTransmit(pkt, pktLen);
                        Serial.println("OK,PKT,TX," + String(pktLen));
                    }
                }
            }
        }

        // 6. Enter light sleep until next event
        solarEnterLightSleep();
        return;
    }
#endif

    // ─── Normal loop ───────────────────────────────────────────
    // 1. Radio RX — MeshCore handles routing, forwarding, decryption
    receiveCheck();

    // 2. Process jittered forwards
    mesh.processPendingForwards();

    // 3. Heartbeat
    if (millis() > nextHeartbeatTime) {
        mesh.sendHeartbeat();
        nextHeartbeatTime = millis() + HB_INTERVAL + random(-2000, 2000);
        if (BOARD_LED >= 0) { digitalWrite(BOARD_LED, HIGH); delay(10); digitalWrite(BOARD_LED, LOW); }
    }

    // 4. Route cleanup (every 30s)
    if (millis() - lastRouteClean > 30000) {
        mesh.pruneStale();
        lastRouteClean = millis();
    }

    // 5. Serial config
    handleSerialConfig();

    // 6. OLED refresh
    updateOLED();

    // 7. Button → manual heartbeat
#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    static bool lastBtn = true;
    bool btn = digitalRead(BOARD_BUTTON);
    if (!btn && lastBtn) { mesh.sendHeartbeat(); Serial.println("Manual heartbeat sent"); }
    lastBtn = btn;
#endif

    yield();
}

#endif // repeater boards
