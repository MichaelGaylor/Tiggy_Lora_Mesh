#include <Arduino.h>
/*
 * TiggyOpenMesh — IO Expansion Board Firmware
 * ═══════════════════════════════════════════
 * Minimal serial slave for daisy-chaining additional I/O to a mesh node.
 * Connects to the main ESP32 via UART (Serial). No LoRa, no mesh — just I/O.
 *
 * Protocol (ASCII, line-based, 115200 baud):
 *   Main sends:     Expansion responds:
 *   ?S              S,<pin>:<val>,<pin>:<val>,...    (read all sensors)
 *   ?S,<pin>        S,<pin>:<val>                   (read single sensor)
 *   !R,<pin>,<val>  R,OK                            (set relay/output pin)
 *   ?P              P,<pin>:<count>,...              (read pulse counters)
 *   ?I              I,ESP32,<pinCount>               (identify board)
 *
 * Wiring: Connect TX→RX, RX→TX, GND→GND between main and expansion ESP32.
 *
 * Configure SENSOR_PINS and RELAY_PINS below for your board.
 */

// ─── Configuration ──────────────────────────────────────────
// Adjust these for your expansion board's wiring
const int SENSOR_PINS[] = {34, 35, 36, 39, 32, 33};  // Analog/digital inputs
const int RELAY_PINS[]  = {2, 4, 5, 12, 13, 14};     // Digital outputs
const int NUM_SENSORS   = sizeof(SENSOR_PINS) / sizeof(SENSOR_PINS[0]);
const int NUM_RELAYS    = sizeof(RELAY_PINS) / sizeof(RELAY_PINS[0]);

// Pulse counters (optional — attach anemometers, flow meters, etc.)
#define MAX_PULSE_PINS 4
struct PulseCounter {
    int pin;
    volatile uint32_t count;
    bool active;
};
static PulseCounter pulseCounters[MAX_PULSE_PINS];
static int pulseCounterCount = 0;

// ISR handlers
static void IRAM_ATTR pulseISR0() { pulseCounters[0].count++; }
static void IRAM_ATTR pulseISR1() { pulseCounters[1].count++; }
static void IRAM_ATTR pulseISR2() { pulseCounters[2].count++; }
static void IRAM_ATTR pulseISR3() { pulseCounters[3].count++; }
static void (*pulseISRs[MAX_PULSE_PINS])() = { pulseISR0, pulseISR1, pulseISR2, pulseISR3 };

// ─── Setup ──────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    // Configure sensor pins as inputs
    for (int i = 0; i < NUM_SENSORS; i++) {
        pinMode(SENSOR_PINS[i], INPUT);
    }

    // Configure relay pins as outputs (default LOW)
    for (int i = 0; i < NUM_RELAYS; i++) {
        pinMode(RELAY_PINS[i], OUTPUT);
        digitalWrite(RELAY_PINS[i], LOW);
    }
}

// ─── Command Handler ────────────────────────────────────────

void handleCommand(String cmd) {
    cmd.trim();

    if (cmd == "?S") {
        // Read all sensors
        String resp = "S";
        for (int i = 0; i < NUM_SENSORS; i++) {
            int val = (SENSOR_PINS[i] >= 32) ? analogRead(SENSOR_PINS[i])
                                              : digitalRead(SENSOR_PINS[i]);
            resp += "," + String(SENSOR_PINS[i]) + ":" + String(val);
        }
        Serial.println(resp);
    }
    else if (cmd.startsWith("?S,")) {
        // Read single sensor
        int pin = cmd.substring(3).toInt();
        int val = (pin >= 32) ? analogRead(pin) : digitalRead(pin);
        Serial.println("S," + String(pin) + ":" + String(val));
    }
    else if (cmd.startsWith("!R,")) {
        // Set relay: !R,<pin>,<val>
        int c1 = cmd.indexOf(',', 3);
        if (c1 > 0) {
            int pin = cmd.substring(3, c1).toInt();
            int val = cmd.substring(c1 + 1).toInt();
            digitalWrite(pin, val ? HIGH : LOW);
            Serial.println("R,OK");
        } else {
            Serial.println("R,ERR");
        }
    }
    else if (cmd == "?P") {
        // Read pulse counters
        String resp = "P";
        for (int i = 0; i < pulseCounterCount; i++) {
            if (!pulseCounters[i].active) continue;
            resp += "," + String(pulseCounters[i].pin) + ":" + String(pulseCounters[i].count);
        }
        if (pulseCounterCount == 0) resp += ",NONE";
        Serial.println(resp);
    }
    else if (cmd == "?I") {
        // Identify
        Serial.println("I,ESP32," + String(NUM_SENSORS) + "S," + String(NUM_RELAYS) + "R");
    }
    else if (cmd.startsWith("!P,")) {
        // Enable pulse counter: !P,<pin>
        int pin = cmd.substring(3).toInt();
        if (pulseCounterCount < MAX_PULSE_PINS) {
            int idx = pulseCounterCount++;
            pulseCounters[idx].pin = pin;
            pulseCounters[idx].count = 0;
            pulseCounters[idx].active = true;
            pinMode(pin, INPUT_PULLUP);
            attachInterrupt(digitalPinToInterrupt(pin), pulseISRs[idx], RISING);
            Serial.println("P,OK," + String(pin));
        } else {
            Serial.println("P,FULL");
        }
    }
}

// ─── Main Loop ──────────────────────────────────────────────

String rxBuf = "";

void loop() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            handleCommand(rxBuf);
            rxBuf = "";
        } else if (c != '\r') {
            rxBuf += c;
        }
    }
}
