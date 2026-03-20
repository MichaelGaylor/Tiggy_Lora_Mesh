// ═══════════════════════════════════════════════════════════════
// TiggyOpenMesh Repeater v4.0
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
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "Pins.h"
#include "MeshCore.h"
#include <TinyGPSPlus.h>
#if defined(RADIO_SX1262)
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#endif

// Only compile for repeater boards
#if defined(BOARD_LORA32) || defined(BOARD_HELTEC_V3) || defined(BOARD_HELTEC_V4) || defined(BOARD_XIAO_S3) || defined(BOARD_CUSTOM)

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
SPIClass loraSPI(FSPI);
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

// ─── Pulse Counter Mode ─────────────────────────────────────
// Sensor pin modes: 0=auto (analog/digital), 1=pulse counter
#define SENSOR_MODE_AUTO   0
#define SENSOR_MODE_PULSE  1
#define MAX_PULSE_PINS     4

uint8_t sensorMode[MAX_SENSOR_PINS_CFG] = {0}; // mode per sensor pin

struct PulseCounter {
    uint8_t pin;
    volatile uint32_t count;
    uint32_t lastRead;           // count at last rate calculation
    unsigned long lastReadTime;  // millis() at last rate calculation
    float rate;                  // pulses per second (calculated)
    uint16_t sampleWindowMs;     // configurable window (default 5000ms)
    bool active;
};
static PulseCounter pulseCounters[MAX_PULSE_PINS];
static uint8_t pulseCounterCount = 0;

// ISR handlers — single-instruction increments, IRAM_ATTR keeps them in fast RAM.
// These complete in <1us and do NOT touch radio state, so they cannot cause
// packet loss or interfere with LoRa reception (which takes milliseconds).
static void IRAM_ATTR pulseISR0() { pulseCounters[0].count++; }
static void IRAM_ATTR pulseISR1() { pulseCounters[1].count++; }
static void IRAM_ATTR pulseISR2() { pulseCounters[2].count++; }
static void IRAM_ATTR pulseISR3() { pulseCounters[3].count++; }
static void (*pulseISRs[MAX_PULSE_PINS])() = { pulseISR0, pulseISR1, pulseISR2, pulseISR3 };

// Find pulse counter for a pin, or -1 if not found
static int findPulseCounter(uint8_t pin) {
    for (int i = 0; i < pulseCounterCount; i++) {
        if (pulseCounters[i].pin == pin && pulseCounters[i].active)
            return i;
    }
    return -1;
}

// Enable pulse counting on a pin
static bool enablePulseCounter(uint8_t pin) {
    if (findPulseCounter(pin) >= 0) return true; // already active
    if (pulseCounterCount >= MAX_PULSE_PINS) return false;
    int idx = pulseCounterCount++;
    pulseCounters[idx].pin = pin;
    pulseCounters[idx].count = 0;
    pulseCounters[idx].lastRead = 0;
    pulseCounters[idx].lastReadTime = millis();
    pulseCounters[idx].rate = 0;
    pulseCounters[idx].sampleWindowMs = 5000;
    pulseCounters[idx].active = true;
    pinMode(pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pin), pulseISRs[idx], RISING);
    return true;
}

// Disable pulse counting on a pin
static void disablePulseCounter(uint8_t pin) {
    int idx = findPulseCounter(pin);
    if (idx < 0) return;
    detachInterrupt(digitalPinToInterrupt(pin));
    pulseCounters[idx].active = false;
}

// Read a pulse counter — returns count since last read (delta).
// Brief interrupt disable ensures atomic read of count vs lastRead.
static uint32_t readPulseCount(uint8_t pin) {
    int idx = findPulseCounter(pin);
    if (idx < 0) return 0;
    noInterrupts();
    uint32_t current = pulseCounters[idx].count;
    uint32_t delta = current - pulseCounters[idx].lastRead;
    pulseCounters[idx].lastRead = current;
    interrupts();
    return delta;
}

// Update pulse counter rates — called from loop(), non-blocking.
// Recalculates pulses/sec when the sample window has elapsed.
static void updatePulseRates() {
    unsigned long now = millis();
    for (int i = 0; i < pulseCounterCount; i++) {
        if (!pulseCounters[i].active) continue;
        if (now - pulseCounters[i].lastReadTime < pulseCounters[i].sampleWindowMs) continue;
        noInterrupts();
        uint32_t current = pulseCounters[i].count;
        uint32_t delta = current - pulseCounters[i].lastRead;
        pulseCounters[i].lastRead = current;
        interrupts();
        float dt = (now - pulseCounters[i].lastReadTime) / 1000.0f;
        pulseCounters[i].rate = (dt > 0.01f) ? (float)delta / dt : 0;
        pulseCounters[i].lastReadTime = now;
    }
}

// Get pulse rate for a pin (pulses per second)
static float getPulseRate(uint8_t pin) {
    int idx = findPulseCounter(pin);
    return (idx >= 0) ? pulseCounters[idx].rate : 0;
}

// IO expansion constants (full implementation defined later, after solar mode)
#define IO_EXPAND_MAX_PINS   16
#define IO_EXPAND_VPIN_BASE  100
int ioExpandRead(int vpin);  // forward declaration

// ─── Sensor read helper ─────────────────────────────────
// PULSE mode: returns rate × 100 (centipulses/sec) for integer precision.
// Use setpoint Scale to convert to engineering units (e.g., m/s).
// AUTO mode: ESP32-S3 has ADC on GPIOs 1-20; classic ESP32 has ADC1 on 32-39.
static int readSensorPin(int pin) {
    // Virtual pins 100-115: IO expansion board
    if (pin >= IO_EXPAND_VPIN_BASE && pin < IO_EXPAND_VPIN_BASE + IO_EXPAND_MAX_PINS)
        return ioExpandRead(pin);
    // Pulse counter mode
    for (int i = 0; i < sensorCount; i++) {
        if (sensorPins[i] == pin && sensorMode[i] == SENSOR_MODE_PULSE)
            return (int)(getPulseRate(pin) * 100.0f);  // centipulses/sec
    }
#ifdef CONFIG_IDF_TARGET_ESP32S3
    return analogRead(pin);
#else
    return (pin >= 32) ? analogRead(pin) : digitalRead(pin);
#endif
}

// ─── Timing ──────────────────────────────────────────────────
unsigned long nextHeartbeatTime = 0;
unsigned long lastRouteClean = 0;
unsigned long lastOledRefresh = 0;

// ─── Gateway Mode ───────────────────────────────────────────
bool gatewayMode = false;

// ─── BLE MSG ACK Retry (matches T-Deck's approach) ─────────
bool bleAckReceived = false;
String blePendingAckID = "";
int bleAckAttempt = 0;
unsigned long bleAckSentAt = 0;
String bleAckPayloadCache = "";
uint16_t bleAckDest = 0;

// ─── Pending SF Change (two-phase commit) ───────────────────
bool sfChangePending = false;
uint8_t sfChangeTarget = LORA_SF;
unsigned long sfChangeAt = 0;
#define EEPROM_SF_ADDR 431

// ─── Auto-Poll (periodic sensor reporting) ──────────────────
#define EEPROM_AUTOPOLL_ADDR 433  // 7 bytes: enabled(1) + target(4) + interval(2)
#define AUTOPOLL_MIN_INTERVAL 30  // seconds
bool autoPollEnabled = false;
char autoPollTarget[NODE_ID_LEN + 1] = "";
uint16_t autoPollInterval = 300;  // default 5 min
unsigned long nextAutoPollTime = 0;

// ─── GPS (optional external module via UART) ────────────────
#define EEPROM_GPS_ADDR 441     // 2 bytes: TX pin, RX pin (0xFF = disabled)
#define EEPROM_BLEPIN_ADDR 443  // 4 bytes: BLE PIN (uint32_t), 0xFFFFFFFF = default
#define BLE_DEFAULT_PIN 123456
uint32_t blePin = BLE_DEFAULT_PIN;
bool blePinIsDefault = true;    // true = setup mode, must change PIN before saving config
#define GPS_BROADCAST_INTERVAL 60000UL  // broadcast position every 60s
int8_t gpsTxPin = -1;
int8_t gpsRxPin = -1;
bool gpsEnabled = false;
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
unsigned long lastGpsBroadcast = 0;
void setupGPS();      // defined after forward declarations
void readGPS();
void broadcastGPS();

// ─── Relay Timers (ephemeral, not persisted) ────────────────
#define MAX_TIMERS 4
struct RelayTimer {
    bool active = false;
    uint8_t pin;
    uint8_t phase;        // 0=waiting first action, 1=ON phase, 2=OFF phase
    bool onAction;        // true=turn ON, false=turn OFF (for simple timers)
    bool isPulse;         // true=repeating pulse mode
    unsigned long nextAt; // millis() when next action fires
    uint16_t onSec;       // pulse ON duration (seconds)
    uint16_t offSec;      // pulse OFF duration (seconds)
    uint16_t repeats;     // 0=forever, else countdown
    uint16_t remaining;   // repeats remaining
};
RelayTimer timers[MAX_TIMERS];

// ─── On-Node Setpoints ──────────────────────────────────────
// Action types:
//   0 = RELAY  — send CMD,SET to targetNode (or local digitalWrite if target is self)
//   1 = MSG    — broadcast msgTrue when triggered, msgFalse when cleared
// Op codes: 0=GT, 1=LT, 2=EQ, 3=GE, 4=LE, 5=NE
#define MAX_SETPOINTS 4
#define SETPOINT_COOLDOWN 10000  // 10s between triggers
#define SETPOINT_MSG_LEN 32     // Max message length for MSG action
struct SetpointRule {
    bool active = false;
    uint8_t sensorPin;
    uint8_t op;              // 0=GT, 1=LT, 2=EQ, 3=GE, 4=LE, 5=NE
    float threshold;         // In scaled engineering units (after scale applied)
    float scaleFactor;       // raw * factor + offset = engineering units
    float scaleOffset;
    uint16_t debounceMs;     // Hold time before firing (0=immediate)
    char targetNode[NODE_ID_LEN + 1];
    uint8_t actionType;      // 0=RELAY, 1=MSG
    uint8_t relayPin;
    uint8_t action;          // RELAY: 0=LOW, 1=HIGH
    char msgTrue[SETPOINT_MSG_LEN + 1];
    char msgFalse[SETPOINT_MSG_LEN + 1];
    bool triggered;
    unsigned long lastFired;
    unsigned long debounceStart;  // Runtime: when condition first became true
};
SetpointRule setpoints[MAX_SETPOINTS];

// ─── Solar Mode ─────────────────────────────────────────────
// Deep low-power mode: OLED off, BLE deinited, ESP32 light-sleeps
// between radio DIO1 interrupts. Radio stays in continuous RX.
// Persisted to EEPROM so it survives power cycles.
bool solarMode = false;
bool solarOledTemporary = false;
unsigned long solarOledWakeUntil = 0;

// ─── IO Expansion Board (UART2) ─────────────────────────────
// Daisy-chain a second ESP32 for additional I/O.
// Virtual pins 100-115 map to expansion board sensors.
// Non-blocking: caches last response, polls periodically.
#define IO_EXPAND_BAUD       115200
#define IO_EXPAND_TIMEOUT    100     // ms max wait for response
#define IO_EXPAND_POLL_MS    2000    // poll interval
// IO_EXPAND_MAX_PINS and IO_EXPAND_VPIN_BASE defined above (before readSensorPin)

#if IO_EXPAND_TX >= 0
HardwareSerial IOSerial(2);
#endif
bool ioExpandEnabled = false;
int  ioExpandCache[IO_EXPAND_MAX_PINS];   // Cached sensor values
bool ioExpandCacheValid[IO_EXPAND_MAX_PINS];
unsigned long ioExpandLastPoll = 0;
String ioExpandRxBuf;

// Non-blocking poll: send query and parse any available response
void ioExpandPoll() {
#if IO_EXPAND_TX >= 0
    if (!ioExpandEnabled) return;
    unsigned long now = millis();

    // Read any available response bytes (non-blocking)
    while (IOSerial.available()) {
        char c = IOSerial.read();
        if (c == '\n') {
            ioExpandRxBuf.trim();
            if (ioExpandRxBuf.startsWith("S,")) {
                // Parse: S,<pin>:<val>,<pin>:<val>,...
                String data = ioExpandRxBuf.substring(2);
                while (data.length() > 0) {
                    int colon = data.indexOf(':');
                    if (colon < 0) break;
                    int comma = data.indexOf(',');
                    int pin = data.substring(0, colon).toInt();
                    String valStr = (comma > 0) ? data.substring(colon + 1, comma)
                                                : data.substring(colon + 1);
                    if (pin >= 0 && pin < IO_EXPAND_MAX_PINS) {
                        ioExpandCache[pin] = valStr.toInt();
                        ioExpandCacheValid[pin] = true;
                    }
                    if (comma < 0) break;
                    data = data.substring(comma + 1);
                }
            }
            ioExpandRxBuf = "";
        } else {
            ioExpandRxBuf += c;
        }
    }

    // Send poll query periodically
    if (now - ioExpandLastPoll >= IO_EXPAND_POLL_MS) {
        IOSerial.println("?S");
        ioExpandLastPoll = now;
    }
#endif
}

// Read a virtual pin (100-115) from expansion board cache
int ioExpandRead(int vpin) {
    int idx = vpin - IO_EXPAND_VPIN_BASE;
    if (idx < 0 || idx >= IO_EXPAND_MAX_PINS) return 0;
    return ioExpandCacheValid[idx] ? ioExpandCache[idx] : 0;
}

// Send a relay command to expansion board
void ioExpandSetRelay(int pin, int val) {
#if IO_EXPAND_TX >= 0
    if (!ioExpandEnabled) return;
    IOSerial.println("!R," + String(pin) + "," + String(val));
#endif
}

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

// ─── GPS function implementations ────────────────────────────
void setupGPS() {
    if (gpsTxPin >= 0 && gpsRxPin >= 0) {
        GPSSerial.begin(9600, SERIAL_8N1, gpsRxPin, gpsTxPin);
        gpsEnabled = true;
        debugPrint("GPS: enabled on TX=" + String(gpsTxPin) + " RX=" + String(gpsRxPin));
    }
}

void readGPS() {
    if (!gpsEnabled) return;
    while (GPSSerial.available()) {
        gps.encode(GPSSerial.read());
    }
}

void broadcastGPS() {
    if (!gpsEnabled || !gps.location.isValid()) return;
    unsigned long now = millis();
    if (now - lastGpsBroadcast < GPS_BROADCAST_INTERVAL) return;
    lastGpsBroadcast = now;

    String pos = "POS," + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
    String mid = mesh.generateMsgID();
    String hex = mesh.encryptMsg(pos);
    String payload = String(mesh.localID) + ",FFFF," + mid + "," +
                     String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
    mesh.transmitPacket(0xFFFF, payload);
    debugPrint("GPS: broadcast " + pos);
    bleSend("GPS," + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6));
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Radio bridge — connects MeshCore to physical radio
// ═══════════════════════════════════════════════════════════════

void receiveCheck();  // Forward declaration — needed by radioTransmit()

void IRAM_ATTR onRadioRx() { mesh.rxFlag = true; }

// Start listening — always continuous RX (no packet loss)
void radioStartListening() {
    radio.startReceive();
}

// MeshCore calls this to transmit raw packets
void radioTransmit(uint8_t* pkt, size_t len) {
    // Rescue any pending RX before we clobber the SX1262 buffer
    // Without this, a packet received between receiveCheck() and this TX
    // would be permanently lost (radio.standby() discards the RX buffer)
    if (mesh.rxFlag) {
        receiveCheck();
    }
    radio.standby();
    radio.transmit(pkt, len);
    mesh.rxFlag = false;  // Clear false RX flag from TX_DONE DIO1 interrupt
    radioStartListening();
}

// MeshCore calls this to check if channel is free
bool radioChannelFree() {
    // Use RSSI scan — if below noise floor, channel is free
    float rssi = radio.getRSSI();
    return rssi < -120.0;
}

#ifndef RADIO_TCXO_VOLTAGE
#define RADIO_TCXO_VOLTAGE 0
#endif

void setupRadio() {
    int state = RADIOLIB_ERR_UNKNOWN;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            debugPrint("Radio retry " + String(attempt + 1) + "/3...");
            delay(500);
        }
#if defined(RADIO_SX1262)
        debugPrint("Initializing SX1262...");
        state = radio.begin(LORA_FREQ, LORA_BW, mesh.currentSF, LORA_CR,
                            LORA_SYNC, LORA_POWER, LORA_PREAMBLE, RADIO_TCXO_VOLTAGE, false);
#elif defined(RADIO_SX1276)
        debugPrint("Initializing SX1276...");
        state = radio.begin(LORA_FREQ, LORA_BW, mesh.currentSF, LORA_CR,
                            LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 0);
#endif
        if (state == RADIOLIB_ERR_NONE) break;
    }
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

        // Gateway mode: forward raw packet + RSSI over serial for bridge server
        if (gatewayMode) {
            Serial.println("PKT," + mesh.toHex(pkt, len) + "," + String(mesh.lastRSSI));
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

    // 1. Disable OLED via Vext — saves ~15mA
#ifdef VEXT_CTRL
    digitalWrite(VEXT_CTRL, HIGH);  // Vext OFF
#endif
    oledAvailable = false;

    // BLE stays active — needed for app control and solar mode toggle
    // Radio stays in continuous RX

    // 2. Configure light sleep with DIO1 wakeup
    //    Radio stays in continuous RX. DIO1 fires on packet → wakes CPU.
    //    Between events, CPU draws ~2-3mA instead of ~80mA (higher than
    //    full deep sleep because BLE is still active, but much less than normal).
    gpio_wakeup_enable((gpio_num_t)RADIO_DIO1, GPIO_INTR_HIGH_LEVEL);
#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    gpio_wakeup_enable((gpio_num_t)BOARD_BUTTON, GPIO_INTR_LOW_LEVEL);
#endif
    esp_sleep_enable_gpio_wakeup();
    // Wake on timer for heartbeats (60s)
    esp_sleep_enable_timer_wakeup(HB_INTERVAL_SOLAR * 1000ULL);  // microseconds
    // Wake on UART input so serial commands (SOLAR OFF) work during sleep
    uart_set_wakeup_threshold(UART_NUM_0, 3);  // wake after 3 edges on RX
    esp_sleep_enable_uart_wakeup(UART_NUM_0);

    debugPrint("SOLAR: OLED off, BLE active, light sleep armed");
    debugPrint("SOLAR: Toggle off via app, PRG button, or SOLAR OFF via serial");
    Serial.flush();
}

void solarLightSleep() {
    // BLE runs on Core 0. esp_light_sleep_start() halts BOTH cores,
    // which crashes the Bluetooth controller (Guru Meditation: interrupt WDT).
    // Use delay() instead — still saves power vs busy-loop while keeping BLE alive.
    delay(100);
}

void stopSolarMode() {
    solarMode = false;
    debugPrint("SOLAR: Restoring normal operation");

    // Disable light sleep wakeup sources
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

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
    debugPrint("SOLAR: OLED restored — full operation resumed");
    // BLE was never deinited, so no need to reinit
}

void solarCheckButton() {
#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    if (!digitalRead(BOARD_BUTTON) && !solarOledTemporary) {
        // Button pressed — temporarily show OLED status
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

    // Turn off temporary OLED after timeout
    if (solarOledTemporary && millis() > solarOledWakeUntil) {
        solarOledTemporary = false;
        oledAvailable = false;
#ifdef VEXT_CTRL
        digitalWrite(VEXT_CTRL, HIGH);  // Vext OFF
#endif
    }
#endif
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
        0,  // boot
        19, 20  // USB D-/D+ on ESP32-S3 — touching these kills native USB
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

// ═══════════════════════════════════════════════════════════════
// SECTION: Timer / Setpoint / Auto-Poll Handlers
// ═══════════════════════════════════════════════════════════════

// Core timer logic — returns response string, used by both mesh and BLE paths
String processTimerCommand(const String& args) {
    if (args == "CLEAR") {
        for (int i = 0; i < MAX_TIMERS; i++) timers[i].active = false;
        return "OK,TIMER,CLEAR";
    }
    if (args == "LIST") {
        String resp = "TIMERS";
        int count = 0;
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (!timers[i].active) continue;
            count++;
            resp += "," + String(timers[i].pin) + ":";
            if (timers[i].isPulse) {
                resp += "PULSE:" + String(timers[i].onSec) + "/" + String(timers[i].offSec);
                if (timers[i].repeats > 0) resp += "x" + String(timers[i].remaining);
            } else {
                resp += (timers[i].onAction ? "ON" : "OFF");
                unsigned long remain = (timers[i].nextAt > millis()) ? (timers[i].nextAt - millis()) / 1000 : 0;
                resp += ":" + String((unsigned long)remain) + "s";
            }
        }
        if (count == 0) resp += ",NONE";
        return resp;
    }

    // Parse: <pin>,ON|OFF,<seconds> or <pin>,PULSE,<onSec>,<offSec>,<repeats>
    int c1 = args.indexOf(',');
    if (c1 < 0) return "ERR,TIMER,FORMAT";
    int pin = args.substring(0, c1).toInt();
    if (!isPinSafe(pin)) return "ERR,TIMER,BADPIN";

    String rest = args.substring(c1 + 1);
    int c2 = rest.indexOf(',');
    String action = (c2 > 0) ? rest.substring(0, c2) : rest;
    action.toUpperCase();

    // Find existing slot for same pin, or allocate free slot
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].pin == pin) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (!timers[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) return "ERR,TIMER,FULL";

    if (action == "ON" || action == "OFF") {
        if (c2 < 0) return "ERR,TIMER,FORMAT";
        int seconds = rest.substring(c2 + 1).toInt();
        if (seconds < 1 || seconds > 86400) return "ERR,TIMER,RANGE";
        pinMode(pin, OUTPUT);
        timers[slot].active = true;
        timers[slot].pin = pin;
        timers[slot].isPulse = false;
        timers[slot].onAction = (action == "ON");
        timers[slot].nextAt = millis() + (unsigned long)seconds * 1000UL;
        timers[slot].phase = 0;
        return "OK,TIMER," + String(pin) + "," + action + "," + String(seconds);
    }
    else if (action == "PULSE") {
        // PULSE,<onSec>,<offSec>,<repeats>
        if (c2 < 0) return "ERR,TIMER,FORMAT";
        String pulseArgs = rest.substring(c2 + 1);
        int p1 = pulseArgs.indexOf(',');
        int p2 = pulseArgs.indexOf(',', p1 + 1);
        if (p1 < 0 || p2 < 0) return "ERR,TIMER,FORMAT";
        int onSec = pulseArgs.substring(0, p1).toInt();
        int offSec = pulseArgs.substring(p1 + 1, p2).toInt();
        int repeats = pulseArgs.substring(p2 + 1).toInt();
        if (onSec < 1 || offSec < 1 || onSec > 3600 || offSec > 3600) return "ERR,TIMER,RANGE";
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);  // Start ON immediately
        timers[slot].active = true;
        timers[slot].pin = pin;
        timers[slot].isPulse = true;
        timers[slot].phase = 1;  // Currently in ON phase
        timers[slot].onSec = onSec;
        timers[slot].offSec = offSec;
        timers[slot].repeats = repeats;
        timers[slot].remaining = (repeats > 0) ? repeats : 0;
        timers[slot].nextAt = millis() + (unsigned long)onSec * 1000UL;
        return "OK,TIMER," + String(pin) + ",PULSE," + String(onSec) + "," + String(offSec) + "," + String(repeats);
    }

    return "ERR,TIMER,BADACTION";
}

// Handle TIMER command received over mesh (CMD,TIMER,...)
void handleTimerCmd(const String& args, const String& from) {
    String resp = processTimerCommand(args);
    mesh.cmdsExecuted++;
    // Send response back over mesh to requester
    String mid = mesh.generateMsgID();
    String hex = mesh.encryptMsg(resp);
    String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                     String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
    mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
    bleSend(resp);
}

// Handle TIMER command from BLE
void handleTimerBle(const String& args) {
    String resp = processTimerCommand(args);
    bleSend(resp);
}

// Core setpoint logic — returns response string
String processSetpointCommand(const String& args) {
    if (args == "CLEAR") {
        for (int i = 0; i < MAX_SETPOINTS; i++) setpoints[i].active = false;
        return "OK,SETPOINT,CLEAR";
    }
    if (args == "LIST") {
        String resp = "SETPOINTS";
        int count = 0;
        const char* ops[] = {"GT", "LT", "EQ", "GE", "LE", "NE"};
        for (int i = 0; i < MAX_SETPOINTS; i++) {
            if (!setpoints[i].active) continue;
            count++;
            resp += "," + String(setpoints[i].sensorPin) + ":" +
                    String(ops[setpoints[i].op]) + ":" +
                    String(setpoints[i].threshold, 1) + "->";
            if (setpoints[i].actionType == 1) {
                resp += "MSG:" + String(setpoints[i].msgTrue);
                if (setpoints[i].msgFalse[0]) resp += "/" + String(setpoints[i].msgFalse);
            } else {
                resp += String(setpoints[i].targetNode) + ":" +
                        String(setpoints[i].relayPin) + ":" +
                        String(setpoints[i].action);
            }
            if (setpoints[i].scaleFactor != 1.0f || setpoints[i].scaleOffset != 0.0f)
                resp += "(x" + String(setpoints[i].scaleFactor, 3) + "+" + String(setpoints[i].scaleOffset, 1) + ")";
            if (setpoints[i].debounceMs > 0)
                resp += "[" + String(setpoints[i].debounceMs) + "ms]";
        }
        if (count == 0) resp += ",NONE";
        return resp;
    }

    // Formats (with optional SCALE/DEBOUNCE suffixes):
    //   RELAY: <pin>,<op>,<threshold>,<target>,<relayPin>,<action>[,SCALE,<f>,<o>][,DEBOUNCE,<ms>]
    //   MSG:   <pin>,<op>,<threshold>,MSG,<msgTrue>[,<msgFalse>][,SCALE,<f>,<o>][,DEBOUNCE,<ms>]
    int c[3];
    c[0] = args.indexOf(',');
    c[1] = (c[0] > 0) ? args.indexOf(',', c[0] + 1) : -1;
    c[2] = (c[1] > 0) ? args.indexOf(',', c[1] + 1) : -1;
    if (c[2] < 0) return "ERR,SETPOINT,FORMAT";

    uint8_t sensorPin = args.substring(0, c[0]).toInt();
    String opStr = args.substring(c[0] + 1, c[1]); opStr.toUpperCase();
    float threshold = args.substring(c[1] + 1, c[2]).toFloat();
    String rest = args.substring(c[2] + 1);

    if (!isPinSafe(sensorPin)) return "ERR,SETPOINT,BADSENSOR";

    uint8_t op = 255;
    if (opStr == "GT") op = 0;
    else if (opStr == "LT") op = 1;
    else if (opStr == "EQ") op = 2;
    else if (opStr == "GE") op = 3;
    else if (opStr == "LE") op = 4;
    else if (opStr == "NE") op = 5;
    if (op > 5) return "ERR,SETPOINT,BADOP";

    // Extract optional SCALE and DEBOUNCE suffixes before parsing action
    float scaleFactor = 1.0f, scaleOffset = 0.0f;
    uint16_t debounceMs = 0;
    int scalePos = rest.indexOf(",SCALE,");
    int debouncePos = rest.indexOf(",DEBOUNCE,");
    String actionPart = rest;  // Will be trimmed below

    if (scalePos >= 0 || debouncePos >= 0) {
        // Find where suffixes start (whichever comes first)
        int suffixStart = (scalePos >= 0 && debouncePos >= 0) ? min(scalePos, debouncePos)
                        : (scalePos >= 0) ? scalePos : debouncePos;
        actionPart = rest.substring(0, suffixStart);
        String suffixes = rest.substring(suffixStart);

        if (scalePos >= 0) {
            int sIdx = suffixes.indexOf("SCALE,") + 6;
            int sComma = suffixes.indexOf(',', sIdx);
            if (sComma > sIdx) {
                scaleFactor = suffixes.substring(sIdx, sComma).toFloat();
                int sEnd = suffixes.indexOf(',', sComma + 1);
                scaleOffset = (sEnd > sComma) ? suffixes.substring(sComma + 1, sEnd).toFloat()
                                              : suffixes.substring(sComma + 1).toFloat();
            }
        }
        if (debouncePos >= 0) {
            int dIdx = suffixes.indexOf("DEBOUNCE,") + 9;
            debounceMs = suffixes.substring(dIdx).toInt();
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_SETPOINTS; i++) {
        if (!setpoints[i].active) { slot = i; break; }
    }
    if (slot < 0) return "ERR,SETPOINT,FULL";

    memset(&setpoints[slot], 0, sizeof(SetpointRule));
    setpoints[slot].active = true;
    setpoints[slot].sensorPin = sensorPin;
    setpoints[slot].op = op;
    setpoints[slot].threshold = threshold;
    setpoints[slot].scaleFactor = scaleFactor;
    setpoints[slot].scaleOffset = scaleOffset;
    setpoints[slot].debounceMs = debounceMs;

    const char* ops[] = {"GT", "LT", "EQ", "GE", "LE", "NE"};
    String resp = "OK,SETPOINT," + String(sensorPin) + "," + String(ops[op]) + "," + String(threshold, 2);

    if (actionPart.startsWith("MSG,")) {
        String msgPart = actionPart.substring(4);
        int comma = msgPart.indexOf(',');
        String msgTrue = (comma > 0) ? msgPart.substring(0, comma) : msgPart;
        String msgFalse = (comma > 0) ? msgPart.substring(comma + 1) : "";
        msgTrue.trim(); msgFalse.trim();
        if (msgTrue.length() == 0) return "ERR,SETPOINT,EMPTYMSG";
        if (msgTrue.length() > SETPOINT_MSG_LEN) return "ERR,SETPOINT,MSGTOOLONG";

        setpoints[slot].actionType = 1;
        strncpy(setpoints[slot].msgTrue, msgTrue.c_str(), SETPOINT_MSG_LEN);
        strncpy(setpoints[slot].msgFalse, msgFalse.c_str(), SETPOINT_MSG_LEN);
        strncpy(setpoints[slot].targetNode, mesh.localID, NODE_ID_LEN);
        resp += ",MSG," + msgTrue;
        if (msgFalse.length() > 0) resp += "," + msgFalse;
    } else {
        int rc1 = actionPart.indexOf(',');
        int rc2 = (rc1 > 0) ? actionPart.indexOf(',', rc1 + 1) : -1;
        if (rc2 < 0) return "ERR,SETPOINT,FORMAT";

        String targetNode = actionPart.substring(0, rc1); targetNode.trim(); targetNode.toUpperCase();
        uint8_t relayPin = actionPart.substring(rc1 + 1, rc2).toInt();
        uint8_t action = actionPart.substring(rc2 + 1).toInt();

        if (!mesh.isValidNodeID(targetNode)) return "ERR,SETPOINT,BADTARGET";

        setpoints[slot].actionType = 0;
        strncpy(setpoints[slot].targetNode, targetNode.c_str(), NODE_ID_LEN);
        setpoints[slot].targetNode[NODE_ID_LEN] = '\0';
        setpoints[slot].relayPin = relayPin;
        setpoints[slot].action = action;
        resp += "," + targetNode + "," + String(relayPin) + "," + String(action);
    }

    if (scaleFactor != 1.0f || scaleOffset != 0.0f)
        resp += ",SCALE," + String(scaleFactor, 4) + "," + String(scaleOffset, 4);
    if (debounceMs > 0)
        resp += ",DEBOUNCE," + String(debounceMs);
    return resp;
}

// Handle SETPOINT command received over mesh (CMD,SETPOINT,...)
void handleSetpointCmd(const String& args, const String& from) {
    String resp = processSetpointCommand(args);
    mesh.cmdsExecuted++;
    String mid = mesh.generateMsgID();
    String hex = mesh.encryptMsg(resp);
    String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                     String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
    mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
    bleSend(resp);
}

// Handle SETPOINT command from BLE
void handleSetpointBle(const String& args) {
    String resp = processSetpointCommand(args);
    bleSend(resp);
}

// Process active relay timers — called from loop()
void processTimers() {
    unsigned long now = millis();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active || now < timers[i].nextAt) continue;

        if (!timers[i].isPulse) {
            // Simple delay timer — fire once and deactivate
            digitalWrite(timers[i].pin, timers[i].onAction ? HIGH : LOW);
            debugPrint("TIMER: pin " + String(timers[i].pin) + " -> " + String(timers[i].onAction ? "ON" : "OFF"));
            bleSend("TIMER,FIRED," + String(timers[i].pin) + "," + String(timers[i].onAction ? "ON" : "OFF"));
            timers[i].active = false;
        } else {
            // Pulse timer — alternate ON/OFF phases
            if (timers[i].phase == 1) {
                // Was ON, switch to OFF
                digitalWrite(timers[i].pin, LOW);
                timers[i].phase = 2;
                timers[i].nextAt = now + (unsigned long)timers[i].offSec * 1000UL;
            } else {
                // Was OFF, check repeats then switch to ON
                if (timers[i].repeats > 0) {
                    timers[i].remaining--;
                    if (timers[i].remaining == 0) {
                        digitalWrite(timers[i].pin, LOW);
                        debugPrint("TIMER: pin " + String(timers[i].pin) + " pulse DONE");
                        bleSend("TIMER,DONE," + String(timers[i].pin));
                        timers[i].active = false;
                        continue;
                    }
                }
                digitalWrite(timers[i].pin, HIGH);
                timers[i].phase = 1;
                timers[i].nextAt = now + (unsigned long)timers[i].onSec * 1000UL;
            }
        }
    }
}

// Fire a setpoint action — relay (local or remote) or message
// Returns true if a LoRa TX happened (caller should yield to avoid back-to-back)
bool fireSetpointAction(SetpointRule& sp, int value, bool triggered) {
    String target = String(sp.targetNode);
    bool isLocal = (target == String(mesh.localID));

    if (sp.actionType == 0) {
        // RELAY action
        if (!triggered) return false;  // Relay only fires on rising edge

        if (isLocal) {
            // Local relay — no radio needed, just set the pin directly
            digitalWrite(sp.relayPin, sp.action ? HIGH : LOW);
            debugPrint("SETPOINT LOCAL: pin " + String(sp.sensorPin) + "=" + String(value) +
                       " -> pin " + String(sp.relayPin) + "=" + String(sp.action));
            bleSend("SETPOINT,FIRED," + String(sp.sensorPin) + "," + String(value) + ",LOCAL");
            return false;  // No radio TX
        } else {
            // Remote relay — send over mesh
            String cmdText = "CMD,SET," + String(sp.relayPin) + "," + String(sp.action);
            uint16_t dest = strtol(target.c_str(), nullptr, 16);
            String mid = mesh.generateMsgID();
            String hex = mesh.encryptMsg(cmdText);
            String payload = String(mesh.localID) + "," + target + "," + mid + "," +
                             String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
            mesh.transmitPacket(dest, payload);
            debugPrint("SETPOINT: pin " + String(sp.sensorPin) + "=" + String(value) +
                       " -> " + target + " pin " + String(sp.relayPin) + "=" + String(sp.action));
            bleSend("SETPOINT,FIRED," + String(sp.sensorPin) + "," + String(value) + "," + target);
            return true;  // Radio TX happened
        }
    }
    else if (sp.actionType == 1) {
        // MSG action — broadcast message on state change (both edges)
        const char* msg = triggered ? sp.msgTrue : sp.msgFalse;
        if (msg[0] == '\0') return false;  // No message configured for this edge

        String msgText = "MSG," + String(msg);
        String mid = mesh.generateMsgID();
        String hex = mesh.encryptMsg(msgText);
        String payload = String(mesh.localID) + ",FFFF," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(0xFFFF, payload);
        debugPrint("SETPOINT MSG: pin " + String(sp.sensorPin) + "=" + String(value) +
                   " -> broadcast '" + String(msg) + "'");
        bleSend("SETPOINT,MSG," + String(sp.sensorPin) + "," + String(value) + "," + String(msg));
        return true;  // Radio TX happened
    }
    return false;
}

// Evaluate setpoint condition: apply scale then compare
bool evalSetpointCondition(SetpointRule& sp, int rawValue, float& scaled) {
    scaled = (float)rawValue * sp.scaleFactor + sp.scaleOffset;
    switch (sp.op) {
        case 0: return scaled >  sp.threshold;  // GT
        case 1: return scaled <  sp.threshold;  // LT
        case 2: return fabsf(scaled - sp.threshold) < 0.001f;  // EQ (float-safe)
        case 3: return scaled >= sp.threshold;  // GE
        case 4: return scaled <= sp.threshold;  // LE
        case 5: return fabsf(scaled - sp.threshold) >= 0.001f; // NE
    }
    return false;
}

// Check setpoint rules against current sensor values — called from loop()
// Only fires ONE radio TX per call to avoid back-to-back LoRa TX.
// Local relay actions (no radio) all run immediately.
void checkSetpoints() {
    unsigned long now = millis();
    for (int i = 0; i < MAX_SETPOINTS; i++) {
        if (!setpoints[i].active) continue;
        if (now - setpoints[i].lastFired < SETPOINT_COOLDOWN) continue;

        int rawValue = readSensorPin(setpoints[i].sensorPin);
        float scaled;
        bool condition = evalSetpointCondition(setpoints[i], rawValue, scaled);

        if (condition && !setpoints[i].triggered) {
            // Debounce: condition must hold continuously for debounceMs
            if (setpoints[i].debounceMs > 0) {
                if (setpoints[i].debounceStart == 0) {
                    setpoints[i].debounceStart = now;  // Start timer
                }
                if (now - setpoints[i].debounceStart < setpoints[i].debounceMs) {
                    continue;  // Not held long enough yet
                }
            }
            // Rising edge — condition confirmed (debounce passed or no debounce)
            setpoints[i].triggered = true;
            setpoints[i].lastFired = now;
            setpoints[i].debounceStart = 0;
            if (fireSetpointAction(setpoints[i], rawValue, true))
                return;  // Radio TX happened — yield, check remaining next loop
        }
        else if (!condition) {
            setpoints[i].debounceStart = 0;  // Reset debounce timer
            if (setpoints[i].triggered) {
                // Falling edge — hysteresis check on scaled value
                float margin = fabsf(setpoints[i].threshold) * 0.1f;
                if (margin < 0.1f) margin = 0.1f;
                bool clearCondition = false;
                switch (setpoints[i].op) {
                    case 0: case 3: clearCondition = (scaled < (setpoints[i].threshold - margin)); break;
                    case 1: case 4: clearCondition = (scaled > (setpoints[i].threshold + margin)); break;
                    case 2: clearCondition = (fabsf(scaled - setpoints[i].threshold) >= 0.001f); break;
                    case 5: clearCondition = (fabsf(scaled - setpoints[i].threshold) < 0.001f); break;
                    default: clearCondition = true;
                }
                if (clearCondition) {
                    setpoints[i].triggered = false;
                    setpoints[i].lastFired = now;
                    if (setpoints[i].actionType == 1) {
                        if (fireSetpointAction(setpoints[i], rawValue, false))
                            return;
                    }
                }
            }
        }
    }
}

// Execute auto-poll — read all sensors and send SDATA to target
void executeAutoPoll() {
    if (!autoPollEnabled || sensorCount == 0) return;
    unsigned long now = millis();
    if (now < nextAutoPollTime) return;

    // Schedule next poll with ±2s jitter to avoid synchronized collisions
    nextAutoPollTime = now + (unsigned long)autoPollInterval * 1000UL + random(-2000, 2000);

    String sdata = "SDATA," + String(mesh.localID);
    for (int i = 0; i < sensorCount; i++) {
        int pin = sensorPins[i];
        int value = readSensorPin(pin);
        sdata += "," + String(pin) + ":" + String(value);
    }

    // Send to target node over mesh
    uint16_t dest = strtol(autoPollTarget, nullptr, 16);
    String mid = mesh.generateMsgID();
    String hex = mesh.encryptMsg(sdata);
    String payload = String(mesh.localID) + "," + String(autoPollTarget) + "," + mid + "," +
                     String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
    mesh.transmitPacket(dest, payload);

    debugPrint("AUTOPOLL: sent SDATA to " + String(autoPollTarget));
    bleSend(sdata);  // Also notify local BLE client
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
        String hex = mesh.encryptMsg(rsp);
        String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
        bleSend("CMD,RSP," + String(pin) + "," + String(val));
    }
    else if (action == "GET") {
        int pin = rest.toInt();
        if (!isPinSafe(pin)) return;
        int value = readSensorPin(pin);
        mesh.cmdsExecuted++;
        String mid = mesh.generateMsgID();
        String rsp = "CMD,RSP," + String(pin) + "," + String(value);
        String hex = mesh.encryptMsg(rsp);
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
        String hex = mesh.encryptMsg(rsp);
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
    else if (action == "REBOOT") {
        // Remote reboot via mesh — no EEPROM wipe, just restart
        mesh.cmdsExecuted++;
        String mid = mesh.generateMsgID();
        String rsp = "CMD,RSP,REBOOTING";
        String hex = mesh.encryptMsg(rsp);
        String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
        delay(200);
        ESP.restart();
    }
    else if (action == "POLL") {
        // Read all sensor pins and return bundled SDATA response
        String sdata = "SDATA," + String(mesh.localID);
        for (int i = 0; i < sensorCount; i++) {
            int pin = sensorPins[i];
            int value = readSensorPin(pin);
            sdata += "," + String(pin) + ":" + String(value);
        }
        mesh.cmdsExecuted++;
        String mid = mesh.generateMsgID();
        String hex = mesh.encryptMsg(sdata);
        String payload = String(mesh.localID) + "," + from + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(strtol(from.c_str(), nullptr, 16), payload);
        bleSend(sdata);
    }
    else if (action == "TIMER") {
        // TIMER,<pin>,ON,<seconds> | TIMER,<pin>,OFF,<seconds>
        // TIMER,<pin>,PULSE,<onSec>,<offSec>,<repeats> | TIMER,CLEAR | TIMER,LIST
        handleTimerCmd(rest, from);
    }
    else if (action == "SETPOINT") {
        // SETPOINT,<sensorPin>,<op>,<threshold>,<targetNode>,<relayPin>,<action>
        // SETPOINT,CLEAR | SETPOINT,LIST
        handleSetpointCmd(rest, from);
    }
}

// Message handler — called by MeshCore when a message arrives for us
void handleMessage(const String& from, const String& text, int rssi) {
    String msg = text.startsWith("MSG,") ? text.substring(4) : text;
    debugPrint("MSG from " + from + ": " + msg);
    bleSend("RX," + from + "," + msg + "," + String(rssi));
}

// ACK handler — called by MeshCore when an ACK arrives for us
void handleAck(const String& from, const String& mid) {
    debugPrint("ACK from " + from + " mid=" + mid);
    bleSend("ACK," + from + "," + mid);
    // Check if this ACK is for a pending BLE MSG send
    if (mid == blePendingAckID) {
        bleAckReceived = true;
    }
}

// BLE MSG ACK retry — called from loop(), matches T-Deck's handleAckRetry()
void handleBleAckRetry() {
    if (blePendingAckID.length() == 0) return;
    if (bleAckReceived) {
        debugPrint("BLE MSG delivered!");
        bleSend("DELIVERED," + blePendingAckID);
        blePendingAckID = "";
        return;
    }
    if (millis() - bleAckSentAt < ACK_TIMEOUT) return;

    bleAckAttempt++;
    if (bleAckAttempt > MAX_RETRIES) {
        debugPrint("BLE MSG send failed - no ACK");
        bleSend("FAILED," + blePendingAckID);
        blePendingAckID = "";
        return;
    }
    debugPrint("BLE MSG retry " + String(bleAckAttempt) + "/" + String(MAX_RETRIES));
    mesh.transmitPacket(bleAckDest, bleAckPayloadCache);
    bleAckSentAt = millis();
}

// CFG handler — a config change request arrived over the mesh
void handleCfg(const String& cfgType, const String& value, const String& changeId, const String& from) {
    debugPrint("CFG: " + cfgType + "=" + value + " from=" + from);
    bleSend("CFG," + cfgType + "," + value + "," + changeId + "," + from);
}

// CFGACK handler — a node acknowledged the config change
void handleCfgAck(const String& cfgType, const String& value, const String& changeId, const String& nodeId) {
    debugPrint("CFGACK: " + cfgType + "=" + value + " node=" + nodeId);
    bleSend("CFGACK," + cfgType + "," + value + "," + changeId + "," + nodeId);
}

// CFGGO handler — commit the config change after delay
void handleCfgGo(const String& cfgType, const String& value, const String& changeId) {
    debugPrint("CFGGO: " + cfgType + "=" + value);
    bleSend("CFGGO," + cfgType + "," + value + "," + changeId);

    if (cfgType == "SF") {
        int newSF = value.toInt();
        if (newSF >= 7 && newSF <= 12) {
            sfChangePending = true;
            sfChangeTarget = newSF;
            sfChangeAt = millis() + CFG_SWITCH_DELAY;
            debugPrint("SF change scheduled: SF" + String(newSF) + " in " + String(CFG_SWITCH_DELAY) + "ms");
        }
    }
}

// Node discovery handler
void handleNodeDiscovered(const String& id, int rssi) {
    debugPrint("New node: " + id);
    // Direct discovery: 1 hop, 0 age, active, nextHop is the node itself
    bleSend("NODE," + id + "," + String(rssi) + ",1,0,1," + id);
}

// ID conflict handler — another node is using our ID
void handleIdConflict(const String& id, int rssi) {
    debugPrint("!! ID CONFLICT: " + id + " RSSI=" + String(rssi));
    if (oledAvailable) {
        oled.clearDisplay();
        oled.setTextSize(2); oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);  oled.println("!! ID");
        oled.println("CONFLICT !!");
        oled.setTextSize(1);
        oled.print("ID "); oled.print(id);
        oled.print(" RSSI:"); oled.println(rssi);
        oled.println("Change via app/serial");
        oled.display();
    }
    bleSend("CONFLICT," + id + "," + String(rssi));
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

// Queue for BLE commands — processed in loop() to avoid BTC_TASK stack overflow
#define BLE_CMD_QUEUE_SIZE 4
String bleCmdQueue[BLE_CMD_QUEUE_SIZE];
volatile uint8_t bleCmdHead = 0;
volatile uint8_t bleCmdTail = 0;

class BleTxCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        String data = c->getValue().c_str();
        bleRxBuffer += data;
        while (bleRxBuffer.indexOf('\n') >= 0) {
            int nl = bleRxBuffer.indexOf('\n');
            String line = bleRxBuffer.substring(0, nl); line.trim();
            bleRxBuffer = bleRxBuffer.substring(nl + 1);
            if (line.length() > 0) {
                // Queue command for processing in loop() — BTC_TASK stack is too small
                uint8_t nextHead = (bleCmdHead + 1) % BLE_CMD_QUEUE_SIZE;
                if (nextHead != bleCmdTail) {
                    bleCmdQueue[bleCmdHead] = line;
                    bleCmdHead = nextHead;
                }
            }
        }
    }
};

// Process queued BLE commands from main loop (safe stack)
void processBleQueue() {
    while (bleCmdTail != bleCmdHead) {
        String cmd = bleCmdQueue[bleCmdTail];
        bleCmdTail = (bleCmdTail + 1) % BLE_CMD_QUEUE_SIZE;
        processBleCommand(cmd);
    }
}

void setupBLE() {
    String bleName = "TOM-" + String(mesh.localID);
    BLEDevice::init(bleName.c_str());

    // Enable BLE security with static PIN
    BLESecurity* bleSecurity = new BLESecurity();
    bleSecurity->setStaticPIN(blePin);

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
    debugPrint("BLE ready: " + bleName + " (PIN protected)");
}

void bleSend(const String& line) {
    // Echo BLE responses to serial when in gateway mode (so gateway GUI sees feedback)
    if (gatewayMode) Serial.println(line);
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

    // Setup mode: only allow BLEPIN, STATUS, REBOOT, and EEPROM,RESET until PIN is changed
    if (blePinIsDefault &&
        !line.startsWith("BLEPIN,") && line != "STATUS" &&
        line != "REBOOT" && line != "EEPROM,RESET") {
        bleSend("ERR,SETUP_MODE,SET_PIN_FIRST");
        bleSend("Send BLEPIN,<6-digit-pin> to set your PIN before using other commands.");
        return;
    }

    if (line.startsWith("MSG,")) {
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        if (c2 < 0) return;
        String target = line.substring(c1 + 1, c2);
        String text = line.substring(c2 + 1);
        uint16_t dest = strtol(target.c_str(), nullptr, 16);
        String mid = mesh.generateMsgID();
        // POS and SOS are sent without MSG prefix so receivers handle them as position/emergency
        String encText = (text.startsWith("POS,") || text.startsWith("SOS,")) ? text : "MSG," + text;
        String payload = String(mesh.localID) + "," + target + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," +
                         mesh.encryptMsg(encText);
        mesh.transmitPacket(dest, payload);
        bleSend("SENT," + target + "," + mid);

        // ACK tracking for directed messages (not broadcasts)
        if (target != "FFFF") {
            bleAckReceived = false;
            blePendingAckID = mid;
            bleAckAttempt = 0;
            bleAckSentAt = millis();
            bleAckPayloadCache = payload;
            bleAckDest = dest;
        }
    }
    else if (line.startsWith("CMD,")) {
        handleCmd("LOCAL", line.substring(4));
    }
    else if (line.startsWith("ID ")) {
        String id = line.substring(3); id.trim(); id.toUpperCase();
        if (mesh.isValidNodeID(id)) {
            strncpy(mesh.localID, id.c_str(), NODE_ID_LEN);
            mesh.localID[NODE_ID_LEN] = '\0';
            mesh.idConflictDetected = false;  // Clear stale conflict on ID change
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
                ",SF:" + String(mesh.currentSF) +
                ",POWER:" + String(solarMode ? "SOLAR" : "NORMAL") +
                ",AUTOPOLL:" + String(autoPollEnabled ? (String(autoPollTarget) + "/" + String(autoPollInterval) + "s").c_str() : "OFF") +
                ",GPS:" + String(gpsEnabled ? (gps.location.isValid() ? "FIX" : "NOFIX") : "OFF") +
                ",SETUP:" + String(blePinIsDefault ? "1" : "0") +
                ",HEAP:" + String(ESP.getFreeHeap()));
    }
    else if (line == "SAVE") { saveConfig(); bleSend("OK,SAVED"); }
    else if (line.startsWith("SF,")) {
        // SF,<value> — initiate mesh-wide SF change (Phase 1: broadcast CFG)
        int newSF = line.substring(3).toInt();
        if (newSF < 7 || newSF > 12) { bleSend("ERR,SF,RANGE"); return; }
        String changeId = mesh.generateMsgID();
        String authTag = mesh.cfgAuthTag(changeId);
        String cfgPayload = "CFG,SF," + String(newSF) + "," + changeId + "," + String(mesh.localID) + "," + authTag;
        mesh.transmitPacket(0xFFFF, cfgPayload);
        bleSend("CFGSTART,SF," + String(newSF) + "," + changeId);
    }
    else if (line.startsWith("SFGO,")) {
        // SFGO,<value>,<changeId> — commit SF change (Phase 2: broadcast CFGGO)
        int c1 = line.indexOf(',', 5);
        if (c1 < 0) return;
        String value = line.substring(5, c1);
        String changeId = line.substring(c1 + 1);
        int newSF = value.toInt();
        if (newSF < 7 || newSF > 12) { bleSend("ERR,SF,RANGE"); return; }
        String authTag = mesh.cfgAuthTag(changeId);
        String goPayload = "CFGGO,SF," + String(newSF) + "," + changeId + "," + authTag;
        mesh.transmitPacket(0xFFFF, goPayload);
        // Initiator switches LAST (extra 500ms so CFGGO propagates first)
        sfChangePending = true;
        sfChangeTarget = newSF;
        sfChangeAt = millis() + CFG_SWITCH_DELAY + 500;
        bleSend("OK,SFGO," + String(newSF));
    }
#if defined(RADIO_SX1262)
    else if (line == "POWER,SOLAR") {
        bleSend("OK,POWER,SOLAR");  // Send response BEFORE deiniting BLE
        delay(200);  // Allow BLE notification to be sent
        solarMode = true;
        saveConfig();
        startSolarMode();
    }
    else if (line == "POWER,NORMAL") {
        if (solarMode) { stopSolarMode(); saveConfig(); }
        bleSend("OK,POWER,NORMAL");
    }
#endif
    else if (line.startsWith("BLEPIN,")) {
        String newPin = line.substring(7); newPin.trim();
        uint32_t pin = newPin.toInt();
        if (pin >= 100000 && pin <= 999999 && pin != BLE_DEFAULT_PIN) {
            blePin = pin;
            blePinIsDefault = false;
            EEPROM.put(EEPROM_BLEPIN_ADDR, blePin);
            EEPROM.commit();
            bleSend("OK,BLEPIN,SET");
            bleSend("PIN changed. Reconnect with new PIN.");
            // Restart BLE with new PIN
            delay(500);
            ESP.restart();
        } else if (pin == BLE_DEFAULT_PIN) {
            bleSend("ERR,BLEPIN,CANNOT_USE_DEFAULT");
        } else {
            bleSend("ERR,BLEPIN,MUST_BE_6_DIGITS");
        }
    }
    else if (line == "REBOOT") {
        bleSend("OK,REBOOT");
        delay(200);
        ESP.restart();
    }
    else if (line == "EEPROM,RESET") {
        // Wipe all EEPROM settings back to defaults
        for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0xFF);
        EEPROM.commit();
        bleSend("OK,EEPROM,RESET");
        delay(200);
        ESP.restart();
    }
    else if (line == "GPSPOS") {
        // Return current GPS position (if available)
        if (gpsEnabled && gps.location.isValid()) {
            bleSend("GPS," + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6));
        } else if (gpsEnabled) {
            bleSend("GPS,NOFIX");
        } else {
            bleSend("GPS,OFF");
        }
    }
    else if (line.startsWith("GPS,")) {
        // GPS pin config via BLE: GPS,<tx>,<rx> or GPS,OFF or GPS,ON
        String args = line.substring(4);
        if (args == "OFF") {
            gpsTxPin = -1; gpsRxPin = -1; gpsEnabled = false;
            bleSend("OK,GPS,OFF");
        } else if (args == "ON") {
            // Re-enable GPS with saved EEPROM pins
            uint8_t savedTx = EEPROM.read(EEPROM_GPS_ADDR);
            uint8_t savedRx = EEPROM.read(EEPROM_GPS_ADDR + 1);
            if (savedTx != 0xFF && savedRx != 0xFF) {
                gpsTxPin = savedTx; gpsRxPin = savedRx;
                setupGPS();
                bleSend("OK,GPS," + String(gpsTxPin) + "," + String(gpsRxPin));
            } else {
                bleSend("ERR,GPS,NO_SAVED_PINS");
            }
        } else {
            int comma = args.indexOf(',');
            if (comma > 0) {
                int tx = args.substring(0, comma).toInt();
                int rx = args.substring(comma + 1).toInt();
                if (isPinSafe(tx) && isPinSafe(rx)) {
                    gpsTxPin = tx; gpsRxPin = rx;
                    setupGPS();
                    bleSend("OK,GPS," + String(tx) + "," + String(rx));
                } else {
                    bleSend("ERR,GPS,BADPIN");
                }
            }
        }
    }
    else if (line == "NODES") {
        // Send full list of known nodes with routing info
        bleSend("NODELIST," + String(mesh.knownCount));
        for (uint8_t i = 0; i < mesh.knownCount; i++) {
            String nodeId = String(mesh.knownNodes[i]);
            int rssi = 0;
            int hops = 0;
            unsigned long age = 0;  // seconds since last seen
            auto it = mesh.routingTable.find(nodeId);
            if (it != mesh.routingTable.end()) {
                rssi = it->second.rssi;
                hops = it->second.cost / COST_WEIGHT;
                if (hops < 1) hops = 1;
                age = (millis() - it->second.lastSeen) / 1000;
            }
            bool active = (age < (STALE_TIMEOUT / 1000));
            String nextHop = "";
            if (it != mesh.routingTable.end()) nextHop = it->second.nextHop;
            // Format: NODE,<id>,<rssi>,<hops>,<age_seconds>,<active>,<nextHop>
            bleSend("NODE," + nodeId + "," + String(rssi) + "," +
                    String(hops) + "," + String(age) + "," +
                    String(active ? "1" : "0") + "," + nextHop);
        }
        bleSend("NODEEND");
    }
    else if (line == "POLL") {
        // Poll all local sensors — return bundled SDATA
        String sdata = "SDATA," + String(mesh.localID);
        for (int i = 0; i < sensorCount; i++) {
            int pin = sensorPins[i];
            int value = readSensorPin(pin);
            sdata += "," + String(pin) + ":" + String(value);
        }
        bleSend(sdata);
    }
    else if (line.startsWith("POLL,")) {
        // Poll remote node's sensors over mesh: POLL,<targetNodeId>
        String target = line.substring(5); target.trim(); target.toUpperCase();
        if (!mesh.isValidNodeID(target)) { bleSend("ERR,POLL,BADID"); return; }
        uint16_t dest = strtol(target.c_str(), nullptr, 16);
        String mid = mesh.generateMsgID();
        String cmdText = "CMD,POLL";
        String hex = mesh.encryptMsg(cmdText);
        String payload = String(mesh.localID) + "," + target + "," + mid + "," +
                         String(TTL_DEFAULT) + "," + String(mesh.localID) + "," + hex;
        mesh.transmitPacket(dest, payload);
        bleSend("SENT," + target + "," + mid);
    }
    else if (line.startsWith("AUTOPOLL,")) {
        // Configure auto-poll: AUTOPOLL,<target>,<interval> or AUTOPOLL,OFF
        String args = line.substring(9); args.trim();
        if (args == "OFF") {
            autoPollEnabled = false;
            saveConfig();
            bleSend("OK,AUTOPOLL,OFF");
        } else {
            int comma = args.indexOf(',');
            if (comma > 0) {
                String target = args.substring(0, comma); target.trim(); target.toUpperCase();
                int interval = args.substring(comma + 1).toInt();
                if (!mesh.isValidNodeID(target)) { bleSend("ERR,AUTOPOLL,BADID"); return; }
                if (interval < AUTOPOLL_MIN_INTERVAL) { bleSend("ERR,AUTOPOLL,MIN30S"); return; }
                autoPollEnabled = true;
                strncpy(autoPollTarget, target.c_str(), NODE_ID_LEN);
                autoPollTarget[NODE_ID_LEN] = '\0';
                autoPollInterval = interval;
                nextAutoPollTime = millis() + (unsigned long)interval * 1000UL;
                saveConfig();
                bleSend("OK,AUTOPOLL," + String(autoPollTarget) + "," + String(interval));
            } else {
                bleSend("ERR,AUTOPOLL,FORMAT");
            }
        }
    }
    else if (line.startsWith("TIMER,")) {
        handleTimerBle(line.substring(6));
    }
    else if (line.startsWith("SETPOINT,")) {
        handleSetpointBle(line.substring(9));
    }
    else if (line.startsWith("PINMODE,")) {
        // PINMODE,<pin>,PULSE|ANALOG|AUTO — set sensor pin mode
        String args = line.substring(8); args.trim();
        int comma = args.indexOf(',');
        if (comma < 0) { bleSend("ERR,PINMODE,FORMAT"); return; }
        int pin = args.substring(0, comma).toInt();
        String mode = args.substring(comma + 1); mode.trim(); mode.toUpperCase();
        // Verify pin is a configured sensor pin
        int pinIdx = -1;
        for (int i = 0; i < sensorCount; i++) {
            if (sensorPins[i] == pin) { pinIdx = i; break; }
        }
        if (pinIdx < 0) { bleSend("ERR,PINMODE,NOT_SENSOR"); return; }
        if (mode == "PULSE") {
            if (!enablePulseCounter(pin)) { bleSend("ERR,PINMODE,MAX4"); return; }
            sensorMode[pinIdx] = SENSOR_MODE_PULSE;
            bleSend("OK,PINMODE," + String(pin) + ",PULSE");
        } else if (mode == "ANALOG" || mode == "AUTO") {
            disablePulseCounter(pin);
            sensorMode[pinIdx] = SENSOR_MODE_AUTO;
            pinMode(pin, INPUT);
            bleSend("OK,PINMODE," + String(pin) + ",AUTO");
        } else {
            bleSend("ERR,PINMODE,UNKNOWN");
        }
    }
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
            mesh.idConflictDetected = false;  // Clear stale conflict on ID change
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
        if (autoPollEnabled) {
            Serial.println("AutoPoll: " + String(autoPollTarget) + " every " + String(autoPollInterval) + "s");
        } else {
            Serial.println("AutoPoll: OFF");
        }
    }
    else if (line == "SAVE") { saveConfig(); Serial.println("OK: Saved"); }
    else if (line == "RESET") {
        strncpy(mesh.localID, "0010", NODE_ID_LEN + 1);
        // Generate random AES key
        for (int i = 0; i < AES_KEY_LEN; i++)
            mesh.aes_key_string[i] = 33 + (esp_random() % 94);
        mesh.aes_key_string[AES_KEY_LEN] = '\0';
        uint8_t d[] = DEFAULT_RELAY_PINS; relayCount = sizeof(d)/sizeof(d[0]); memcpy(relayPins, d, relayCount);
        uint8_t s[] = DEFAULT_SENSOR_PINS; sensorCount = sizeof(s)/sizeof(s[0]); memcpy(sensorPins, s, sensorCount);
        saveConfig();
        Serial.println("OK: Reset. New key: " + String(mesh.aes_key_string));
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
        if (mesh.canForward()) {
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
    else if (line.startsWith("AUTOPOLL ")) {
        String args = line.substring(9); args.trim();
        if (args == "OFF") {
            autoPollEnabled = false;
            saveConfig();
            Serial.println("OK: Auto-poll disabled");
        } else {
            // AUTOPOLL <target> <interval>
            int sp = args.indexOf(' ');
            if (sp > 0) {
                String target = args.substring(0, sp); target.trim(); target.toUpperCase();
                int interval = args.substring(sp + 1).toInt();
                if (!mesh.isValidNodeID(target)) { Serial.println("ERROR: Invalid target ID"); return; }
                if (interval < AUTOPOLL_MIN_INTERVAL) { Serial.println("ERROR: Minimum interval is 30s"); return; }
                autoPollEnabled = true;
                strncpy(autoPollTarget, target.c_str(), NODE_ID_LEN);
                autoPollTarget[NODE_ID_LEN] = '\0';
                autoPollInterval = interval;
                nextAutoPollTime = millis() + (unsigned long)interval * 1000UL;
                saveConfig();
                Serial.println("OK: Auto-poll to " + String(autoPollTarget) + " every " + String(interval) + "s");
            } else {
                Serial.println("Usage: AUTOPOLL <targetId> <seconds> | AUTOPOLL OFF");
            }
        }
    }
#if defined(RADIO_SX1262)
    else if (line == "SOLAR ON") {
        solarMode = true;
        saveConfig();
        Serial.println("OK: Solar mode ON — OLED off, BLE off, CPU light-sleep between packets");
        Serial.println("Send SOLAR OFF via serial or press PRG to exit");
        Serial.flush();
        startSolarMode();
    }
    else if (line == "SOLAR OFF") {
        stopSolarMode();
        saveConfig();
        Serial.println("OK: Solar mode OFF — normal operation restored");
    }
#endif
    else if (line.startsWith("PINMODE ")) {
        // PINMODE <pin> PULSE|ANALOG|AUTO — set sensor pin mode
        String args = line.substring(8); args.trim();
        int sp = args.indexOf(' ');
        if (sp < 0) { Serial.println("Usage: PINMODE <pin> PULSE|ANALOG|AUTO"); return; }
        int pin = args.substring(0, sp).toInt();
        String mode = args.substring(sp + 1); mode.trim(); mode.toUpperCase();
        int pinIdx = -1;
        for (int i = 0; i < sensorCount; i++) {
            if (sensorPins[i] == pin) { pinIdx = i; break; }
        }
        if (pinIdx < 0) { Serial.println("ERROR: Pin " + String(pin) + " is not a configured sensor pin"); return; }
        if (mode.startsWith("PULSE")) {
            if (!enablePulseCounter(pin)) { Serial.println("ERROR: Max 4 pulse counters"); return; }
            sensorMode[pinIdx] = SENSOR_MODE_PULSE;
            // Optional sample window: PINMODE <pin> PULSE <windowMs>
            int sp2 = mode.indexOf(' ');
            if (sp2 > 0) {
                uint16_t windowMs = mode.substring(sp2 + 1).toInt();
                if (windowMs >= 100) {
                    int idx = findPulseCounter(pin);
                    if (idx >= 0) pulseCounters[idx].sampleWindowMs = windowMs;
                }
            }
            int idx = findPulseCounter(pin);
            uint16_t win = (idx >= 0) ? pulseCounters[idx].sampleWindowMs : 5000;
            Serial.println("OK: Pin " + String(pin) + " set to PULSE mode (window=" + String(win) + "ms)");
        } else if (mode == "ANALOG" || mode == "AUTO") {
            disablePulseCounter(pin);
            sensorMode[pinIdx] = SENSOR_MODE_AUTO;
            pinMode(pin, INPUT);
            Serial.println("OK: Pin " + String(pin) + " set to AUTO mode");
        } else {
            Serial.println("ERROR: Unknown mode. Use PULSE, ANALOG, or AUTO");
        }
    }
    else if (line.startsWith("GPS ")) {
        String args = line.substring(4); args.trim();
        if (args == "OFF" || args == "off") {
            gpsTxPin = -1; gpsRxPin = -1; gpsEnabled = false;
            Serial.println("OK: GPS disabled. SAVE to persist.");
        } else {
            int comma = args.indexOf(',');
            if (comma > 0) {
                int tx = args.substring(0, comma).toInt();
                int rx = args.substring(comma + 1).toInt();
                if (isPinSafe(tx) && isPinSafe(rx)) {
                    gpsTxPin = tx; gpsRxPin = rx;
                    setupGPS();
                    Serial.println("OK: GPS on TX=" + String(tx) + " RX=" + String(rx) + ". SAVE to persist.");
                } else {
                    Serial.println("ERROR: GPS pins not safe (conflict with radio/I2C/USB)");
                }
            } else {
                Serial.println("Usage: GPS <tx>,<rx>  or  GPS OFF");
            }
        }
    }
    else if (line == "EXPAND ON") {
#if IO_EXPAND_TX >= 0
        IOSerial.begin(IO_EXPAND_BAUD, SERIAL_8N1, IO_EXPAND_RX, IO_EXPAND_TX);
        ioExpandEnabled = true;
        memset(ioExpandCache, 0, sizeof(ioExpandCache));
        memset(ioExpandCacheValid, 0, sizeof(ioExpandCacheValid));
        ioExpandRxBuf = "";
        Serial.println("OK: IO expansion enabled on UART2 (TX=" + String(IO_EXPAND_TX) + " RX=" + String(IO_EXPAND_RX) + ")");
        Serial.println("Virtual pins " + String(IO_EXPAND_VPIN_BASE) + "-" + String(IO_EXPAND_VPIN_BASE + IO_EXPAND_MAX_PINS - 1));
#else
        Serial.println("ERROR: IO expansion not supported on this board");
#endif
    }
    else if (line == "EXPAND OFF") {
#if IO_EXPAND_TX >= 0
        ioExpandEnabled = false;
        Serial.println("OK: IO expansion disabled");
#else
        Serial.println("ERROR: IO expansion not supported on this board");
#endif
    }
    // Route BLE-format comma commands received over serial (gateway GUI uses this format)
    else if (line.startsWith("POLL") || line.startsWith("TIMER,") ||
             line.startsWith("CMD,") || line.startsWith("MSG,") ||
             line.startsWith("SETPOINT,") || line.startsWith("AUTOPOLL,")) {
        processBleCommand(line);
    }
    else Serial.println("Commands: ID xxxx | KEY xxx... | RELAY 2,4,12 | SENSOR 34,36 | GPS <tx>,<rx> | GPS OFF | STATUS | SAVE | RESET | GATEWAY ON/OFF | AUTOPOLL <id> <sec> | PINMODE <pin> PULSE|AUTO"
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
    EEPROM.write(EEPROM_SF_ADDR, mesh.currentSF);
    // Auto-poll config: enabled(1) + target(5) + interval(2) = 8 bytes at addr 433
    EEPROM.write(EEPROM_AUTOPOLL_ADDR, autoPollEnabled ? 1 : 0);
    EEPROM.put(EEPROM_AUTOPOLL_ADDR + 1, autoPollTarget);
    EEPROM.put(EEPROM_AUTOPOLL_ADDR + 6, autoPollInterval);
    // GPS pins
    EEPROM.write(EEPROM_GPS_ADDR, (uint8_t)(gpsTxPin >= 0 ? gpsTxPin : 0xFF));
    EEPROM.write(EEPROM_GPS_ADDR + 1, (uint8_t)(gpsRxPin >= 0 ? gpsRxPin : 0xFF));
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
    EEPROM.commit();
}

bool firstBoot = false;  // set by loadConfig, checked in setup() for conflict scan

void loadConfig() {
    if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
        debugPrint("First boot - generating random ID");
        firstBoot = true;
        // Generate random 4-hex-char node ID (0001-FFFE, avoids 0000 and FFFF)
        uint16_t randId = random(1, 0xFFFE);
        char idBuf[5];
        snprintf(idBuf, sizeof(idBuf), "%04X", randId);
        strncpy(mesh.localID, idBuf, NODE_ID_LEN + 1);
        uint8_t d[] = DEFAULT_RELAY_PINS; relayCount = sizeof(d)/sizeof(d[0]); memcpy(relayPins, d, relayCount);
        uint8_t s[] = DEFAULT_SENSOR_PINS; sensorCount = sizeof(s)/sizeof(s[0]); memcpy(sensorPins, s, sensorCount);
        // Don't saveConfig() yet — defer until after conflict scan in setup()
        return;
    }
    EEPROM.get(0, mesh.localID);
    if (!mesh.isValidNodeID(String(mesh.localID))) strncpy(mesh.localID, "0010", NODE_ID_LEN + 1);

    EEPROM.get(EEPROM_KEY_ADDR, mesh.aes_key_string);
    bool keyValid = (strlen(mesh.aes_key_string) == AES_KEY_LEN);
    if (keyValid) for (int i = 0; i < AES_KEY_LEN; i++)
        if (mesh.aes_key_string[i] < 0x20 || mesh.aes_key_string[i] > 0x7E) { keyValid = false; break; }
    if (!keyValid) {
        // Generate random AES key
        for (int i = 0; i < AES_KEY_LEN; i++)
            mesh.aes_key_string[i] = 33 + (esp_random() % 94);
        mesh.aes_key_string[AES_KEY_LEN] = '\0';
        debugPrint("Generated new AES key: " + String(mesh.aes_key_string));
    }

    EEPROM.get(EEPROM_GPIO_ADDR, relayCount);
    if (relayCount > MAX_RELAY_PINS_CFG) relayCount = 0;
    EEPROM.get(EEPROM_GPIO_ADDR + 1, relayPins);
    EEPROM.get(EEPROM_GPIO_ADDR + 1 + MAX_RELAY_PINS_CFG, sensorCount);
    if (sensorCount > MAX_SENSOR_PINS_CFG) sensorCount = 0;
    EEPROM.get(EEPROM_GPIO_ADDR + 2 + MAX_RELAY_PINS_CFG, sensorPins);

    // Load solar mode flag
    uint8_t solarByte = EEPROM.read(EEPROM_SOLAR_ADDR);
    solarMode = (solarByte == 1);

    // Load persisted SF (valid range 7-12, else use compile-time default)
    uint8_t storedSF = EEPROM.read(EEPROM_SF_ADDR);
    mesh.currentSF = (storedSF >= 7 && storedSF <= 12) ? storedSF : LORA_SF;

    // Load auto-poll config
    uint8_t apEnabled = EEPROM.read(EEPROM_AUTOPOLL_ADDR);
    if (apEnabled == 1) {
        autoPollEnabled = true;
        EEPROM.get(EEPROM_AUTOPOLL_ADDR + 1, autoPollTarget);
        autoPollTarget[NODE_ID_LEN] = '\0';
        EEPROM.get(EEPROM_AUTOPOLL_ADDR + 6, autoPollInterval);
        if (autoPollInterval < AUTOPOLL_MIN_INTERVAL) autoPollInterval = 300;
        if (!mesh.isValidNodeID(String(autoPollTarget))) autoPollEnabled = false;
        if (autoPollEnabled) nextAutoPollTime = millis() + 10000;  // Start 10s after boot
    }

    // Load GPS pins — only override board default if user explicitly configured via GPS command
    // EEPROM may contain stale data from previous firmware versions at this address
    uint8_t gTx = EEPROM.read(EEPROM_GPS_ADDR);
    uint8_t gRx = EEPROM.read(EEPROM_GPS_ADDR + 1);
    if (gTx != 0xFF && gRx != 0xFF && gTx > 1 && gRx > 1 && gTx < 48 && gRx < 48) {
        // Valid saved GPS pins — use them
        gpsTxPin = (int8_t)gTx;
        gpsRxPin = (int8_t)gRx;
    } else {
        // No valid saved config — use board defaults (may be -1 = disabled)
        gpsTxPin = BOARD_GPS_TX;
        gpsRxPin = BOARD_GPS_RX;
    }

    // Load BLE PIN
    uint32_t savedPin = 0;
    EEPROM.get(EEPROM_BLEPIN_ADDR, savedPin);
    if (savedPin != 0xFFFFFFFF && savedPin >= 100000 && savedPin <= 999999) {
        blePin = savedPin;
        blePinIsDefault = false;
    } else {
        blePin = BLE_DEFAULT_PIN;
        blePinIsDefault = true;
    }
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
    delay(1000);
    Serial.println("\n═══════════════════════════════════");
    Serial.println("  TiggyOpenMesh Repeater v4.0");
    Serial.println("  Board: " + String(BOARD_NAME));
    Serial.println("═══════════════════════════════════");

    // Seed RNG with hardware random XOR'd with unique MAC address
    // esp_random() alone may not be truly random this early in boot (before WiFi/BT)
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint32_t macSeed = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                       ((uint32_t)mac[4] << 8)  | mac[5];
    randomSeed(esp_random() ^ macSeed);

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
    mesh.onAck = handleAck;
    mesh.onCfg = handleCfg;
    mesh.onCfgAck = handleCfgAck;
    mesh.onCfgGo = handleCfgGo;
    mesh.onIdConflict = handleIdConflict;

    // SPI + Radio
    loraSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, RADIO_CS);
    setupRadio();

    // ─── First boot: scan for ID conflicts before saving ─────────
    if (firstBoot) {
        // Show scan status on OLED
        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
        bool oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);

        bool idClear = false;
        while (!idClear) {
            mesh.idConflictDetected = false;

            if (oledOk) {
                oled.clearDisplay();
                oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
                oled.setCursor(0, 0);
                oled.println("First Boot Setup");
                oled.println("");
                oled.print("ID: "); oled.println(mesh.localID);
                oled.println("");
                oled.println("Scanning for");
                oled.println("conflicts...");
                oled.display();
            }
            debugPrint("First boot: scanning for ID " + String(mesh.localID));

            // Listen for 10 seconds, checking for conflicts
            unsigned long scanEnd = millis() + 10000;
            while (millis() < scanEnd) {
                receiveCheck();
                mesh.processPendingForwards();

                // Show countdown on OLED
                if (oledOk && (millis() % 1000) < 50) {
                    int remaining = (scanEnd - millis()) / 1000;
                    oled.fillRect(0, 54, OLED_W, 10, SSD1306_BLACK);
                    oled.setCursor(0, 54);
                    oled.print("Time left: "); oled.print(remaining); oled.print("s");
                    oled.display();
                }

                if (mesh.idConflictDetected) break;
                yield();
            }

            if (mesh.idConflictDetected) {
                // Conflict found — regenerate random ID and rescan
                uint16_t randId = random(1, 0xFFFE);
                char idBuf[5];
                snprintf(idBuf, sizeof(idBuf), "%04X", randId);
                strncpy(mesh.localID, idBuf, NODE_ID_LEN + 1);
                debugPrint("ID conflict! Regenerated to " + String(mesh.localID));
                if (oledOk) {
                    oled.clearDisplay();
                    oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
                    oled.setCursor(0, 0);
                    oled.println("ID Conflict!");
                    oled.println("Trying new ID...");
                    oled.print("New: "); oled.println(mesh.localID);
                    oled.display();
                    delay(1500);
                }
            } else {
                idClear = true;
            }
        }

        // ID confirmed — save config (writes magic byte)
        saveConfig();
        debugPrint("First boot: ID confirmed as " + String(mesh.localID));
        if (oledOk) {
            oled.clearDisplay();
            oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
            oled.setCursor(0, 0);
            oled.println("ID confirmed:");
            oled.setTextSize(2);
            oled.setCursor(0, 20);
            oled.println(mesh.localID);
            oled.display();
            delay(2000);
            oledAvailable = false;  // Will be re-init'd below in normal boot
        }
    }

    setupGPIO();
    setupGPS();

    // ─── Solar mode boot path ──────────────────────────────────
    // BLE stays active, OLED off — full radio, no packet loss
#if defined(RADIO_SX1262)
    if (solarMode) {
        setupBLE();  // BLE stays on for app access
        // Brief OLED splash then turn it off
        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
        if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
            oled.clearDisplay();
            oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
            oled.setCursor(0, 0);
            oled.println("SOLAR MODE");
            oled.println("ID: " + String(mesh.localID));
            oled.println(String(LORA_FREQ, 1) + " MHz");
            oled.println("OLED off, BLE+radio on");
            oled.display();
            delay(2000);
        }
        nextHeartbeatTime = millis() + random(2000, 8000);
        startSolarMode();
        Serial.println("Solar mode active. OLED off, BLE off, CPU light-sleep between packets.");
        Serial.println("Send SOLAR OFF via serial or press PRG button to restore.");
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
    // ─── Solar mode — deep low-power with light sleep ──────────
    // OLED off, BLE off, CPU sleeps between DIO1 (radio packet) interrupts.
    // Radio stays in continuous RX — no packets lost.
    // ~11mA total (CPU 0.8mA light-sleep + radio 10mA RX).
#if defined(RADIO_SX1262)
    if (solarMode) {
        // Radio RX — process any packet that woke us
        receiveCheck();
        handleBleAckRetry();
        mesh.processPendingForwards();

        // Heartbeat (60s interval in solar mode to save TX power)
        if (millis() > nextHeartbeatTime) {
            mesh.sendHeartbeat();
            nextHeartbeatTime = millis() + HB_INTERVAL_SOLAR + random(-5000, 5000);
            if (BOARD_LED >= 0) { digitalWrite(BOARD_LED, HIGH); delay(2); digitalWrite(BOARD_LED, LOW); }
        }

        // Route cleanup
        if (millis() - lastRouteClean > 60000) {
            mesh.pruneStale();
            lastRouteClean = millis();
        }

        // Button: temporarily show OLED status
        solarCheckButton();

        // Timers, IO expansion, pulse rates, setpoints, GPS, and auto-poll still run
        processTimers();
        ioExpandPoll();
        updatePulseRates();
        checkSetpoints();
        readGPS();
        broadcastGPS();
        executeAutoPoll();

        // BLE + Serial commands — app can toggle solar mode off via BLE
        processBleQueue();
        handleSerialConfig();

        // Enter light sleep — CPU halts until DIO1 (packet), button, or timer
        // This is where the power saving happens (~0.8mA vs ~80mA active)
        if (!mesh.rxFlag && !Serial.available()) {
            solarLightSleep();
        }
        return;
    }
#endif

    // ─── Normal loop ───────────────────────────────────────────
    // 0. Process queued BLE commands (runs on main stack, not BTC_TASK)
    processBleQueue();

    // 1. Radio RX — MeshCore handles routing, forwarding, decryption
    receiveCheck();
    handleBleAckRetry();

    // 2. Process jittered forwards
    mesh.processPendingForwards();

    // 2b. SF change execution (after delay from CFGGO)
    if (sfChangePending && millis() >= sfChangeAt) {
        sfChangePending = false;
        radio.standby();
        int sfState = radio.setSpreadingFactor(sfChangeTarget);
        if (sfState == RADIOLIB_ERR_NONE) {
            mesh.currentSF = sfChangeTarget;
            EEPROM.write(EEPROM_SF_ADDR, sfChangeTarget);
            EEPROM.commit();
            debugPrint("SF changed to " + String(sfChangeTarget));
            bleSend("OK,SF," + String(sfChangeTarget));
        } else {
            debugPrint("SF change FAILED: " + String(sfState));
            bleSend("ERR,SF," + String(sfState));
        }
        radioStartListening();
    }

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

    // 7. Relay timers
    processTimers();

    // 8. IO expansion, pulse rates, setpoint rules
    ioExpandPoll();
    updatePulseRates();
    checkSetpoints();

    // 9. GPS read + broadcast
    readGPS();
    broadcastGPS();

    // 10. Auto-poll periodic sensor reporting
    executeAutoPoll();

    // 10. Button → manual heartbeat
#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    static bool lastBtn = true;
    static unsigned long btnPressStart = 0;
    bool btn = digitalRead(BOARD_BUTTON);
    if (!btn && lastBtn) {
        // Button just pressed
        btnPressStart = millis();
        mesh.sendHeartbeat();
        Serial.println("Manual heartbeat sent");
    }
    if (!btn && btnPressStart > 0 && (millis() - btnPressStart) > 10000) {
        // Held for 10 seconds — reset BLE PIN to default
        blePin = BLE_DEFAULT_PIN;
        blePinIsDefault = true;
        uint32_t resetVal = 0xFFFFFFFF;
        EEPROM.put(EEPROM_BLEPIN_ADDR, resetVal);
        EEPROM.commit();
        Serial.println("BLE PIN reset to default (123456)");
        btnPressStart = 0;  // Prevent re-triggering
        ESP.restart();
    }
    if (btn) btnPressStart = 0;
    lastBtn = btn;
#endif

    yield();
}

#endif // repeater boards
