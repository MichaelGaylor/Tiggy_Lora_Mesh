// ═══════════════════════════════════════════════════════════════
// TiggyOpenMesh WiFi Gateway v1.1
// ═══════════════════════════════════════════════════════════════
// Bridges the LoRa mesh network to a cloud hub via WebSocket.
//
// Receives packets from the LoRa mesh and forwards them as JSON
// to a hub server.  Receives JSON "pkt" messages from the hub
// and transmits them into the local LoRa mesh.
//
// Configuration via BLE (Nordic UART Service):
//   WIFI,ssid,password         → store WiFi credentials
//   HUBURL,wss://your-hub.ts.net → store hub WebSocket URL (wss:// for Tailscale Funnel)
//   HUBKEY,secret              → store hub auth key
//   GWNAME,My Gateway          → store gateway display name
//   GWLOC,51.5,-3.2            → store lat/lon
//   GWANTENNA,0                → 0=indoor 1=external 2=rooftop
//   SAVE                       → persist all to NVS and reboot
//   GWSTATUS  (or STATUS)      → reply with current config
//
// Boards supported (select via -D build flag):
//   BOARD_HELTEC_V2  — ESP32  + SX1276
//   BOARD_HELTEC_V3  — ESP32-S3 + SX1262
//   BOARD_HELTEC_V4  — ESP32-S3 + SX1262 + 28dBm PA
//   BOARD_XIAO_S3    — ESP32-S3 + SX1262 (Wio-SX1262 kit)
// ═══════════════════════════════════════════════════════════════

#if defined(GATEWAY_MODE)

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <WebSocketsClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "Pins.h"

// ─── Conditional OLED ────────────────────────────────────────
#if HAS_OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define OLED_W 128
#define OLED_H 64
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, OLED_RST);
bool oledAvailable = false;
#endif

// ─── Debug helpers ───────────────────────────────────────────
#ifndef DEBUG
#define DEBUG 1
#endif
#if DEBUG
  #define dbg(x)   Serial.println(x)
  #define dbgf(...) Serial.printf(__VA_ARGS__)
#else
  #define dbg(x)
  #define dbgf(...)
#endif

// ─── BLE UUIDs (Nordic UART Service — same as repeater) ──────
#define BLE_SERVICE_UUID   "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_TX_CHAR_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX_CHAR_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ─── NVS namespace ────────────────────────────────────────────
#define NVS_NS  "gw_cfg"

// ─── Timing constants ─────────────────────────────────────────
#define WIFI_RECONNECT_INTERVAL_MS   30000UL
#define WS_RECONNECT_INTERVAL_MS     15000UL
#define OLED_REFRESH_INTERVAL_MS      5000UL

// ─── Dedup cache ─────────────────────────────────────────────
#define DEDUP_SIZE      256
#define DEDUP_TTL_MS  30000UL

struct DedupEntry {
    uint32_t hash;
    unsigned long ts;
};
static DedupEntry dedupCache[DEDUP_SIZE];
static uint8_t    dedupHead = 0;

// Simple djb2-style hash over raw bytes
static uint32_t hashBytes(const uint8_t* data, size_t len) {
    uint32_t h = 5381;
    for (size_t i = 0; i < len; i++) h = ((h << 5) + h) ^ data[i];
    return h;
}

static bool dedupSeen(const uint8_t* data, size_t len) {
    uint32_t h = hashBytes(data, len);
    unsigned long now = millis();
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (dedupCache[i].hash == h && (now - dedupCache[i].ts) < DEDUP_TTL_MS)
            return true;
    }
    // Not seen — record it
    dedupCache[dedupHead].hash = h;
    dedupCache[dedupHead].ts   = now;
    dedupHead = (dedupHead + 1) & (DEDUP_SIZE - 1);
    return false;
}

// ─── Config (loaded from NVS) ─────────────────────────────────
struct GwConfig {
    char  wifi_ssid[64];
    char  wifi_pass[64];
    char  hub_url[128];   // e.g. ws://hub.example.com:9000
    char  hub_key[64];
    char  gw_name[64];
    char  ble_password[32]; // BLE auth password (empty = no protection)
    float gw_lat;
    float gw_lon;
    uint8_t antenna_type; // 0=indoor 1=external 2=rooftop
    float gw_height;      // antenna height in meters
};

static GwConfig cfg;
static Preferences prefs;

void loadConfig() {
    prefs.begin(NVS_NS, true);  // read-only
    prefs.getString("wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    prefs.getString("wifi_pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    prefs.getString("hub_url",   cfg.hub_url,   sizeof(cfg.hub_url));
    prefs.getString("hub_key",   cfg.hub_key,   sizeof(cfg.hub_key));
    prefs.getString("gw_name",   cfg.gw_name,   sizeof(cfg.gw_name));
    prefs.getString("ble_pass",   cfg.ble_password, sizeof(cfg.ble_password));
    cfg.gw_lat      = prefs.getFloat("gw_lat",      0.0f);
    cfg.gw_lon      = prefs.getFloat("gw_lon",      0.0f);
    cfg.antenna_type = prefs.getUChar("antenna",    0);
    cfg.gw_height    = prefs.getFloat("gw_height",  2.0f);
    prefs.end();

    // Defaults
    if (strlen(cfg.gw_name) == 0) strncpy(cfg.gw_name, "LoRa Gateway", sizeof(cfg.gw_name) - 1);
    if (strlen(cfg.hub_url) == 0) strncpy(cfg.hub_url,  "ws://hub.example.com:9000", sizeof(cfg.hub_url) - 1);
}

void saveConfig() {
    prefs.begin(NVS_NS, false);  // read-write
    prefs.putString("wifi_ssid", cfg.wifi_ssid);
    prefs.putString("wifi_pass", cfg.wifi_pass);
    prefs.putString("hub_url",   cfg.hub_url);
    prefs.putString("hub_key",   cfg.hub_key);
    prefs.putString("gw_name",   cfg.gw_name);
    prefs.putString("ble_pass",   cfg.ble_password);
    prefs.putFloat("gw_lat",     cfg.gw_lat);
    prefs.putFloat("gw_lon",     cfg.gw_lon);
    prefs.putUChar("antenna",    cfg.antenna_type);
    prefs.putFloat("gw_height",  cfg.gw_height);
    prefs.end();
    dbg("Config saved to NVS");
}

// ─── Stats ────────────────────────────────────────────────────
static uint32_t rxCount = 0;  // LoRa → hub
static uint32_t txCount = 0;  // hub → LoRa

// ─── Radio ───────────────────────────────────────────────────
// Default constructor works on all ESP32 variants (arduino-esp32 v2 + v3).
// Explicit pins are set in loraSPI.begin() so the bus number doesn't matter.
SPIClass loraSPI;

#if defined(RADIO_SX1262)
SX1262 radio = new Module(RADIO_CS, RADIO_DIO1, RADIO_RST, RADIO_BUSY, loraSPI);
#elif defined(RADIO_SX1276)
SX1276 radio = new Module(RADIO_CS, RADIO_DIO0, RADIO_RST, -1, loraSPI);
#endif

volatile bool rxFlag = false;

void IRAM_ATTR onRadioRx() { rxFlag = true; }

void radioStartListening() {
#if defined(RADIO_RXEN)
    digitalWrite(RADIO_RXEN, HIGH);
#endif
    radio.startReceive();
}

void radioTransmit(const uint8_t* pkt, size_t len) {
#if defined(RADIO_RXEN)
    digitalWrite(RADIO_RXEN, LOW);
#endif
    radio.standby();
    radio.transmit(pkt, len);
    rxFlag = false;  // Clear false RX flag from TX_DONE DIO1 interrupt
    radioStartListening();
    txCount++;
}

// Encode bytes to hex string
static String toHex(const uint8_t* data, size_t len) {
    String s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        s += buf;
    }
    return s;
}

// Decode hex string to bytes; returns byte count
static int fromHex(const String& hex, uint8_t* out, size_t maxLen) {
    int n = 0;
    for (unsigned int i = 0; i + 1 < hex.length() && (size_t)n < maxLen; i += 2) {
        char byteStr[3] = { hex[i], hex[i+1], '\0' };
        out[n++] = (uint8_t)strtol(byteStr, nullptr, 16);
    }
    return n;
}

// ─── LoRa parameters — must match MeshCore.h exactly ─────────
#ifndef LORA_FREQ
#define LORA_FREQ     868.0
#endif
#ifndef LORA_BW
#define LORA_BW       125.0
#endif
#ifndef LORA_SF
#define LORA_SF       9
#endif
#ifndef LORA_CR
#define LORA_CR       5
#endif
#ifndef LORA_SYNC
#define LORA_SYNC     0x12
#endif
#ifndef LORA_POWER
#define LORA_POWER    20
#endif
#ifndef LORA_PREAMBLE
#define LORA_PREAMBLE 8
#endif

#ifndef RADIO_TCXO_VOLTAGE
#define RADIO_TCXO_VOLTAGE 0
#endif
#ifndef RADIO_CURRENT_LIMIT
#define RADIO_CURRENT_LIMIT 140.0
#endif

void setupRadio() {
    dbg("Initializing radio...");
#if defined(RADIO_SX1262)
    int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                            LORA_SYNC, LORA_POWER, LORA_PREAMBLE, RADIO_TCXO_VOLTAGE, false);
#elif defined(RADIO_SX1276)
    int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                            LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 0);
#endif
    if (state != RADIOLIB_ERR_NONE) {
        dbgf("Radio FAILED: %d\n", state);
        if (BOARD_LED >= 0) {
            while (1) {
                digitalWrite(BOARD_LED, HIGH); delay(100);
                digitalWrite(BOARD_LED, LOW);  delay(100);
            }
        }
    }
#if defined(RADIO_SX1262)
  #if defined(RADIO_DIO2_RF_SWITCH)
    radio.setDio2AsRfSwitch(true);
  #endif
    radio.setCurrentLimit(RADIO_CURRENT_LIMIT);
    radio.setDio1Action(onRadioRx);
#elif defined(RADIO_SX1276)
    radio.setDio0Action(onRadioRx, RISING);
#endif
    radioStartListening();
    dbgf("Radio OK: %.1f MHz %ddBm\n", LORA_FREQ, LORA_POWER);
}

// ─── WebSocket ───────────────────────────────────────────────
static WebSocketsClient ws;
static bool wsConnected = false;

// Forward declaration
void wsSend(const String& json);

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_CONNECTED: {
            wsConnected = true;
            dbg("WS: connected");
            // Auth message (includes location for hub map)
            char authBuf[384];
            snprintf(authBuf, sizeof(authBuf),
                "{\"type\":\"auth\",\"key\":\"%s\",\"id\":\"%s\",\"name\":\"%s\","
                "\"lat\":%.6f,\"lon\":%.6f,\"antenna\":%d,\"height\":%.1f}",
                cfg.hub_key, WiFi.macAddress().c_str(), cfg.gw_name,
                cfg.gw_lat, cfg.gw_lon, cfg.antenna_type, cfg.gw_height);
            ws.sendTXT(authBuf);
            break;
        }

        case WStype_DISCONNECTED:
            wsConnected = false;
            dbg("WS: disconnected");
            break;

        case WStype_TEXT: {
            // Parse incoming JSON — look for type=="pkt"
            String msg = String((char*)payload);
            dbg("WS RX: " + msg);

            // Simple JSON field extraction (no full JSON library needed)
            // Expect: {"type":"pkt","data":"aabbcc..."}
            int typeIdx = msg.indexOf("\"type\"");
            if (typeIdx < 0) break;
            int tStart = msg.indexOf('"', typeIdx + 7);
            if (tStart < 0) break;
            int tEnd   = msg.indexOf('"', tStart + 1);
            if (tEnd < 0) break;
            String msgType = msg.substring(tStart + 1, tEnd);

            if (msgType == "pkt") {
                int dIdx = msg.indexOf("\"data\"");
                if (dIdx < 0) break;
                int dStart = msg.indexOf('"', dIdx + 7);
                if (dStart < 0) break;
                int dEnd   = msg.indexOf('"', dStart + 1);
                if (dEnd < 0) break;
                String hexData = msg.substring(dStart + 1, dEnd);

                if (hexData.length() < 14 || hexData.length() > 512) {
                    dbg("WS: pkt hex out of range");
                    break;
                }

                uint8_t pkt[256];
                int pktLen = fromHex(hexData, pkt, sizeof(pkt));
                if (pktLen < 7) { dbg("WS: pkt too short"); break; }

                // Dedup: don't re-transmit a packet we recently saw on air
                if (dedupSeen(pkt, pktLen)) { dbg("WS: dedup suppressed TX"); break; }

                radioTransmit(pkt, pktLen);
                dbgf("WS->LoRa: %d bytes\n", pktLen);
            }
            break;
        }

        default:
            break;
    }
}

// Helper — send JSON text if connected
// sendTXT() takes non-const String& so we copy to a local
void wsSend(const String& json) {
    if (!wsConnected) return;
    String msg = json;
    ws.sendTXT(msg);
}

// Parse hub_url into host, port, path
static bool parseHubUrl(const char* url, String& host, uint16_t& port, String& path, bool& tls) {
    // Format: ws://hostname:port/path  or  wss://hostname (port defaults to 443)
    host = "";
    port = 9000;
    path = "/";
    tls  = false;

    const char* p = url;
    if (strncmp(p, "wss://", 6) == 0) { tls = true;  p += 6; port = 443; }
    else if (strncmp(p, "ws://", 5) == 0) {            p += 5; }
    else return false;

    const char* colon = strchr(p, ':');
    const char* slash = strchr(p, '/');

    if (colon && (!slash || colon < slash)) {
        host = String(p).substring(0, colon - p);
        port = (uint16_t)atoi(colon + 1);
        if (slash) path = String(slash);
    } else if (slash) {
        host = String(p).substring(0, slash - p);
        path = String(slash);
    } else {
        host = String(p);
    }
    return true;
}

void setupWebSocket() {
    if (strlen(cfg.hub_url) == 0) { dbg("WS: no hub URL configured"); return; }

    String host;
    uint16_t port;
    String path;
    bool tls;
    if (!parseHubUrl(cfg.hub_url, host, port, path, tls)) {
        dbg("WS: invalid hub URL");
        return;
    }

    dbgf("WS: connecting to %s%s:%d%s\n", tls ? "wss://" : "ws://", host.c_str(), port, path.c_str());
    if (tls) {
#ifdef VERIFY_TLS_CERT
        // ISRG Root X1 (Let's Encrypt) — used by Tailscale Funnel and most modern hosts
        static const char caCert[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6
UA5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+s
WT8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qy
HB5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+U
CvdGI8O7gO+k1Pee3CKdJrgEOiQ9E/aP4o2fXp7t/rassSnW2DsAtl4mJkiBAtBB
piS5ua2Mz2ELgHUE/sS/eOCDfrhslGPGAjUEIorFRkHegeYb9OkB5YNQ8LMvjqWv
IjSJphYOXhNnMKGpNOYtoJc9RRrkDAzMk7u9s5b+sp0wMPG12rNgGEIS8wJB2sFI
NjNp5KVjlaFvKPmHquQE1AIGnkB/4VRafdYWQ0sv0esjg2Rdj4pC6j99J0vIQsv5
g08IG0RcVa07kSC5IMrpBPH6GRdbfzuAebMw9R+7fPM75hbiCD1yM5HPHCO8EGAn
UHjaNPDOBJmTDFBn6wxDAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMB
Af8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkqhkiG
9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZLubhz
EFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ3BEB
YLQv2lW+nQh2o1aGmF4dOg2eCTGZwDOKRIK/HGFqvj2VEjI1wJON2GhN5vlEh/O
4vGF5VIQPkIgvvwqBGDc6LKq/pyYOxME+beCOtMRNNB1dAeRiGpCcO1JMd6kHpeK
qlb6EeiHDCeXhEzGN2KlGKCblO2RTkKlB5qw7YMq1hMfkKF/kmFDNE+gTs80WNQE
YCyFjLIixpI6ta+KZlPlr1EhRxJ2FGlq0DbHqYjyFMbGQzCih03hX/95DmF1TRWQ
nbM9FdJ9JCa0lvXw+bVMneDj3BX19BO6Fr3faOe1p9M+6Isq0/pgQYH4DrmIFBer
m5YIbIqiihR8bX7JO/j3U39yolWex9bjJ87EE/NR8mH3XPXX3USsfl+u7n94BqH
T5VaKYXE/UiTgkRcaGM4YxkG8OJnCppGmIujSKHT0tXK7Am4pt/MnMj2IQGKH7BQ
kDnFt2M3yZDOQ7YXZV2CiKBHXEP+wgs31+2WBBDkxqGfnbbGfGnbgO0jPLqPofr
F1j9IEbM3LEMGu5JBWOHiTCA2IK+GZYwzLNvk6sc2j9oKH2Y9Rcuts4=
-----END CERTIFICATE-----
)EOF";
        ws.beginSslWithCA(host.c_str(), port, path.c_str(), caCert);
        dbg("WS: TLS with ISRG Root X1 cert verification");
#else
        // Insecure mode — no certificate verification
        ws.beginSSL(host, port, path);
        dbg("WS: WARNING - TLS without certificate verification");
#endif
    } else {
        ws.begin(host, port, path);
    }
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
}

// ─── WiFi ────────────────────────────────────────────────────
static unsigned long lastWifiAttempt = 0;
static bool wifiStarted = false;

void startWifi() {
    if (strlen(cfg.wifi_ssid) == 0) { dbg("WiFi: no SSID configured"); return; }
    dbgf("WiFi: connecting to %s\n", cfg.wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
    wifiStarted = true;
    lastWifiAttempt = millis();
}

void checkWifi() {
    if (!wifiStarted) return;
    static bool wasConnected = false;

    if (WiFi.status() == WL_CONNECTED) {
        if (!wasConnected) {
            wasConnected = true;
            dbg("WiFi: connected, IP=" + WiFi.localIP().toString());
            setupWebSocket();
        }
    } else {
        if (wasConnected) {
            wasConnected = false;
            wsConnected  = false;
            dbg("WiFi: lost connection");
        }
        // Try reconnect every 30s
        if (millis() - lastWifiAttempt > WIFI_RECONNECT_INTERVAL_MS) {
            lastWifiAttempt = millis();
            dbg("WiFi: retrying...");
            WiFi.reconnect();
        }
    }
}

// ─── BLE ─────────────────────────────────────────────────────
static BLEServer* bleServer = nullptr;
static BLECharacteristic* bleNotifyChar = nullptr;
static bool bleConnected = false;
static bool bleAuthenticated = false;  // Must AUTH before config commands
static String bleRxBuffer;

// Forward declaration
void processBleCommand(const String& line);

class BleServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer* s)    override { bleConnected = true;  dbg("BLE: connected"); }
    void onDisconnect(BLEServer* s) override { bleConnected = false; bleAuthenticated = false; dbg("BLE: disconnected"); s->startAdvertising(); }
};

class BleWriteCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        String data = c->getValue().c_str();
        bleRxBuffer += data;
        while (bleRxBuffer.indexOf('\n') >= 0) {
            int nl = bleRxBuffer.indexOf('\n');
            String line = bleRxBuffer.substring(0, nl);
            bleRxBuffer  = bleRxBuffer.substring(nl + 1);
            line.trim();
            if (line.length() > 0) processBleCommand(line);
        }
    }
};

void bleSend(const String& line) {
    if (!bleConnected || !bleNotifyChar) return;
    String msg = line + "\n";
    for (unsigned int i = 0; i < msg.length(); i += 20) {
        String chunk = msg.substring(i, min((unsigned int)msg.length(), i + 20));
        bleNotifyChar->setValue(chunk.c_str());
        bleNotifyChar->notify();
        if (i + 20 < msg.length()) delay(10);
    }
}

// Check if password is set and user hasn't authenticated yet
bool bleNeedsAuth() {
    return strlen(cfg.ble_password) > 0 && !bleAuthenticated;
}

void processBleCommand(const String& line) {
    dbg("BLE CMD: " + line);

    // AUTH command — always allowed
    if (line.startsWith("AUTH,")) {
        String pass = line.substring(5); pass.trim();
        if (strlen(cfg.ble_password) == 0) {
            bleAuthenticated = true;
            bleSend("OK,AUTH,no password set");
        } else if (pass == String(cfg.ble_password)) {
            bleAuthenticated = true;
            bleSend("OK,AUTH");
            dbg("BLE: authenticated");
        } else {
            bleSend("ERR,AUTH,wrong password");
            dbg("BLE: auth failed");
        }
        return;
    }

    // SETPASS — set or change the BLE password (requires existing auth if password is set)
    if (line.startsWith("SETPASS,")) {
        if (bleNeedsAuth()) { bleSend("ERR,AUTH_REQUIRED"); return; }
        String newPass = line.substring(8); newPass.trim();
        if (newPass.length() > 30) { bleSend("ERR,PASS_TOO_LONG"); return; }
        strncpy(cfg.ble_password, newPass.c_str(), sizeof(cfg.ble_password) - 1);
        cfg.ble_password[sizeof(cfg.ble_password) - 1] = '\0';
        saveConfig();
        bleAuthenticated = true;  // Stay authenticated after setting
        bleSend("OK,SETPASS");
        dbg("BLE: password updated");
        return;
    }

    // STATUS/GWSTATUS — always allowed (read-only, doesn't expose secrets)
    if (line == "GWSTATUS" || line == "STATUS") {
        const char* antNames[] = { "indoor", "external", "rooftop" };
        String s = "GWSTATUS";
        s += ",NAME:" + String(cfg.gw_name);
        s += ",SSID:" + String(cfg.wifi_ssid);
        s += ",HUB:"  + String(cfg.hub_url);
        s += ",WIFI:" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "disconnected");
        s += ",WS:"   + String(wsConnected ? "connected" : "disconnected");
        s += ",ANT:"  + String(antNames[cfg.antenna_type]);
        s += ",LAT:"  + String(cfg.gw_lat, 5);
        s += ",LON:"  + String(cfg.gw_lon, 5);
        s += ",HEIGHT:" + String(cfg.gw_height, 1);
        s += ",RX:"   + String(rxCount);
        s += ",TX:"   + String(txCount);
        s += ",HEAP:" + String(ESP.getFreeHeap());
        s += ",LOCKED:" + String(bleNeedsAuth() ? "YES" : "NO");
        bleSend(s);
        return;
    }

    // All other commands require authentication
    if (bleNeedsAuth()) {
        bleSend("ERR,AUTH_REQUIRED");
        return;
    }

    if (line.startsWith("WIFI,")) {
        // WIFI,ssid,password
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        if (c2 < 0) { bleSend("ERR,WIFI,bad format"); return; }
        String ssid = line.substring(c1 + 1, c2);
        String pass = line.substring(c2 + 1);
        ssid.trim(); pass.trim();
        strncpy(cfg.wifi_ssid, ssid.c_str(), sizeof(cfg.wifi_ssid) - 1);
        strncpy(cfg.wifi_pass, pass.c_str(), sizeof(cfg.wifi_pass) - 1);
        bleSend("OK,WIFI," + ssid);
    }
    else if (line.startsWith("HUBURL,")) {
        String url = line.substring(7); url.trim();
        strncpy(cfg.hub_url, url.c_str(), sizeof(cfg.hub_url) - 1);
        bleSend("OK,HUBURL," + url);
    }
    else if (line.startsWith("HUBKEY,")) {
        String key = line.substring(7); key.trim();
        strncpy(cfg.hub_key, key.c_str(), sizeof(cfg.hub_key) - 1);
        bleSend("OK,HUBKEY");
    }
    else if (line.startsWith("GWNAME,")) {
        String name = line.substring(7); name.trim();
        strncpy(cfg.gw_name, name.c_str(), sizeof(cfg.gw_name) - 1);
        bleSend("OK,GWNAME," + name);
    }
    else if (line.startsWith("GWLOC,")) {
        // GWLOC,lat,lon
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        if (c2 < 0) { bleSend("ERR,GWLOC,bad format"); return; }
        cfg.gw_lat = line.substring(c1 + 1, c2).toFloat();
        cfg.gw_lon = line.substring(c2 + 1).toFloat();
        bleSend("OK,GWLOC," + String(cfg.gw_lat, 5) + "," + String(cfg.gw_lon, 5));
    }
    else if (line.startsWith("GWANTENNA,")) {
        uint8_t ant = (uint8_t)line.substring(10).toInt();
        if (ant > 2) ant = 0;
        cfg.antenna_type = ant;
        const char* antNames[] = { "indoor", "external", "rooftop" };
        bleSend("OK,GWANTENNA," + String(antNames[ant]));
    }
    else if (line.startsWith("GWHEIGHT,")) {
        cfg.gw_height = constrain(line.substring(9).toFloat(), 0.0f, 500.0f);
        bleSend("OK,GWHEIGHT," + String(cfg.gw_height, 1));
    }
    else if (line == "SAVE") {
        saveConfig();
        bleSend("OK,SAVED,rebooting");
        delay(500);
        ESP.restart();
    }
    else {
        bleSend("ERR,UNKNOWN_CMD");
    }
}

void setupBLE() {
    String bleName = "LoRaGW-" + String(cfg.gw_name).substring(0, 10);
    BLEDevice::init(bleName.c_str());
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new BleServerCB());

    BLEService* service = bleServer->createService(BLE_SERVICE_UUID);

    // Write characteristic (phone writes commands here)
    BLECharacteristic* writeChar = service->createCharacteristic(
        BLE_TX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    writeChar->setCallbacks(new BleWriteCB());

    // Notify characteristic (gateway replies via this)
    bleNotifyChar = service->createCharacteristic(
        BLE_RX_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    bleNotifyChar->addDescriptor(new BLE2902());

    service->start();
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    BLEDevice::startAdvertising();
    dbg("BLE ready: " + bleName);
}

// ─── OLED Display ─────────────────────────────────────────────
#if HAS_OLED
static unsigned long lastOledRefresh = 0;

void updateOLED() {
    if (!oledAvailable) return;
    if (millis() - lastOledRefresh < OLED_REFRESH_INTERVAL_MS) return;
    lastOledRefresh = millis();

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    // Line 0: gateway name (truncated to 16 chars)
    oled.setCursor(0, 0);
    String nameLine = String(cfg.gw_name);
    if (nameLine.length() > 16) nameLine = nameLine.substring(0, 16);
    oled.print("GW: "); oled.println(nameLine);
    oled.drawLine(0, 10, OLED_W, 10, SSD1306_WHITE);

    // Line 1: WiFi
    oled.setCursor(0, 14);
    if (WiFi.status() == WL_CONNECTED) {
        oled.print("WiFi: ");
        oled.print(WiFi.localIP().toString());
    } else {
        oled.print("WiFi: connecting...");
    }

    // Line 2: Hub WebSocket
    oled.setCursor(0, 24);
    oled.print("Hub: ");
    oled.print(wsConnected ? "connected" : "offline");

    // Line 3: Packet counts
    oled.setCursor(0, 34);
    oled.print("RX:"); oled.print(rxCount);
    oled.setCursor(64, 34);
    oled.print("TX:"); oled.print(txCount);

    // Line 4: Antenna type
    oled.setCursor(0, 44);
    const char* antNames[] = { "Indoor", "External", "Rooftop" };
    oled.print("Ant: "); oled.print(antNames[cfg.antenna_type]);

    // Line 5: Uptime
    oled.setCursor(0, 54);
    unsigned long up = millis() / 1000;
    oled.print("Up:");
    if (up > 3600) { oled.print(up / 3600); oled.print("h"); }
    oled.print((up % 3600) / 60); oled.print("m");

    oled.display();
}
#endif // HAS_OLED

// ─── Radio receive check ──────────────────────────────────────
void checkRadioRx() {
    if (!rxFlag) return;
    rxFlag = false;

    uint8_t pkt[256];
    size_t len = radio.getPacketLength();
    if (len < 7 || len > sizeof(pkt)) {
        radioStartListening();
        return;
    }

    int state = radio.readData(pkt, len);
    radioStartListening();
    if (state != RADIOLIB_ERR_NONE) return;

    // Dedup check
    if (dedupSeen(pkt, len)) {
        dbg("LoRa: dedup suppressed");
        return;
    }

    rxCount++;
    String hexStr = toHex(pkt, len);
    float rssi = radio.getRSSI();

    dbgf("LoRa RX: %d bytes, RSSI=%.1f\n", (int)len, rssi);

    // Build JSON and forward to hub
    // {"type":"pkt","data":"<hex>","rssi":-85,"origin":"My Gateway"}
    char jsonBuf[640];
    snprintf(jsonBuf, sizeof(jsonBuf),
        "{\"type\":\"pkt\",\"data\":\"%s\",\"rssi\":%.1f,\"origin\":\"%s\"}",
        hexStr.c_str(), rssi, cfg.gw_name);

    wsSend(String(jsonBuf));
}

// ─── Setup ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n═══════════════════════════════════");
    Serial.println("  TiggyOpenMesh WiFi Gateway v1.1");
    Serial.println("  Board: " BOARD_NAME);
    Serial.println("═══════════════════════════════════");

    // LEDs & buttons
    if (BOARD_LED >= 0) { pinMode(BOARD_LED, OUTPUT); digitalWrite(BOARD_LED, LOW); }
#if defined(BOARD_BUTTON) && BOARD_BUTTON >= 0
    pinMode(BOARD_BUTTON, INPUT_PULLUP);
#endif

    // VEXT (Heltec OLED power)
#ifdef VEXT_CTRL
    pinMode(VEXT_CTRL, OUTPUT);
    digitalWrite(VEXT_CTRL, LOW);   // LOW = VEXT on
    delay(50);
#endif

    // RXEN (XIAO Wio-SX1262 RF switch)
#ifdef RADIO_RXEN
    pinMode(RADIO_RXEN, OUTPUT);
    digitalWrite(RADIO_RXEN, LOW);
#endif

    // FEM (GC1109 on Heltec V4)
#ifdef RADIO_FEM_EN
    pinMode(RADIO_FEM_EN, OUTPUT);
    digitalWrite(RADIO_FEM_EN, HIGH);
#endif
#ifdef RADIO_FEM_TXEN
    pinMode(RADIO_FEM_TXEN, OUTPUT);
    digitalWrite(RADIO_FEM_TXEN, HIGH);
#endif

    // Load config from NVS
    loadConfig();

    // BLE provisioning
    setupBLE();

    // SPI + Radio
    loraSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, RADIO_CS);
    setupRadio();

    // OLED
#if HAS_OLED
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        oledAvailable = true;
        oled.clearDisplay();
        oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);  oled.println("LoRa WiFi Gateway");
        oled.println(cfg.gw_name);
        oled.println("WiFi: " + String(cfg.wifi_ssid));
        oled.println("Hub: connecting...");
        oled.display();
    }
#endif

    // WiFi
    startWifi();

    // Startup blink
    if (BOARD_LED >= 0) {
        for (int i = 0; i < 3; i++) {
            digitalWrite(BOARD_LED, HIGH); delay(100);
            digitalWrite(BOARD_LED, LOW);  delay(100);
        }
    }

    Serial.println("Ready. Connect via BLE to configure.");
    Serial.println("BLE: AUTH,pass | WIFI,ssid,pass | HUBURL,wss://host | GWNAME,name");
    Serial.println("     GWLOC,lat,lon | GWANTENNA,0-2 | GWHEIGHT,metres | SAVE");
    if (strlen(cfg.ble_password) > 0) Serial.println("BLE password protection: ENABLED");
    else Serial.println("BLE password protection: DISABLED (use SETPASS,yourpassword to enable)");
    dbgf("Free heap: %u\n", ESP.getFreeHeap());
}

// ─── Loop ─────────────────────────────────────────────────────
void loop() {
    // 1. WiFi health check + reconnect
    checkWifi();

    // 2. WebSocket loop (keepalive, reconnect, receive)
    if (WiFi.status() == WL_CONNECTED) {
        ws.loop();
    }

    // 3. Radio RX → hub
    checkRadioRx();

    // 4. OLED refresh
#if HAS_OLED
    updateOLED();
#endif

    // 5. Serial config — same commands as BLE, useful via web flasher terminal
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n'); line.trim();
        if (line.length() == 0) return;
        if (line == "STATUS") {
            Serial.println("═══ WiFi Gateway Status ═══");
            Serial.println("Name:    " + String(cfg.gw_name));
            Serial.println("SSID:    " + String(cfg.wifi_ssid));
            Serial.println("Hub:     " + String(cfg.hub_url));
            Serial.println("WiFi:    " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "disconnected"));
            Serial.println("WS:      " + String(wsConnected ? "connected" : "disconnected"));
            Serial.println("Ant:     " + String((const char*[]){"indoor","external","rooftop"}[cfg.antenna_type]));
            Serial.println("RX:      " + String(rxCount));
            Serial.println("TX:      " + String(txCount));
            Serial.println("BLE PW:  " + String(strlen(cfg.ble_password) > 0 ? "SET" : "NONE"));
            Serial.println("Heap:    " + String(ESP.getFreeHeap()));
        } else if (line.startsWith("SETPASS,")) {
            // Serial can always set password (physical access = trusted)
            String newPass = line.substring(8); newPass.trim();
            if (newPass.length() > 30) { Serial.println("ERROR: Password too long (max 30)"); }
            else {
                strncpy(cfg.ble_password, newPass.c_str(), sizeof(cfg.ble_password) - 1);
                cfg.ble_password[sizeof(cfg.ble_password) - 1] = '\0';
                saveConfig();
                Serial.println("OK: BLE password " + String(newPass.length() > 0 ? "set" : "removed"));
            }
        } else if (line.startsWith("WIFI,")     || line.startsWith("HUBURL,") ||
                   line.startsWith("HUBKEY,")   || line.startsWith("GWNAME,") ||
                   line.startsWith("GWLOC,")    || line.startsWith("GWANTENNA,") ||
                   line.startsWith("GWHEIGHT,") ||
                   line == "SAVE"               || line == "GWSTATUS") {
            // Serial is trusted (physical access) — bypass BLE auth
            bleAuthenticated = true;
            processBleCommand(line);
            Serial.println("OK");
        } else {
            Serial.println("Commands: STATUS | WIFI,ssid,pass | HUBURL,wss://host | HUBKEY,secret | GWNAME,name");
            Serial.println("          GWLOC,lat,lon | GWANTENNA,0-2 | GWHEIGHT,metres | SETPASS,password | SAVE");
        }
    }

    yield();
}

#endif // GATEWAY_MODE
