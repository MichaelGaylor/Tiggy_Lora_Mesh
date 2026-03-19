// ═══════════════════════════════════════════════════════════════
// TiggyOpenMesh v3.1 - T-Deck Plus Edition
// ═══════════════════════════════════════════════════════════════
//
// Hardware: LilyGO T-Deck Plus (ESP32-S3 + SX1262 + GPS + Keyboard)
// Display:  ST7789 320x240 2.8" IPS (landscape)
// Radio:    SX1262 via RadioLib (interrupt-driven RX)
// Input:    Physical QWERTY keyboard + Trackball
// GPS:      MIA-M10Q GNSS
//
// Uses MeshCore shared library for all mesh protocol logic.
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <EEPROM.h>
#include <RadioLib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TinyGPSPlus.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <map>
#include <vector>
#include <deque>
#include "Pins.h"
#include "phrases.h"
#include "MeshCore.h"

// ─── Theme Colors ───────────────────────────────────────────
#define COL_BG          0x0000
#define COL_PANEL       0x10A2
#define COL_HEADER      0x0926
#define COL_ACCENT      0x07FF
#define COL_TEXT         0xFFFF
#define COL_DIM         0x8410
#define COL_FAINT       0x4208
#define COL_GOOD        0x07E0
#define COL_WARN        0xFD20
#define COL_BAD         0xF800
#define COL_BUBBLE_IN   0x1126
#define COL_BUBBLE_OUT  0x0320
#define COL_BUBBLE_SOS  0x8000
#define COL_SELECTED    0x2104
#define COL_CURSOR      0xFFE0

// ─── T-Deck specific constants ──────────────────────────────
#define GPS_INTERVAL    60000UL
#define SOS_INTERVAL    120000UL
#define BAR_UPDATE_MS   10000UL

#define MSG_HISTORY     20
#define MAX_CHUNKS      5
#define MAX_PARTS       5
#define MAX_INPUT_LEN   160
#define CHUNK_PLAINTEXT 70

#define SCREEN_W        320
#define SCREEN_H        240
#define HEADER_H        28
#define STATUS_H        18
#define CONTENT_Y       (HEADER_H + 2)
#define CONTENT_H       (SCREEN_H - HEADER_H - STATUS_H - 4)

#define DEBUG 1
#if DEBUG
  #define debugPrint(x) Serial.println(x)
#else
  #define debugPrint(x)
#endif

// ─── Hardware Objects ───────────────────────────────────────
SPIClass sharedSPI(FSPI);
Adafruit_ST7789 display(&sharedSPI, BOARD_TFT_CS, BOARD_TFT_DC, -1);
SX1262 radio = new Module(RADIO_CS, RADIO_DIO1, RADIO_RST, RADIO_BUSY, sharedSPI);
HardwareSerial GPSSerial(1);
TinyGPSPlus gps;

// ─── Data Structures (T-Deck specific) ──────────────────────
struct NodeInfo {
  String id;
  int rssi;
  unsigned long lastSeen;
  std::vector<String> neighbors;
};

struct ChunkBuffer {
  String baseMid;
  int totalParts;
  String parts[MAX_PARTS];
  unsigned long lastUpdate;
  bool active;
};

struct ChatMessage {
  String from;
  String text;
  int rssi;
  bool outgoing;
  unsigned long timestamp;
};

// ─── Global State (T-Deck specific) ────────────────────────
// Note: localID, knownNodes, knownCount, aes_key_string, routingTable
// are now in mesh.* (MeshCore global instance)
char targetID[NODE_ID_LEN + 1] = "FFFF";

std::map<String, NodeInfo> meshMap;

// Chat history (ring buffer)
ChatMessage chatHistory[MSG_HISTORY];
uint8_t chatHead = 0;
uint8_t chatCount = 0;

ChunkBuffer chunkBuffers[MAX_CHUNKS];

// ACK tracking
String pendingAckID;
bool ackReceived = false;
uint8_t ackAttempt = 0;
unsigned long ackSentAt = 0;
String ackPayloadCache;

// Position
struct NodePos { double lat, lon; bool valid; };
NodePos nodePos[MAX_NODES] = {};

// Timers
unsigned long nextHeartbeatTime = 0;
unsigned long lastGpsSend = 0;
unsigned long lastSosSent = 0;
unsigned long lastBarUpdate = 0;
unsigned long lastRouteClean = 0;
unsigned long trackUpdateTimer = 0;
unsigned long lastDisplayRefresh = 0;
unsigned long lastNotification = 0;

// Notification overlay
String notifyText;
unsigned long notifyUntil = 0;

// UI state
enum Mode { MENU, MESSAGING, TRACKING, SOS_MODE, MESHVIEW, TEXT_INPUT, FIRST_BOOT };
enum MenuState { M_MAIN, M_SETTINGS };
Mode currentMode = MENU;
MenuState menuState = M_MAIN;
int menuIndex = 0;
int menuScroll = 0;
int chatScroll = 0;
uint8_t brightness = 255;

// Display sleep
unsigned long lastInputTime = 0;
uint8_t displaySleepTimeout = 60;  // seconds, 0=disabled
bool displayAsleep = false;
#define EEPROM_SLEEP_ADDR 432

void wakeDisplay() {
    if (displayAsleep) {
        analogWrite(BOARD_TFT_BL, brightness);
        displayAsleep = false;
    }
    lastInputTime = millis();
}

// Text input
String inputBuffer;
String inputPrompt;
bool inputActive = false;
enum InputTarget { INPUT_MSG, INPUT_LOCAL_ID, INPUT_TARGET_ID, INPUT_AES_KEY };
InputTarget inputTarget;

// Last sender for quick reply
String lastSender;

// Trackball
volatile int tbDeltaUp = 0, tbDeltaDown = 0;
volatile int tbDeltaLeft = 0, tbDeltaRight = 0;

// First boot wizard
int wizardStep = 0;
bool wizardScanActive = false;
bool wizardScanDone = false;
unsigned long wizardScanEnd = 0;

// Menu definitions
const char* mainMenu[] = {
  "New Message",
  "Quick Phrases",
  "View Messages",
  "Remote Control",
  "SOS Emergency",
  "Settings",
  "Known Nodes",
  "Mesh Map",
  "Broadcast Pos",
  "Find Node"
};
const uint8_t mainCount = sizeof(mainMenu) / sizeof(mainMenu[0]);

const char* settingsMenu[] = {
  "Set Local ID",
  "Set Target ID",
  "Edit AES Key",
  "Brightness",
  "Display Timeout",
  "Clear Nodes",
  "Back"
};
const uint8_t settingsCount = sizeof(settingsMenu) / sizeof(settingsMenu[0]);

const uint16_t mainMenuColors[] = {
  COL_GOOD, COL_ACCENT, COL_ACCENT, COL_WARN,
  COL_BAD, COL_DIM, COL_WARN, COL_WARN, COL_GOOD, COL_ACCENT
};

// ─── Forward Declarations ───────────────────────────────────
void drawCurrentMenu();
void drawChatView();
void drawStatusBar();
void drawHeaderBar(const String& title);
void drawSignalBars(int x, int y, int rssi);
void showNotification(const String& text, unsigned long durationMs = 2000);
void receiveCheck();
void saveIDsToEEPROM();
void saveKeyToEEPROM();
void addChatMessage(const String& from, const String& text, int rssi, bool outgoing);
void handleIncomingCmd(const String& from, const String& cmdBody);
void sendCmdResponse(const String& to, const String& rsp);
void sendRemoteCmd(const String& target, const String& cmd);
void drawCtrlMenu(const char* items[], int count, int sel);
void showControlPanel();
void viewKnownNodes();
void broadcastPosition();

// ═══════════════════════════════════════════════════════════════
// SECTION: Radio Bridge (connects MeshCore to SX1262 hardware)
// ═══════════════════════════════════════════════════════════════

void IRAM_ATTR onRadioRx() { mesh.rxFlag = true; }

void radioTransmit(uint8_t* pkt, size_t len) {
  radio.standby();
  radio.transmit(pkt, len);
  mesh.rxFlag = false;  // Clear false RX flag from TX_DONE DIO1 interrupt
  radio.startReceive();
}

bool radioChannelFree() {
  float rssi = radio.getRSSI();
  return (rssi < -120.0);
}

void setupRadio() {
  debugPrint("Initializing SX1262...");
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                          LORA_SYNC, LORA_POWER, LORA_PREAMBLE, RADIO_TCXO_VOLTAGE, false);
  if (state != RADIOLIB_ERR_NONE) {
    drawHeaderBar("RADIO ERROR");
    display.setFont(&FreeSans12pt7b);
    display.setTextColor(COL_BAD);
    display.setCursor(20, 80);
    display.print("SX1262 FAILED");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 110);
    display.print("Error code: " + String(state));
    display.setCursor(20, 140);
    display.setTextColor(COL_DIM);
    display.print("Check wiring and antenna");
    while (1) delay(1000);
  }
  radio.setDio1Action(onRadioRx);
  radio.startReceive();
  debugPrint("Radio OK: " + String(LORA_FREQ, 1) + " MHz");
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Mesh Map (T-Deck specific visualization data)
// ═══════════════════════════════════════════════════════════════

void updateMeshMap(const String& from, const String& to, int rssi) {
  NodeInfo& node = meshMap[from];
  node.id = from;
  node.rssi = rssi;
  node.lastSeen = millis();
  if (std::find(node.neighbors.begin(), node.neighbors.end(), to) == node.neighbors.end())
    node.neighbors.push_back(to);
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Phrase Compression
// ═══════════════════════════════════════════════════════════════

String decompressQuickMsg(const String& input) {
  if (!input.startsWith("$")) return input;
  String out;
  int start = 0, end;
  while ((end = input.indexOf('|', start)) != -1) {
    String part = input.substring(start, end);
    if (part.startsWith("$")) {
      int id = strtol(part.substring(1).c_str(), nullptr, 16);
      if (id >= 0 && id < phraseCount) {
        if (out.length()) out += " ";
        out += phraseLibrary[id];
      }
    } else {
      if (out.length()) out += " ";
      out += part;
    }
    start = end + 1;
  }
  String last = input.substring(start);
  if (last.startsWith("$")) {
    int id = strtol(last.substring(1).c_str(), nullptr, 16);
    if (id >= 0 && id < phraseCount) {
      if (out.length()) out += " ";
      out += String(phraseLibrary[id]);
    }
  } else {
    if (out.length()) out += " ";
    out += last;
  }
  return out;
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Chat History
// ═══════════════════════════════════════════════════════════════

void addChatMessage(const String& from, const String& text, int rssi, bool outgoing) {
  chatHistory[chatHead] = { from, text, rssi, outgoing, millis() };
  chatHead = (chatHead + 1) % MSG_HISTORY;
  if (chatCount < MSG_HISTORY) chatCount++;
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Send (uses MeshCore for crypto & transmission)
// ═══════════════════════════════════════════════════════════════

void sendAck(const String& to, const String& mid) {
  String body = "ACK," + to + "," + mid;
  mesh.transmitPacket(strtol(to.c_str(), nullptr, 16), body);
}

void sendMeshMessage(const String& payload) {
  if (payload.length() < 4) return;

  if (payload.startsWith("HB,")) {
    mesh.transmitPacket(0xFFFF, payload);
    return;
  }

  String mid = mesh.generateMsgID();
  String hex = mesh.encryptMsg(payload);
  if (hex.length() == 0) { showNotification("Encrypt failed!"); return; }

  bool noAck = payload.startsWith("POS,") || payload.startsWith("SOS,")
            || String(targetID) == "FFFF";

  String route = String(mesh.localID);
  String fullPayload = String(mesh.localID) + "," + String(targetID) + "," + mid + ","
                      + String((int)TTL_DEFAULT) + "," + route + "," + hex;

  uint16_t dest = strtol(targetID, nullptr, 16);

  if (noAck) {
    mesh.transmitPacket(dest, fullPayload);
    return;
  }

  // ACK-tracked send
  ackReceived = false;
  pendingAckID = mid;
  ackAttempt = 0;
  ackSentAt = millis();
  ackPayloadCache = fullPayload;
  mesh.transmitPacket(dest, fullPayload);
}

void handleAckRetry() {
  if (pendingAckID.length() == 0) return;
  if (ackReceived) {
    showNotification("Delivered!");
    pendingAckID = "";
    return;
  }
  if (millis() - ackSentAt < ACK_TIMEOUT) return;

  ackAttempt++;
  if (ackAttempt > MAX_RETRIES) {
    showNotification("Send failed - no ACK");
    pendingAckID = "";
    return;
  }
  mesh.transmitPacket(strtol(targetID, nullptr, 16), ackPayloadCache);
  ackSentAt = millis();
}

// Chunked send for long messages
void sendSmartMessage(const String& baseMsg) {
  std::vector<String> words;
  int start = 0, end;
  while ((end = baseMsg.indexOf('|', start)) != -1) {
    words.push_back(baseMsg.substring(start, end));
    start = end + 1;
  }
  words.push_back(baseMsg.substring(start));

  std::vector<String> chunks;
  String current;
  for (const String& word : words) {
    String tryAdd = current.length() ? current + "|" + word : word;
    if ((int)tryAdd.length() <= CHUNK_PLAINTEXT) {
      current = tryAdd;
    } else {
      if (current.length()) chunks.push_back(current);
      current = word;
    }
  }
  if (current.length()) chunks.push_back(current);

  if (chunks.size() == 1) {
    sendMeshMessage("MSG," + chunks[0]);
    return;
  }

  String baseMid = mesh.generateMsgID();
  showNotification("Sending " + String(chunks.size()) + " parts...");

  for (size_t i = 0; i < chunks.size(); i++) {
    String chunkMid = baseMid + String((char)('A' + i));
    String header = "#" + String(i + 1) + "/" + String(chunks.size()) + "|";
    String payload = "MSG," + header + chunks[i];

    String hex = mesh.encryptMsg(payload);
    if (hex.length() == 0) continue;

    String route = String(mesh.localID);
    String fullPayload = String(mesh.localID) + "," + String(targetID) + "," + chunkMid + ","
                        + String((int)TTL_DEFAULT) + "," + route + "," + hex;

    ackReceived = false;
    pendingAckID = chunkMid;
    uint16_t dest = strtol(targetID, nullptr, 16);

    for (int attempt = 0; attempt <= MAX_RETRIES && !ackReceived; attempt++) {
      mesh.transmitPacket(dest, fullPayload);
      unsigned long waitStart = millis();
      while (!ackReceived && millis() - waitStart < ACK_TIMEOUT) {
        receiveCheck();
        yield();
      }
    }

    if (!ackReceived) {
      showNotification("Part " + String(i + 1) + " failed!");
      pendingAckID = "";
      return;
    }
  }
  pendingAckID = "";
  showNotification("All " + String(chunks.size()) + " parts sent!");
}

// ═══════════════════════════════════════════════════════════════
// SECTION: MeshCore Callbacks
// ═══════════════════════════════════════════════════════════════

// Called by MeshCore when a decrypted message arrives for us
void handleMessage(const String& from, const String& plain, int rssi) {
  uint16_t myAddr = strtol(mesh.localID, nullptr, 16);

  // MSG
  if (plain.startsWith("MSG,")) {
    String actual = plain.substring(4);

    if (actual.startsWith("#")) {
      // Chunked message
      int slash = actual.indexOf('/');
      int pipe  = actual.indexOf('|', slash);
      if (slash < 0 || pipe < 0) return;

      int part  = actual.substring(1, slash).toInt();
      int total = actual.substring(slash + 1, pipe).toInt();
      String chunk = actual.substring(pipe + 1);

      int bi = -1;
      // We don't have mid here directly, but MeshCore already sent ACK
      // Use a hash of from+part+total as baseMid proxy
      String baseMid = from + String(total);
      for (int i = 0; i < MAX_CHUNKS; i++) {
        if (chunkBuffers[i].active && chunkBuffers[i].baseMid == baseMid) { bi = i; break; }
      }
      if (bi == -1) {
        for (int i = 0; i < MAX_CHUNKS; i++) {
          if (!chunkBuffers[i].active) {
            bi = i;
            chunkBuffers[i].baseMid = baseMid;
            chunkBuffers[i].totalParts = total;
            chunkBuffers[i].active = true;
            for (int j = 0; j < MAX_PARTS; j++) chunkBuffers[i].parts[j] = "";
            break;
          }
        }
      }
      if (bi == -1) return;

      ChunkBuffer& buf = chunkBuffers[bi];
      if (part >= 1 && part <= MAX_PARTS) buf.parts[part - 1] = chunk;
      buf.lastUpdate = millis();

      showNotification("Part " + String(part) + "/" + String(total) + " from " + from);

      bool complete = true;
      for (int i = 0; i < buf.totalParts && i < MAX_PARTS; i++) {
        if (buf.parts[i].length() == 0) { complete = false; break; }
      }
      if (!complete) return;

      // Reassemble
      String finalMsg;
      for (int i = 0; i < buf.totalParts; i++) {
        if (i > 0) finalMsg += "|";
        finalMsg += buf.parts[i];
      }
      String displayText = decompressQuickMsg(finalMsg);
      addChatMessage(from, displayText, rssi, false);
      lastSender = from;
      buf.active = false;
      if (currentMode == MESSAGING) drawChatView();

    } else {
      // Single message
      String displayText = decompressQuickMsg(actual);
      addChatMessage(from, displayText, rssi, false);
      lastSender = from;
      if (currentMode == MESSAGING) drawChatView();
      else showNotification(from + ": " + displayText);
    }
  }
  else if (plain.startsWith("POS,")) {
    int c1 = plain.indexOf(',');
    int c2 = plain.indexOf(',', c1 + 1);
    if (c1 >= 0 && c2 > c1) {
      double lat = plain.substring(c1 + 1, c2).toDouble();
      double lon = plain.substring(c2 + 1).toDouble();
      int idx = -1;
      for (uint8_t i = 0; i < mesh.knownCount; i++) {
        if (String(mesh.knownNodes[i]) == from) { idx = i; break; }
      }
      if (idx == -1) { mesh.addNode(from); idx = mesh.knownCount - 1; }
      if (idx >= 0 && idx < MAX_NODES) nodePos[idx] = { lat, lon, true };
    }
  }
  else if (plain.startsWith("SOS,")) {
    addChatMessage(from, "!! SOS EMERGENCY !!", rssi, false);
    for (int i = 0; i < 4; i++) {
      display.fillScreen(COL_BAD); delay(150);
      display.fillScreen(COL_BG); delay(150);
    }
    showNotification("SOS from " + from + "!");
  }
  else if (plain.startsWith("CMD,")) {
    // CMD that came through onMessage (shouldn't normally, but handle gracefully)
    handleIncomingCmd(from, plain.substring(4));
  }

  updateMeshMap(from, String(mesh.localID), rssi);
}

// Called by MeshCore when a CMD arrives
void handleCmd(const String& from, const String& cmdBody) {
  handleIncomingCmd(from, cmdBody);
  updateMeshMap(from, String(mesh.localID), mesh.lastRSSI);
}

// Called by MeshCore when an ACK arrives
void handleAck(const String& from, const String& mid) {
  if (mid == pendingAckID) {
    ackReceived = true;
  }
}

// Called by MeshCore when a new node is discovered
void handleNodeDiscovered(const String& id, int rssi) {
  saveIDsToEEPROM();
  showNotification("New node: " + id);
  updateMeshMap(id, String(mesh.localID), rssi);
}

// Called by MeshCore when a heartbeat with our own ID is detected
void handleIdConflict(const String& id, int rssi) {
  showNotification("ID CONFLICT! " + id + " in use!", 10000);
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Receive
// ═══════════════════════════════════════════════════════════════

void receiveCheck() {
  if (!mesh.rxFlag) return;
  mesh.rxFlag = false;

  uint8_t pkt[256];
  size_t len = radio.getPacketLength();
  if (len < 7 || len > sizeof(pkt)) { radio.startReceive(); return; }

  int state = radio.readData(pkt, len);
  mesh.lastRSSI = radio.getRSSI();
  radio.startReceive();

  if (state != RADIOLIB_ERR_NONE) return;

  MeshPacket parsed;
  if (mesh.parseRawPacket(pkt, len, parsed)) {
    mesh.processPacket(parsed);
  }
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Battery
// ═══════════════════════════════════════════════════════════════

int batteryPercent() {
  int raw = analogRead(BOARD_BAT_ADC);
  float v = (raw / 4095.0) * 3.3 * 2.0;
  return constrain((int)((v - 3.0) / 1.2 * 100.0), 0, 100);
}

void drawBatteryIcon(int x, int y, int pct) {
  display.drawRect(x, y, 18, 9, COL_TEXT);
  display.fillRect(x + 18, y + 2, 2, 5, COL_TEXT);
  int fillW = map(pct, 0, 100, 0, 16);
  uint16_t col = (pct > 50) ? COL_GOOD : (pct > 20) ? COL_WARN : COL_BAD;
  if (fillW > 0) display.fillRect(x + 1, y + 1, fillW, 7, col);
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Display & UI
// ═══════════════════════════════════════════════════════════════

void drawHeaderBar(const String& title) {
  display.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HEADER);
  display.setFont(&FreeSans9pt7b);
  display.setTextSize(1);
  display.setTextColor(COL_ACCENT, COL_HEADER);
  display.setCursor(8, 19);
  display.print(title);
}

void drawStatusBar() {
  int y = SCREEN_H - STATUS_H;
  display.fillRect(0, y, SCREEN_W, STATUS_H, COL_PANEL);
  display.drawLine(0, y, SCREEN_W, y, COL_FAINT);

  display.setFont(nullptr);
  display.setTextSize(1);

  display.fillCircle(6, y + 9, 3, COL_GOOD);
  display.setTextColor(COL_TEXT, COL_PANEL);
  display.setCursor(12, y + 5);
  display.print(mesh.localID);

  display.setTextColor(COL_DIM, COL_PANEL);
  display.setCursor(50, y + 5);
  display.print(">");
  display.setTextColor(COL_ACCENT, COL_PANEL);
  display.setCursor(58, y + 5);
  display.print(targetID);

  display.setCursor(106, y + 5);
  if (gps.location.isValid()) {
    display.fillCircle(104, y + 9, 3, COL_GOOD);
    display.setCursor(110, y + 5);
    display.setTextColor(COL_GOOD, COL_PANEL);
    display.print("GPS");
  } else {
    display.fillCircle(104, y + 9, 3, COL_BAD);
    display.setCursor(110, y + 5);
    display.setTextColor(COL_BAD, COL_PANEL);
    display.print("GPS");
  }

  display.setTextColor(COL_DIM, COL_PANEL);
  display.setCursor(140, y + 5);
  display.print("N:" + String(mesh.knownCount));

  if (pendingAckID.length() > 0) {
    display.setTextColor(COL_WARN, COL_PANEL);
    display.setCursor(175, y + 5);
    display.print("TX..");
  }

  String tk = String(targetID);
  auto it = mesh.routingTable.find(tk);
  if (it != mesh.routingTable.end()) {
    drawSignalBars(210, y + 14, it->second.rssi);
  }

  drawBatteryIcon(260, y + 4, batteryPercent());

  display.setTextColor(COL_DIM, COL_PANEL);
  display.setCursor(282, y + 5);
  display.print(String(batteryPercent()) + "%");
}

void drawSignalBars(int x, int y, int rssi) {
  int bars = constrain((int)(((float)(rssi + 120) / 80.0f) * 5), 0, 5);
  for (int i = 0; i < 5; i++) {
    int h = 3 * (i + 1);
    int bx = x + i * 5;
    int by = y - h;
    if (i < bars) {
      uint16_t c = (i < 2) ? COL_BAD : (i < 4) ? COL_WARN : COL_GOOD;
      display.fillRect(bx, by, 3, h, c);
    } else {
      display.drawRect(bx, by, 3, h, COL_FAINT);
    }
  }
}

void showNotification(const String& text, unsigned long durationMs) {
  notifyText = text;
  notifyUntil = millis() + durationMs;
}

void drawNotification() {
  if (millis() > notifyUntil) return;
  int boxW = min((int)notifyText.length() * 7 + 20, SCREEN_W - 20);
  int boxX = (SCREEN_W - boxW) / 2;
  int boxY = HEADER_H + 4;
  display.fillRoundRect(boxX, boxY, boxW, 24, 6, COL_HEADER);
  display.drawRoundRect(boxX, boxY, boxW, 24, 6, COL_ACCENT);
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_TEXT, COL_HEADER);
  display.setCursor(boxX + 8, boxY + 8);
  display.print(notifyText);
}

void drawInfoScreen(const String& title, const String& body) {
  display.fillScreen(COL_BG);
  drawHeaderBar(title);
  display.setFont(&FreeSans9pt7b);
  display.setTextColor(COL_TEXT, COL_BG);
  int y = CONTENT_Y + 20;
  int i = 0;
  while (i < (int)body.length()) {
    int nl = body.indexOf('\n', i);
    if (nl == -1) nl = body.length();
    display.setCursor(12, y);
    display.print(body.substring(i, nl));
    i = nl + 1;
    y += 22;
  }
  drawStatusBar();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Chat View
// ═══════════════════════════════════════════════════════════════

void drawChatView() {
  display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - STATUS_H, COL_BG);
  drawHeaderBar("Messages  >  " + String(targetID));

  display.setFont(nullptr);
  display.setTextSize(1);

  int y = SCREEN_H - STATUS_H - 6;
  int lineH = 28;
  int bubblePad = 6;
  int maxBubbleW = SCREEN_W - 60;

  int count = min((int)chatCount, (int)MSG_HISTORY);
  int startIdx = (chatHead - count + MSG_HISTORY) % MSG_HISTORY;

  int visibleCount = (SCREEN_H - HEADER_H - STATUS_H - 10) / lineH;
  int drawFrom = max(0, count - visibleCount - chatScroll);
  int drawTo = max(0, count - chatScroll);

  y = CONTENT_Y + 4;
  for (int n = drawFrom; n < drawTo; n++) {
    if (y + lineH > SCREEN_H - STATUS_H - 2) break;
    int idx = (startIdx + n) % MSG_HISTORY;
    ChatMessage& msg = chatHistory[idx];

    String txt = msg.text;
    int maxChars = (maxBubbleW - bubblePad * 2) / 6;
    if ((int)txt.length() > maxChars) txt = txt.substring(0, maxChars - 3) + "...";

    int bubbleW = txt.length() * 6 + bubblePad * 2 + 2;
    bubbleW = min(bubbleW, maxBubbleW);

    if (msg.outgoing) {
      int bx = SCREEN_W - bubbleW - 8;
      display.fillRoundRect(bx, y, bubbleW, lineH - 4, 4, COL_BUBBLE_OUT);
      display.setTextColor(COL_TEXT, COL_BUBBLE_OUT);
      display.setCursor(bx + bubblePad, y + 8);
      display.print(txt);
      display.setTextColor(COL_DIM, COL_BG);
      display.setCursor(bx - 24, y + 8);
      display.print("You");
    } else {
      int bx = 8;
      display.fillRoundRect(bx, y, bubbleW, lineH - 4, 4, COL_BUBBLE_IN);
      display.setTextColor(COL_TEXT, COL_BUBBLE_IN);
      display.setCursor(bx + bubblePad, y + 8);
      display.print(txt);
      display.setTextColor(COL_ACCENT, COL_BG);
      display.setCursor(bx + bubbleW + 4, y + 4);
      display.print(msg.from);
      if (msg.rssi != 0) drawSignalBars(bx + bubbleW + 4, y + 22, msg.rssi);
    }
    y += lineH;
  }

  display.setTextColor(COL_FAINT, COL_BG);
  display.setCursor(8, SCREEN_H - STATUS_H - 4);
  if (lastSender.length()) {
    display.print("[r] Reply  [n] New  [Esc] Back  [Up/Dn] Scroll");
  } else {
    display.print("[n] New msg  [Esc] Back  [Up/Dn] Scroll");
  }

  drawStatusBar();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Menu
// ═══════════════════════════════════════════════════════════════

void drawMenu(int idx, const char* items[], uint8_t count, const uint16_t* colors) {
  display.fillScreen(COL_BG);
  drawHeaderBar("TiggyOpenMesh v3.1");

  display.setFont(nullptr);
  display.setTextSize(1);
  display.fillRoundRect(250, 6, 60, 16, 4, COL_GOOD);
  display.setTextColor(COL_BG, COL_GOOD);
  display.setCursor(256, 10);
  display.print("ONLINE");

  display.setFont(&FreeSans9pt7b);
  int startY = CONTENT_Y + 18;
  int lineH = 24;
  int maxVisible = (SCREEN_H - STATUS_H - startY) / lineH;

  if (idx >= menuScroll + maxVisible) menuScroll = idx - maxVisible + 1;
  if (idx < menuScroll) menuScroll = idx;

  for (int i = menuScroll; i < (int)count && (i - menuScroll) < maxVisible; i++) {
    int y = startY + (i - menuScroll) * lineH;
    int rowY = y - 16;

    if (i == idx) {
      display.fillRoundRect(4, rowY, SCREEN_W - 8, lineH - 1, 6, COL_SELECTED);
      display.fillRoundRect(4, rowY, 4, lineH - 1, 2, colors ? colors[i] : COL_ACCENT);
      display.setTextColor(COL_TEXT, COL_SELECTED);
      display.setCursor(16, y);
      display.print(items[i]);
      display.setCursor(SCREEN_W - 24, y);
      display.print(">");
    } else {
      display.fillRect(8, rowY + 4, 2, lineH - 9, colors ? colors[i] : COL_FAINT);
      display.setTextColor(COL_DIM, COL_BG);
      display.setCursor(16, y);
      display.print(items[i]);
    }
  }

  display.setFont(nullptr);
  display.setTextSize(1);
  if (menuScroll > 0) {
    display.setTextColor(COL_ACCENT, COL_BG);
    display.setCursor(SCREEN_W - 12, CONTENT_Y + 2);
    display.print("^");
  }
  if (menuScroll + maxVisible < (int)count) {
    display.setTextColor(COL_ACCENT, COL_BG);
    display.setCursor(SCREEN_W - 12, SCREEN_H - STATUS_H - 12);
    display.print("v");
  }

  drawStatusBar();
}

void drawCurrentMenu() {
  if (menuState == M_MAIN)
    drawMenu(menuIndex, mainMenu, mainCount, mainMenuColors);
  else
    drawMenu(menuIndex, settingsMenu, settingsCount, nullptr);
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Keyboard & Trackball
// ═══════════════════════════════════════════════════════════════

char readKeyboard() {
  if (digitalRead(BOARD_KB_INT) == HIGH) return 0;
  Wire.requestFrom((uint8_t)KB_I2C_ADDR, (uint8_t)1);
  if (Wire.available()) {
    char c = Wire.read();
    if (c != 0) {
      if (displayAsleep) { wakeDisplay(); return 0; }  // first press wakes only
      wakeDisplay();
      return c;
    }
  }
  return 0;
}

void IRAM_ATTR tbUpISR()    { tbDeltaUp++; }
void IRAM_ATTR tbDownISR()  { tbDeltaDown++; }
void IRAM_ATTR tbLeftISR()  { tbDeltaLeft++; }
void IRAM_ATTR tbRightISR() { tbDeltaRight++; }

void setupTrackball() {
  pinMode(BOARD_TBALL_UP, INPUT_PULLUP);
  pinMode(BOARD_TBALL_DOWN, INPUT_PULLUP);
  pinMode(BOARD_TBALL_LEFT, INPUT_PULLUP);
  pinMode(BOARD_TBALL_RIGHT, INPUT_PULLUP);
  pinMode(BOARD_TBALL_CLICK, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOARD_TBALL_UP),    tbUpISR,    FALLING);
  attachInterrupt(digitalPinToInterrupt(BOARD_TBALL_DOWN),  tbDownISR,  FALLING);
  attachInterrupt(digitalPinToInterrupt(BOARD_TBALL_LEFT),  tbLeftISR,  FALLING);
  attachInterrupt(digitalPinToInterrupt(BOARD_TBALL_RIGHT), tbRightISR, FALLING);
}

char readTrackball() {
  const int threshold = 2;
  char result = 0;
  if (tbDeltaUp >= threshold) {
    tbDeltaUp = tbDeltaDown = tbDeltaLeft = tbDeltaRight = 0;
    result = 'U';
  } else if (tbDeltaDown >= threshold) {
    tbDeltaUp = tbDeltaDown = tbDeltaLeft = tbDeltaRight = 0;
    result = 'D';
  } else if (tbDeltaLeft >= threshold) {
    tbDeltaUp = tbDeltaDown = tbDeltaLeft = tbDeltaRight = 0;
    result = 'L';
  } else if (tbDeltaRight >= threshold) {
    tbDeltaUp = tbDeltaDown = tbDeltaLeft = tbDeltaRight = 0;
    result = 'R';
  } else {
    static bool lastClick = false;
    bool click = (digitalRead(BOARD_TBALL_CLICK) == LOW);
    if (click && !lastClick) { lastClick = true; result = 'C'; }
    lastClick = click;
  }
  if (result) {
    if (displayAsleep) { wakeDisplay(); return 0; }  // first input wakes only
    wakeDisplay();
  }
  return result;
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Text Input
// ═══════════════════════════════════════════════════════════════

void drawTextInput() {
  display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - STATUS_H, COL_BG);
  drawHeaderBar(inputPrompt);

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_DIM, COL_BG);
  display.setCursor(8, CONTENT_Y + 6);
  display.print("To: ");
  display.setTextColor(COL_ACCENT, COL_BG);
  display.print(targetID);

  int maxLen = MAX_INPUT_LEN;
  if (inputTarget == INPUT_LOCAL_ID || inputTarget == INPUT_TARGET_ID) maxLen = NODE_ID_LEN;
  if (inputTarget == INPUT_AES_KEY) maxLen = AES_KEY_LEN;

  display.setTextColor(COL_DIM, COL_BG);
  display.setCursor(SCREEN_W - 50, CONTENT_Y + 6);
  display.print(String(inputBuffer.length()) + "/" + String(maxLen));

  int boxY = CONTENT_Y + 18;
  int boxH = 80;
  display.fillRoundRect(6, boxY, SCREEN_W - 12, boxH, 6, COL_PANEL);
  display.drawRoundRect(6, boxY, SCREEN_W - 12, boxH, 6, COL_ACCENT);

  display.setTextColor(COL_TEXT, COL_PANEL);
  String displayBuf = inputBuffer + "_";
  int charsPerLine = (SCREEN_W - 30) / 6;
  int ty = boxY + 10;
  for (int i = 0; i < (int)displayBuf.length(); i += charsPerLine) {
    display.setCursor(14, ty);
    display.print(displayBuf.substring(i, min((int)displayBuf.length(), i + charsPerLine)));
    ty += 12;
    if (ty > boxY + boxH - 4) break;
  }

  int helpY = boxY + boxH + 8;
  display.setTextColor(COL_FAINT, COL_BG);
  display.setCursor(8, helpY);
  display.print("[Enter] Send   [Bksp] Delete   [Esc] Cancel");

  drawStatusBar();
}

void startTextInput(const String& prompt, InputTarget target) {
  inputBuffer = "";
  inputPrompt = prompt;
  inputTarget = target;
  inputActive = true;
  currentMode = TEXT_INPUT;
  drawTextInput();
}

void handleTextInput(char key) {
  if (key == 0) return;

  int maxLen = MAX_INPUT_LEN;
  if (inputTarget == INPUT_LOCAL_ID || inputTarget == INPUT_TARGET_ID) maxLen = NODE_ID_LEN;
  if (inputTarget == INPUT_AES_KEY) maxLen = AES_KEY_LEN;

  if (key == '\n' || key == '\r') {
    inputActive = false;
    String result = inputBuffer;
    inputBuffer = "";

    switch (inputTarget) {
      case INPUT_MSG:
        if (result.length() > 0) {
          if ((int)result.length() > CHUNK_PLAINTEXT) {
            sendSmartMessage(result);
          } else {
            sendMeshMessage("MSG," + result);
          }
          addChatMessage(String(mesh.localID), result, 0, true);
        }
        break;
      case INPUT_LOCAL_ID:
        if ((int)result.length() == NODE_ID_LEN && mesh.isValidNodeID(result)) {
          strncpy(mesh.localID, result.c_str(), NODE_ID_LEN);
          mesh.localID[NODE_ID_LEN] = '\0';
          saveIDsToEEPROM();
          showNotification("Local ID set: " + result);
        } else {
          showNotification("Invalid ID! Use 4 hex chars");
        }
        break;
      case INPUT_TARGET_ID:
        if ((int)result.length() == NODE_ID_LEN) {
          strncpy(targetID, result.c_str(), NODE_ID_LEN);
          targetID[NODE_ID_LEN] = '\0';
          saveIDsToEEPROM();
          showNotification("Target set: " + result);
        }
        break;
      case INPUT_AES_KEY:
        if ((int)result.length() == AES_KEY_LEN) {
          strncpy(mesh.aes_key_string, result.c_str(), AES_KEY_LEN);
          mesh.aes_key_string[AES_KEY_LEN] = '\0';
          saveKeyToEEPROM();
          showNotification("Key updated!");
        } else {
          showNotification("Key must be 16 chars!");
        }
        break;
    }
    currentMode = MENU;
    drawCurrentMenu();
    return;
  }

  if (key == 0x08 || key == 0x7F) {
    if (inputBuffer.length() > 0) inputBuffer.remove(inputBuffer.length() - 1);
    drawTextInput();
    return;
  }

  if (key == 0x1B) {
    inputActive = false;
    inputBuffer = "";
    currentMode = MENU;
    drawCurrentMenu();
    return;
  }

  if ((int)inputBuffer.length() < maxLen && key >= 0x20 && key <= 0x7E) {
    if (inputTarget == INPUT_LOCAL_ID || inputTarget == INPUT_TARGET_ID)
      key = toupper(key);
    inputBuffer += key;
    drawTextInput();
  }
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Phrase Picker
// ═══════════════════════════════════════════════════════════════

void drawPhrasePicker(int category, int sentence, bool pickingCat,
                      const std::vector<int>& selected) {
  display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - STATUS_H, COL_BG);
  drawHeaderBar(pickingCat ? "Pick Category" : String(categoryLabels[category]));

  if (selected.size() > 0) {
    display.setFont(nullptr);
    display.setTextSize(1);
    int badgeX = SCREEN_W - 36;
    display.fillRoundRect(badgeX, 6, 28, 16, 4, COL_WARN);
    display.setTextColor(COL_BG, COL_WARN);
    display.setCursor(badgeX + 6, 10);
    display.print(String(selected.size()) + "/5");
  }

  display.setFont(&FreeSans9pt7b);
  int startY = CONTENT_Y + 18;
  int lineH = 24;

  if (pickingCat) {
    for (int i = 0; i < categoryCount; i++) {
      int y = startY + i * lineH;
      if (i == category) {
        display.fillRoundRect(4, y - 16, SCREEN_W - 8, lineH - 1, 6, COL_SELECTED);
        display.setTextColor(COL_ACCENT, COL_SELECTED);
        display.setCursor(16, y);
        display.print("> ");
        display.print(categoryLabels[i]);
      } else {
        display.setTextColor(COL_DIM, COL_BG);
        display.setCursor(16, y);
        display.print("  ");
        display.print(categoryLabels[i]);
      }
    }
  } else {
    int startIdx = category * phrasesPerCategory;
    for (int i = 0; i < phrasesPerCategory; i++) {
      int y = startY + i * lineH;
      if (y > SCREEN_H - STATUS_H - 10) break;
      bool alreadyPicked = false;
      for (int s : selected) {
        if (s == startIdx + i) { alreadyPicked = true; break; }
      }
      if (i == sentence) {
        display.fillRoundRect(4, y - 16, SCREEN_W - 8, lineH - 1, 6, COL_SELECTED);
        display.setTextColor(alreadyPicked ? COL_WARN : COL_TEXT, COL_SELECTED);
      } else {
        display.setTextColor(alreadyPicked ? COL_WARN : COL_DIM, COL_BG);
      }
      display.setCursor(16, y);
      if (alreadyPicked) display.print("* ");
      else display.print("  ");
      display.print(phraseLibrary[startIdx + i]);
    }
  }

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_FAINT, COL_BG);
  display.setCursor(8, SCREEN_H - STATUS_H - 4);
  display.print("[Enter] Select  [Esc] Done  [Bksp] Back");

  drawStatusBar();
}

std::vector<int> pickMultiplePhrases() {
  std::vector<int> selected;
  int category = 0, sentence = 0;
  bool pickingCat = true;
  drawPhrasePicker(category, sentence, pickingCat, selected);

  while (true) {
    char tb = readTrackball();
    char kb = readKeyboard();

    if (tb == 'U') {
      if (pickingCat) category = (category - 1 + categoryCount) % categoryCount;
      else sentence = (sentence - 1 + phrasesPerCategory) % phrasesPerCategory;
      drawPhrasePicker(category, sentence, pickingCat, selected);
    }
    if (tb == 'D') {
      if (pickingCat) category = (category + 1) % categoryCount;
      else sentence = (sentence + 1) % phrasesPerCategory;
      drawPhrasePicker(category, sentence, pickingCat, selected);
    }
    if (tb == 'C' || kb == '\r' || kb == '\n') {
      if (pickingCat) {
        pickingCat = false;
        sentence = 0;
      } else {
        selected.push_back(category * phrasesPerCategory + sentence);
        if ((int)selected.size() >= 5) break;
        pickingCat = true;
      }
      drawPhrasePicker(category, sentence, pickingCat, selected);
    }
    if (tb == 'L' || kb == 0x08) {
      if (!pickingCat) { pickingCat = true; drawPhrasePicker(category, sentence, pickingCat, selected); }
      else break;
    }
    if (kb == 0x1B) break;

    receiveCheck();
    yield();
  }
  return selected;
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Menu Handler
// ═══════════════════════════════════════════════════════════════

void handleMenu() {
  char tb = readTrackball();
  char kb = readKeyboard();
  bool needRedraw = false;

  if (tb == 'U') {
    int c = (menuState == M_MAIN) ? mainCount : settingsCount;
    menuIndex = (menuIndex - 1 + c) % c;
    needRedraw = true;
  }
  if (tb == 'D') {
    int c = (menuState == M_MAIN) ? mainCount : settingsCount;
    menuIndex = (menuIndex + 1) % c;
    needRedraw = true;
  }

  if (tb == 'C' || kb == '\r' || kb == '\n') {
    if (menuState == M_MAIN) {
      switch (menuIndex) {
        case 0: startTextInput("New Message", INPUT_MSG); return;
        case 1: {
          std::vector<int> parts = pickMultiplePhrases();
          if (parts.size() > 0) {
            String phraseMsg;
            for (int id : parts) {
              if (phraseMsg.length()) phraseMsg += "|";
              phraseMsg += "$" + String(id, HEX);
            }
            sendSmartMessage(phraseMsg);
            addChatMessage(String(mesh.localID), decompressQuickMsg(phraseMsg), 0, true);
            showNotification("Phrases sent!");
          }
          currentMode = MENU;
          drawCurrentMenu();
          return;
        }
        case 2: currentMode = MESSAGING; chatScroll = 0; drawChatView(); return;
        case 3: showControlPanel(); return;
        case 4: currentMode = SOS_MODE; lastSosSent = 0; return;
        case 5: menuState = M_SETTINGS; menuIndex = 0; needRedraw = true; break;
        case 6: viewKnownNodes(); return;
        case 7: currentMode = MESHVIEW; lastDisplayRefresh = 0; return;
        case 8: broadcastPosition(); return;
        case 9: currentMode = TRACKING; trackUpdateTimer = 0; return;
      }
    } else {
      switch (menuIndex) {
        case 0: startTextInput("Local ID (4 hex)", INPUT_LOCAL_ID); return;
        case 1: startTextInput("Target ID (4 hex)", INPUT_TARGET_ID); return;
        case 2: startTextInput("AES Key (16 chars)", INPUT_AES_KEY); return;
        case 3:
          brightness = (brightness == 255) ? 128 : (brightness == 128) ? 64 : 255;
          analogWrite(BOARD_TFT_BL, brightness);
          showNotification("Brightness: " + String(brightness * 100 / 255) + "%");
          break;
        case 4: {
          // Cycle: 0 (off) → 30 → 60 → 120 → 300 → 0
          if (displaySleepTimeout == 0) displaySleepTimeout = 30;
          else if (displaySleepTimeout <= 30) displaySleepTimeout = 60;
          else if (displaySleepTimeout <= 60) displaySleepTimeout = 120;
          else if (displaySleepTimeout <= 120) displaySleepTimeout = 255; // ~5 min
          else displaySleepTimeout = 0;
          EEPROM.write(EEPROM_SLEEP_ADDR, displaySleepTimeout);
          EEPROM.commit();
          String label = (displaySleepTimeout == 0) ? "Off" : String(displaySleepTimeout) + "s";
          showNotification("Display Timeout: " + label);
          break;
        }
        case 5:
          mesh.knownCount = 0;
          memset(mesh.knownNodes, 0, sizeof(mesh.knownNodes));
          saveIDsToEEPROM();
          showNotification("All nodes cleared");
          needRedraw = true;
          break;
        case 6: menuState = M_MAIN; menuIndex = 0; needRedraw = true; break;
      }
    }
  }

  if (tb == 'L' || kb == 0x1B) {
    if (menuState == M_SETTINGS) {
      menuState = M_MAIN;
      menuIndex = 0;
      needRedraw = true;
    }
  }

  if (needRedraw) drawCurrentMenu();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Known Nodes
// ═══════════════════════════════════════════════════════════════

void viewKnownNodes() {
  display.fillScreen(COL_BG);
  drawHeaderBar("Known Nodes (" + String(mesh.knownCount) + ")");

  display.setFont(&FreeSans9pt7b);
  int y = CONTENT_Y + 18;

  for (uint8_t i = 0; i < mesh.knownCount; i++) {
    if (y > SCREEN_H - STATUS_H - 10) break;

    auto it = mesh.routingTable.find(String(mesh.knownNodes[i]));
    bool active = (it != mesh.routingTable.end() && millis() - it->second.lastSeen < STALE_TIMEOUT);
    display.fillCircle(12, y - 4, 4, active ? COL_GOOD : COL_FAINT);

    display.setTextColor(COL_TEXT, COL_BG);
    display.setCursor(22, y);
    display.print(mesh.knownNodes[i]);

    if (it != mesh.routingTable.end()) {
      display.setTextColor(COL_DIM, COL_BG);
      display.setCursor(80, y);
      display.print(String(it->second.rssi) + "dBm");
      drawSignalBars(170, y + 4, it->second.rssi);

      unsigned long age = (millis() - it->second.lastSeen) / 1000;
      display.setCursor(210, y);
      if (age < 60) display.print(String(age) + "s");
      else display.print(String(age / 60) + "m");

      // Route info: show "via <nextHop>" for multi-hop nodes
      int hops = it->second.cost / COST_WEIGHT;
      if (hops > 1 && it->second.nextHop.length() > 0) {
        display.setCursor(250, y);
        display.print("via " + it->second.nextHop);
      }
    } else {
      display.setTextColor(COL_FAINT, COL_BG);
      display.setCursor(80, y);
      display.print("not seen");
    }

    if (nodePos[i].valid) {
      display.setTextColor(COL_GOOD, COL_BG);
      display.setCursor(270, y);
      display.print("GPS");
    }

    y += 24;
  }

  if (mesh.knownCount == 0) {
    display.setTextColor(COL_DIM, COL_BG);
    display.setCursor(40, 100);
    display.print("No nodes discovered yet");
    display.setCursor(40, 125);
    display.print("Waiting for heartbeats...");
  }

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_FAINT, COL_BG);
  display.setCursor(8, SCREEN_H - STATUS_H - 4);
  display.print("[Any key to go back]");
  drawStatusBar();

  while (true) {
    receiveCheck();
    if (readKeyboard() || readTrackball()) break;
    yield();
  }
  currentMode = MENU;
  drawCurrentMenu();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: GPS & Position
// ═══════════════════════════════════════════════════════════════

void updateMyOwnPosition() {
  if (!gps.location.isValid()) return;
  for (uint8_t i = 0; i < mesh.knownCount; i++) {
    if (strcmp(mesh.knownNodes[i], mesh.localID) == 0) {
      nodePos[i] = { gps.location.lat(), gps.location.lng(), true };
      return;
    }
  }
}

void broadcastPosition() {
  if (!gps.location.isValid()) {
    showNotification("No GPS fix - can't broadcast");
    return;
  }
  String pos = "POS," + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
  char oldTarget[NODE_ID_LEN + 1];
  strncpy(oldTarget, targetID, NODE_ID_LEN + 1);
  strncpy(targetID, "FFFF", NODE_ID_LEN + 1);
  sendMeshMessage(pos);
  strncpy(targetID, oldTarget, NODE_ID_LEN + 1);
  updateMyOwnPosition();
  showNotification("Position broadcast OK");
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Tracking
// ═══════════════════════════════════════════════════════════════

void drawArrow(int cx, int cy, int x1, int y1) {
  display.drawLine(cx, cy, x1, y1, COL_GOOD);
  float angle = atan2(y1 - cy, x1 - cx);
  int len = 12;
  int x2 = x1 - len * cos(angle - PI / 6);
  int y2 = y1 - len * sin(angle - PI / 6);
  int x3 = x1 - len * cos(angle + PI / 6);
  int y3 = y1 - len * sin(angle + PI / 6);
  display.fillTriangle(x1, y1, x2, y2, x3, y3, COL_GOOD);
}

void handleTracking() {
  char kb = readKeyboard();
  if (kb == 0x1B || kb == 'q') { currentMode = MENU; drawCurrentMenu(); return; }
  if (millis() - trackUpdateTimer < 1000) return;
  trackUpdateTimer = millis();

  int idx = -1;
  for (uint8_t i = 0; i < mesh.knownCount; i++) {
    if (strcmp(mesh.knownNodes[i], targetID) == 0) { idx = i; break; }
  }

  if (idx < 0 || !nodePos[idx].valid) {
    display.fillScreen(COL_BG);
    drawHeaderBar("Find: " + String(targetID));
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(COL_DIM, COL_BG);
    display.setCursor(40, 100);
    display.print("No position data for");
    display.setCursor(40, 125);
    display.setTextColor(COL_ACCENT, COL_BG);
    display.print(String(targetID));
    display.setTextColor(COL_FAINT, COL_BG);
    display.setCursor(40, 155);
    display.print("Target must broadcast position");
    drawStatusBar();
    return;
  }
  if (!gps.location.isValid()) {
    display.fillScreen(COL_BG);
    drawHeaderBar("Find: " + String(targetID));
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(COL_WARN, COL_BG);
    display.setCursor(40, 100);
    display.print("Waiting for GPS fix...");
    drawStatusBar();
    return;
  }

  double myLat = gps.location.lat(), myLon = gps.location.lng();
  double d = TinyGPSPlus::distanceBetween(myLat, myLon, nodePos[idx].lat, nodePos[idx].lon);
  double bearing = TinyGPSPlus::courseTo(myLat, myLon, nodePos[idx].lat, nodePos[idx].lon);

  display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - STATUS_H, COL_BG);
  drawHeaderBar("Find: " + String(targetID));

  int cx = 220, cy = 120;
  display.drawCircle(cx, cy, 65, COL_FAINT);
  display.drawCircle(cx, cy, 64, COL_FAINT);

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_DIM, COL_BG);
  display.setCursor(cx - 2, cy - 72); display.print("N");
  display.setCursor(cx + 70, cy - 4); display.print("E");
  display.setCursor(cx - 2, cy + 68); display.print("S");
  display.setCursor(cx - 72, cy - 4); display.print("W");

  double rad = (90.0 - bearing) * DEG_TO_RAD;
  int ax = cx + cos(rad) * 55;
  int ay = cy - sin(rad) * 55;
  drawArrow(cx, cy, ax, ay);

  display.setFont(&FreeSans12pt7b);
  display.setTextColor(COL_TEXT, COL_BG);
  display.setCursor(10, 80);
  if (d > 1000) {
    display.print(d / 1000.0, 1);
    display.setFont(&FreeSans9pt7b);
    display.print(" km");
  } else {
    display.print((int)d);
    display.setFont(&FreeSans9pt7b);
    display.print(" m");
  }

  display.setTextColor(COL_DIM, COL_BG);
  display.setCursor(10, 110);
  display.print(String(bearing, 0) + " degrees");

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_FAINT, COL_BG);
  display.setCursor(10, 140);
  display.print("Target: " + String(nodePos[idx].lat, 5) + "," + String(nodePos[idx].lon, 5));
  display.setCursor(10, 152);
  display.print("You:    " + String(myLat, 5) + "," + String(myLon, 5));

  display.setCursor(10, 170);
  display.setTextColor(COL_FAINT, COL_BG);
  display.print("[q/Esc] Back");

  drawStatusBar();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: SOS Mode
// ═══════════════════════════════════════════════════════════════

void handleSOS() {
  static bool flashState = false;
  static unsigned long lastFlash = 0;
  static unsigned long exitTimer = 0;

  if (millis() - lastFlash > 400) {
    lastFlash = millis();
    flashState = !flashState;
    display.fillRect(0, 0, SCREEN_W, SCREEN_H - STATUS_H, flashState ? COL_BAD : 0x0010);

    display.setFont(&FreeSans12pt7b);
    display.setTextColor(COL_TEXT);
    display.setCursor(30, 60);
    display.print("!! EMERGENCY !!");

    display.setFont(&FreeSans9pt7b);
    display.setCursor(30, 100);
    if (gps.location.isValid()) {
      display.print(String(gps.location.lat(), 5) + ", " + String(gps.location.lng(), 5));
    } else {
      display.print("No GPS fix");
    }

    display.setCursor(30, 140);
    display.setTextColor(COL_DIM);
    unsigned long nextSend = (SOS_INTERVAL - (millis() - lastSosSent)) / 1000;
    display.print("Next broadcast: " + String(nextSend) + "s");

    display.setCursor(30, 180);
    display.print("Hold Esc 3s to cancel");
    drawStatusBar();
  }

  if (millis() - lastSosSent >= SOS_INTERVAL || lastSosSent == 0) {
    lastSosSent = millis();
    char oldTarget[NODE_ID_LEN + 1];
    strncpy(oldTarget, targetID, NODE_ID_LEN + 1);
    strncpy(targetID, "FFFF", NODE_ID_LEN + 1);
    if (gps.location.isValid())
      sendMeshMessage("SOS," + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6));
    else
      sendMeshMessage("SOS,NOFIX");
    strncpy(targetID, oldTarget, NODE_ID_LEN + 1);
  }

  char kb = readKeyboard();
  if (kb == 0x1B) {
    if (exitTimer == 0) exitTimer = millis();
    if (millis() - exitTimer > 3000) {
      exitTimer = 0;
      currentMode = MENU;
      drawCurrentMenu();
    }
  } else if (kb != 0) {
    exitTimer = 0;
  }
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Mesh Map View
// ═══════════════════════════════════════════════════════════════

void handleMeshView() {
  char kb = readKeyboard();
  if (kb == 0x1B || kb == 'q' || readTrackball() == 'L') {
    currentMode = MENU;
    drawCurrentMenu();
    return;
  }
  if (millis() - lastDisplayRefresh < 5000 && lastDisplayRefresh != 0) return;
  lastDisplayRefresh = millis();

  display.fillScreen(COL_BG);
  drawHeaderBar("Mesh Map (" + String(meshMap.size()) + " nodes)");

  display.setFont(&FreeSans9pt7b);
  int y = CONTENT_Y + 18;

  for (const auto& pair : meshMap) {
    if (y > SCREEN_H - STATUS_H - 10) break;
    const NodeInfo& node = pair.second;

    unsigned long ageS = (millis() - node.lastSeen) / 1000;
    bool fresh = (ageS < 60);

    display.fillCircle(12, y - 4, 4, fresh ? COL_GOOD : COL_FAINT);
    display.setTextColor(fresh ? COL_TEXT : COL_DIM, COL_BG);
    display.setCursor(22, y);
    display.print(node.id);

    display.setTextColor(COL_DIM, COL_BG);
    display.setCursor(80, y);
    if (ageS < 60) display.print("<1m");
    else display.print(String(ageS / 60) + "m");

    drawSignalBars(130, y + 4, node.rssi);

    display.setTextColor(COL_FAINT, COL_BG);
    display.setCursor(170, y);
    for (size_t n = 0; n < node.neighbors.size() && n < 3; n++) {
      if (n > 0) display.print(",");
      display.print(node.neighbors[n]);
    }

    y += 24;
  }

  if (meshMap.empty()) {
    display.setTextColor(COL_DIM, COL_BG);
    display.setCursor(40, 100);
    display.print("No mesh data yet");
    display.setCursor(40, 125);
    display.print("Waiting for heartbeats...");
  }

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_FAINT, COL_BG);
  display.setCursor(8, SCREEN_H - STATUS_H - 4);
  display.print("[q/Esc] Back   Auto-refresh 5s");
  drawStatusBar();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: First-Boot Wizard
// ═══════════════════════════════════════════════════════════════

void drawWizardStep() {
  display.fillScreen(COL_BG);

  for (int i = 0; i < 3; i++) {
    int dx = SCREEN_W / 2 - 20 + i * 20;
    if (i == wizardStep)
      display.fillCircle(dx, 10, 5, COL_ACCENT);
    else
      display.drawCircle(dx, 10, 5, COL_FAINT);
  }

  display.setFont(&FreeSans12pt7b);
  display.setTextColor(COL_ACCENT, COL_BG);

  switch (wizardStep) {
    case 0:
      display.setCursor(20, 50);
      display.print("Welcome!");
      display.setFont(&FreeSans9pt7b);
      display.setTextColor(COL_TEXT, COL_BG);
      display.setCursor(20, 80);
      display.print("Suggested ID: ");
      display.setTextColor(COL_ACCENT, COL_BG);
      display.print(inputBuffer);

      display.setTextColor(COL_DIM, COL_BG);
      if (wizardScanActive) {
        int remaining = max(0, (int)(wizardScanEnd - millis()) / 1000);
        display.setCursor(20, 105);
        display.print("Scanning for conflicts... ");
        display.print(remaining);
        display.print("s");
        // Progress bar
        int barW = SCREEN_W - 40;
        int elapsed = 10000 - (wizardScanEnd - millis());
        int fillW = constrain(elapsed * barW / 10000, 0, barW);
        display.drawRect(20, 115, barW, 8, COL_FAINT);
        display.fillRect(20, 115, fillW, 8, COL_ACCENT);
      } else if (wizardScanDone) {
        display.setTextColor(COL_GOOD, COL_BG);
        display.setCursor(20, 105);
        display.print("No conflicts found!");
        display.setTextColor(COL_DIM, COL_BG);
        display.setCursor(20, 130);
        display.print("Press Enter to accept or");
        display.setCursor(20, 150);
        display.print("type a new ID:");
      } else {
        display.setCursor(20, 105);
        display.print("Type your 4-character");
        display.setCursor(20, 125);
        display.print("Node ID (e.g. 0001):");
      }

      display.setFont(&FreeSans12pt7b);
      display.setTextColor(COL_ACCENT, COL_BG);
      display.setCursor(20, 220);
      display.print(inputBuffer + "_");
      break;

    case 1:
      display.setCursor(20, 50);
      display.print("AES Encryption Key");
      display.setFont(&FreeSans9pt7b);
      display.setTextColor(COL_TEXT, COL_BG);
      display.setCursor(20, 85);
      display.print("All nodes in your mesh must");
      display.setCursor(20, 107);
      display.print("share the same 16-char key.");
      display.setTextColor(COL_DIM, COL_BG);
      display.setCursor(20, 140);
      display.print("Current key:");
      display.setFont(nullptr);
      display.setTextSize(1);
      display.fillRoundRect(18, 152, SCREEN_W - 36, 20, 4, COL_PANEL);
      display.setTextColor(COL_ACCENT, COL_PANEL);
      display.setCursor(24, 158);
      display.print(inputBuffer + "_");

      display.setTextColor(COL_FAINT, COL_BG);
      display.setCursor(20, 190);
      display.print("[Enter] Accept  or type new key");
      break;

    case 2:
      display.setCursor(20, 50);
      display.print("Ready!");
      display.setFont(&FreeSans9pt7b);
      display.setTextColor(COL_GOOD, COL_BG);
      display.setCursor(20, 85);
      display.print("Node ID:  " + String(mesh.localID));
      display.setCursor(20, 110);
      display.print("AES Key:  " + String(mesh.aes_key_string).substring(0, 8) + "...");
      display.setCursor(20, 135);
      display.print("Freq:     " + String(LORA_FREQ, 1) + " MHz");
      display.setTextColor(COL_DIM, COL_BG);
      display.setCursor(20, 170);
      display.print("Tip: Set Target ID in Settings");
      display.setCursor(20, 192);
      display.print("to message a specific node.");
      display.setTextColor(COL_ACCENT, COL_BG);
      display.setCursor(20, 220);
      display.print("[Enter] Start using TiggyOpenMesh");
      break;
  }
}

void handleFirstBoot() {
  // Process scan during wizard step 0
  if (wizardStep == 0 && wizardScanActive) {
    // Temporarily set localID to scan for conflicts against suggested ID
    char savedID[NODE_ID_LEN + 1];
    strncpy(savedID, mesh.localID, NODE_ID_LEN + 1);
    strncpy(mesh.localID, inputBuffer.c_str(), NODE_ID_LEN);
    mesh.localID[NODE_ID_LEN] = '\0';

    if (mesh.idConflictDetected) {
      // Conflict found — regenerate random ID and restart scan
      uint16_t randId = random(1, 0xFFFE);
      char idBuf[5];
      snprintf(idBuf, sizeof(idBuf), "%04X", randId);
      inputBuffer = String(idBuf);
      strncpy(mesh.localID, idBuf, NODE_ID_LEN + 1);
      mesh.idConflictDetected = false;
      wizardScanEnd = millis() + 10000;
      showNotification("ID conflict! Trying " + inputBuffer);
    } else if (millis() >= wizardScanEnd) {
      // Scan complete — no conflicts
      wizardScanActive = false;
      wizardScanDone = true;
    }

    // Restore localID (will be set properly when user confirms)
    strncpy(mesh.localID, savedID, NODE_ID_LEN + 1);

    // Refresh display periodically during scan
    static unsigned long lastWizardRefresh = 0;
    if (millis() - lastWizardRefresh > 500) {
      lastWizardRefresh = millis();
      drawWizardStep();
    }
  }

  char kb = readKeyboard();
  if (kb == 0) return;

  switch (wizardStep) {
    case 0:
      if ((kb == '\r' || kb == '\n') && (int)inputBuffer.length() == NODE_ID_LEN) {
        // If scan still active, wait for it to finish
        if (wizardScanActive) return;
        strncpy(mesh.localID, inputBuffer.c_str(), NODE_ID_LEN);
        mesh.localID[NODE_ID_LEN] = '\0';
        saveIDsToEEPROM();
        inputBuffer = String(mesh.aes_key_string);
        wizardStep = 1;
        drawWizardStep();
      } else if (kb == 0x08 && inputBuffer.length() > 0) {
        inputBuffer.remove(inputBuffer.length() - 1);
        // User is typing — stop auto-scan, they're choosing their own ID
        wizardScanActive = false;
        wizardScanDone = false;
        drawWizardStep();
      } else if ((int)inputBuffer.length() < NODE_ID_LEN && kb >= 0x20) {
        inputBuffer += (char)toupper(kb);
        // User is typing — stop auto-scan
        wizardScanActive = false;
        wizardScanDone = false;
        drawWizardStep();
      }
      break;

    case 1:
      if (kb == '\r' || kb == '\n') {
        if ((int)inputBuffer.length() == AES_KEY_LEN) {
          strncpy(mesh.aes_key_string, inputBuffer.c_str(), AES_KEY_LEN);
          mesh.aes_key_string[AES_KEY_LEN] = '\0';
          saveKeyToEEPROM();
        }
        wizardStep = 2;
        drawWizardStep();
      } else if (kb == 0x08 && inputBuffer.length() > 0) {
        inputBuffer.remove(inputBuffer.length() - 1);
        drawWizardStep();
      } else if ((int)inputBuffer.length() < AES_KEY_LEN && kb >= 0x20) {
        inputBuffer += kb;
        drawWizardStep();
      }
      break;

    case 2:
      if (kb == '\r' || kb == '\n') {
        EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
        EEPROM.commit();
        currentMode = MENU;
        drawCurrentMenu();
      }
      break;
  }
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Remote GPIO Control
// ═══════════════════════════════════════════════════════════════

bool isPinSafe(int pin) {
  const int forbidden[] = {
    BOARD_SPI_MOSI, BOARD_SPI_MISO, BOARD_SPI_SCK,
    RADIO_CS, RADIO_RST, RADIO_DIO1,
    #if RADIO_BUSY >= 0
    RADIO_BUSY,
    #endif
    #if BOARD_TFT_CS >= 0
    BOARD_TFT_CS, BOARD_TFT_DC,
    #endif
    0
  };
  for (int f : forbidden) {
    if (pin == f) return false;
  }
  if (pin < 0 || pin > 48) return false;
  return true;
}

void handleIncomingCmd(const String& from, const String& cmdBody) {
  int c1 = cmdBody.indexOf(',');
  String action = (c1 > 0) ? cmdBody.substring(0, c1) : cmdBody;
  String rest = (c1 > 0) ? cmdBody.substring(c1 + 1) : "";

  if (action == "SET") {
    int c2 = rest.indexOf(',');
    if (c2 < 0) return;
    int pin = rest.substring(0, c2).toInt();
    int val = rest.substring(c2 + 1).toInt();
    if (!isPinSafe(pin)) {
      debugPrint("CMD SET blocked: pin " + String(pin));
      return;
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, val ? HIGH : LOW);
    debugPrint("CMD SET pin " + String(pin) + " = " + String(val));
    addChatMessage(from, "SET pin " + String(pin) + " -> " + (val ? "ON" : "OFF"), mesh.lastRSSI, false);
    sendCmdResponse(from, "RSP," + String(pin) + "," + String(val));
  }
  else if (action == "GET") {
    int pin = rest.toInt();
    if (!isPinSafe(pin)) return;
    int value;
    if (pin >= 34 && pin <= 39) {
      value = analogRead(pin);
    } else {
      pinMode(pin, INPUT);
      value = digitalRead(pin);
    }
    debugPrint("CMD GET pin " + String(pin) + " = " + String(value));
    sendCmdResponse(from, "RSP," + String(pin) + "," + String(value));
  }
  else if (action == "PULSE") {
    int c2 = rest.indexOf(',');
    if (c2 < 0) return;
    int pin = rest.substring(0, c2).toInt();
    int ms = rest.substring(c2 + 1).toInt();
    if (!isPinSafe(pin) || ms > 30000) return;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    delay(ms);
    digitalWrite(pin, LOW);
    debugPrint("CMD PULSE pin " + String(pin) + " for " + String(ms) + "ms");
    addChatMessage(from, "PULSE pin " + String(pin) + " " + String(ms) + "ms", mesh.lastRSSI, false);
    sendCmdResponse(from, "RSP," + String(pin) + ",0");
  }
  else if (action == "RSP") {
    int c2 = rest.indexOf(',');
    if (c2 < 0) return;
    int pin = rest.substring(0, c2).toInt();
    String value = rest.substring(c2 + 1);
    String msg = "Pin " + String(pin) + " = " + value;
    addChatMessage(from, msg, mesh.lastRSSI, false);
    showNotification(from + ": " + msg);
  }
}

void sendCmdResponse(const String& to, const String& rsp) {
  char oldTarget[NODE_ID_LEN + 1];
  strncpy(oldTarget, targetID, NODE_ID_LEN + 1);
  strncpy(targetID, to.c_str(), NODE_ID_LEN);
  targetID[NODE_ID_LEN] = '\0';
  sendMeshMessage("CMD," + rsp);
  strncpy(targetID, oldTarget, NODE_ID_LEN + 1);
}

void sendRemoteCmd(const String& target, const String& cmd) {
  char oldTarget[NODE_ID_LEN + 1];
  strncpy(oldTarget, targetID, NODE_ID_LEN + 1);
  strncpy(targetID, target.c_str(), NODE_ID_LEN);
  targetID[NODE_ID_LEN] = '\0';
  sendMeshMessage("CMD," + cmd);
  strncpy(targetID, oldTarget, NODE_ID_LEN + 1);
  showNotification("CMD sent to " + target);
}

// ─── Control Panel UI ───────────────────────────────────────

void showControlPanel() {
  display.fillScreen(COL_BG);
  drawHeaderBar("Remote Control > " + String(targetID));

  display.setFont(&FreeSans9pt7b);
  display.setTextColor(COL_DIM, COL_BG);
  display.setCursor(8, CONTENT_Y + 16);
  display.print("Target: " + String(targetID));

  const char* actions[] = {
    "Relay 1 ON  (pin 2)",
    "Relay 1 OFF (pin 2)",
    "Relay 2 ON  (pin 4)",
    "Relay 2 OFF (pin 4)",
    "Read Sensor (pin 34)",
    "Read Sensor (pin 36)",
    "Pulse pin 2 (2s)",
    "Custom Command",
    "Back"
  };
  const char* cmds[] = {
    "SET,2,1", "SET,2,0", "SET,4,1", "SET,4,0",
    "GET,34", "GET,36", "PULSE,2,2000", "", ""
  };
  const int actionCount = 9;

  static int ctrlIdx = 0;
  bool inMenu = true;
  drawCtrlMenu(actions, actionCount, ctrlIdx);

  while (inMenu) {
    char tb = readTrackball();
    char kb = readKeyboard();

    if (tb == 'U') { ctrlIdx = (ctrlIdx - 1 + actionCount) % actionCount; drawCtrlMenu(actions, actionCount, ctrlIdx); }
    if (tb == 'D') { ctrlIdx = (ctrlIdx + 1) % actionCount; drawCtrlMenu(actions, actionCount, ctrlIdx); }

    if (tb == 'C' || kb == '\r' || kb == '\n') {
      if (ctrlIdx == actionCount - 1) {
        inMenu = false;
      } else if (ctrlIdx == actionCount - 2) {
        inMenu = false;
        startTextInput("CMD (e.g. SET,2,1)", INPUT_MSG);
        inputTarget = INPUT_MSG;
      } else {
        sendRemoteCmd(String(targetID), String(cmds[ctrlIdx]));
        addChatMessage(String(mesh.localID), "CMD>" + String(cmds[ctrlIdx]), 0, true);
        delay(500);
      }
    }

    if (kb == 0x1B) inMenu = false;

    receiveCheck();
    yield();
  }
  currentMode = MENU;
  drawCurrentMenu();
}

void drawCtrlMenu(const char* items[], int count, int sel) {
  int startY = CONTENT_Y + 36;
  int lineH = 22;
  display.fillRect(0, startY - 16, SCREEN_W, SCREEN_H - startY - STATUS_H + 16, COL_BG);

  display.setFont(&FreeSans9pt7b);
  for (int i = 0; i < count; i++) {
    int y = startY + i * lineH;
    if (y > SCREEN_H - STATUS_H - 5) break;
    if (i == sel) {
      display.fillRoundRect(4, y - 16, SCREEN_W - 8, lineH - 1, 4, COL_SELECTED);
      display.setTextColor(COL_ACCENT, COL_SELECTED);
      display.setCursor(12, y);
      display.print("> ");
    } else {
      display.setTextColor(COL_DIM, COL_BG);
      display.setCursor(12, y);
      display.print("  ");
    }
    display.print(items[i]);
  }
  drawStatusBar();
}

// ═══════════════════════════════════════════════════════════════
// SECTION: EEPROM
// ═══════════════════════════════════════════════════════════════

void setupEEPROM() { EEPROM.begin(EEPROM_SIZE); }

void saveKeyToEEPROM() {
  EEPROM.put(EEPROM_KEY_ADDR, mesh.aes_key_string);
  EEPROM.commit();
}

void loadKeyFromEEPROM() {
  EEPROM.get(EEPROM_KEY_ADDR, mesh.aes_key_string);
  bool valid = (strlen(mesh.aes_key_string) == AES_KEY_LEN);
  if (valid) {
    for (int i = 0; i < AES_KEY_LEN; i++) {
      if (mesh.aes_key_string[i] < 0x20 || mesh.aes_key_string[i] > 0x7E) { valid = false; break; }
    }
  }
  if (!valid) {
    // Generate random key using ESP32 hardware RNG
    for (int i = 0; i < AES_KEY_LEN; i++)
      mesh.aes_key_string[i] = 33 + (esp_random() % 94);  // printable ASCII !-~
    mesh.aes_key_string[AES_KEY_LEN] = '\0';
    saveKeyToEEPROM();
    Serial.println("Generated new AES key: " + String(mesh.aes_key_string));
  }
}

void saveIDsToEEPROM() {
  EEPROM.put(0, mesh.localID);
  EEPROM.put(10, targetID);
  EEPROM.put(20, mesh.knownNodes);
  EEPROM.put(400, mesh.knownCount);
  EEPROM.commit();
}

void loadIDsFromEEPROM() {
  EEPROM.get(0, mesh.localID);
  EEPROM.get(10, targetID);
  EEPROM.get(20, mesh.knownNodes);
  EEPROM.get(400, mesh.knownCount);
  if (!mesh.isValidNodeID(String(mesh.localID))) strncpy(mesh.localID, "0001", NODE_ID_LEN + 1);
  if (strlen(targetID) != NODE_ID_LEN) strncpy(targetID, "FFFF", NODE_ID_LEN + 1);
  if (mesh.knownCount > MAX_NODES) { mesh.knownCount = 0; memset(mesh.knownNodes, 0, sizeof(mesh.knownNodes)); }
  uint8_t storedSleep = EEPROM.read(EEPROM_SLEEP_ADDR);
  displaySleepTimeout = (storedSleep == 0xFF) ? 60 : storedSleep;  // 0xFF = unset, default 60s
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Boot Screen
// ═══════════════════════════════════════════════════════════════

void drawBootScreen() {
  display.fillScreen(COL_BG);

  for (int x = 0; x < SCREEN_W; x++) {
    uint16_t c = (x < SCREEN_W / 3) ? COL_BAD : (x < 2 * SCREEN_W / 3) ? COL_WARN : COL_GOOD;
    display.drawPixel(x, 0, c);
    display.drawPixel(x, 1, c);
  }

  display.setFont(&FreeSans12pt7b);
  display.setTextColor(COL_ACCENT);
  display.setCursor(60, 55);
  display.print("TiggyOpenMesh");

  display.setFont(&FreeSans9pt7b);
  display.setTextColor(COL_GOOD);
  display.setCursor(60, 82);
  display.print("T-Deck Plus Edition");

  display.fillRoundRect(60, 90, 40, 16, 4, COL_ACCENT);
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(COL_BG, COL_ACCENT);
  display.setCursor(68, 94);
  display.print("v3.1");

  display.fillRoundRect(30, 115, SCREEN_W - 60, 80, 6, COL_PANEL);
  display.setTextColor(COL_DIM, COL_PANEL);
  int iy = 126;
  display.setCursor(42, iy);      display.print("Freq:  " + String(LORA_FREQ, 1) + " MHz");
  display.setCursor(170, iy);     display.print("SF: " + String(LORA_SF));
  display.setCursor(42, iy + 14); display.print("BW:    " + String(LORA_BW, 0) + " kHz");
  display.setCursor(170, iy + 14);display.print("Power: " + String(LORA_POWER) + "dBm");
  display.setCursor(42, iy + 28); display.print("Node:  " + String(mesh.localID));
  display.setCursor(170, iy + 28);display.print("AES: ");
  display.setTextColor(COL_GOOD, COL_PANEL);
  display.print("Active");
  display.setCursor(42, iy + 42);
  display.setTextColor(COL_DIM, COL_PANEL);
  display.print("Chip:  ESP32-S3 + SX1262");

  for (int i = 0; i <= 100; i += 2) {
    int barW = (SCREEN_W - 80) * i / 100;
    display.fillRoundRect(40, 210, barW, 10, 3, COL_ACCENT);
    display.drawRoundRect(40, 210, SCREEN_W - 80, 10, 3, COL_FAINT);
    delay(15);
  }
  display.fillRoundRect(40, 210, SCREEN_W - 80, 10, 3, COL_GOOD);
  delay(300);
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Setup
// ═══════════════════════════════════════════════════════════════

void setup() {
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
  delay(100);

  Serial.begin(115200);
  delay(200);
  debugPrint("\n=== TiggyOpenMesh v3.1 - T-Deck Plus ===");
  // Seed RNG with hardware random XOR'd with unique MAC address
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  uint32_t macSeed = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                     ((uint32_t)mac[4] << 8)  | mac[5];
  randomSeed(esp_random() ^ macSeed);

  // SPI chip selects HIGH before bus init
  pinMode(BOARD_TFT_CS, OUTPUT);    digitalWrite(BOARD_TFT_CS, HIGH);
  pinMode(RADIO_CS, OUTPUT);        digitalWrite(RADIO_CS, HIGH);
  pinMode(BOARD_SDCARD_CS, OUTPUT); digitalWrite(BOARD_SDCARD_CS, HIGH);

  sharedSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);

  // Display
  pinMode(BOARD_TFT_BL, OUTPUT);
  analogWrite(BOARD_TFT_BL, brightness);
  display.init(240, 320);
  display.setRotation(3);
  display.fillScreen(COL_BG);

  // I2C keyboard
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  pinMode(BOARD_KB_INT, INPUT_PULLUP);

  // I2C scan — show on display for debugging
  display.setTextSize(1);
  display.setTextColor(0xFFFF);
  display.setCursor(10, 10);
  display.print("I2C Scan:");
  int foundCount = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      display.setCursor(10 + (foundCount % 6) * 50, 25 + (foundCount / 6) * 12);
      display.print("0x");
      if (addr < 16) display.print("0");
      display.print(addr, HEX);
      foundCount++;
    }
  }
  if (foundCount == 0) { display.setCursor(10, 25); display.print("No devices found!"); }
  display.setCursor(10, 60);
  display.print("KB_INT pin " + String(BOARD_KB_INT) + ": " + String(digitalRead(BOARD_KB_INT) ? "HIGH" : "LOW"));
  delay(5000);  // Show for 5 seconds
  display.fillScreen(COL_BG);

  setupTrackball();
  setupEEPROM();
  loadKeyFromEEPROM();
  loadIDsFromEEPROM();

  drawBootScreen();

  GPSSerial.begin(9600, SERIAL_8N1, BOARD_GPS_RX, BOARD_GPS_TX);
  setupRadio();

  // Register MeshCore callbacks
  mesh.onTransmitRaw = radioTransmit;
  mesh.onChannelFree = radioChannelFree;
  mesh.onMessage = handleMessage;
  mesh.onCmd = handleCmd;
  mesh.onAck = handleAck;
  mesh.onNodeDiscovered = handleNodeDiscovered;
  mesh.onIdConflict = handleIdConflict;

  analogReadResolution(12);
  pinMode(BOARD_BAT_ADC, INPUT);

  nextHeartbeatTime = millis() + random(1000, 5000);
  lastInputTime = millis();

  debugPrint("Heap: " + String(ESP.getFreeHeap()));

  // Check if first boot
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
    currentMode = FIRST_BOOT;
    wizardStep = 0;
    // Generate random suggested ID
    uint16_t randId = random(1, 0xFFFE);
    char idBuf[5];
    snprintf(idBuf, sizeof(idBuf), "%04X", randId);
    inputBuffer = String(idBuf);
    // Start conflict scan
    wizardScanActive = true;
    wizardScanDone = false;
    mesh.idConflictDetected = false;
    wizardScanEnd = millis() + 10000;
    drawWizardStep();
  } else {
    delay(400);
    drawCurrentMenu();
  }
}

// ═══════════════════════════════════════════════════════════════
// SECTION: Main Loop
// ═══════════════════════════════════════════════════════════════

void loop() {
  receiveCheck();
  handleAckRetry();
  mesh.processPendingForwards();

  // Heartbeat
  if (millis() > nextHeartbeatTime) {
    mesh.sendHeartbeat();
    nextHeartbeatTime = millis() + HB_INTERVAL + random(-2000, 2000);
  }

  // Route cleanup (every 10s)
  if (millis() - lastRouteClean > 10000) {
    mesh.pruneStale();
    lastRouteClean = millis();
  }

  // GPS feed
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  // Periodic GPS broadcast
  if (millis() - lastGpsSend >= GPS_INTERVAL) {
    lastGpsSend = millis();
    if (gps.location.isValid()) {
      char oldT[NODE_ID_LEN + 1];
      strncpy(oldT, targetID, NODE_ID_LEN + 1);
      strncpy(targetID, "FFFF", NODE_ID_LEN + 1);
      sendMeshMessage("POS," + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6));
      strncpy(targetID, oldT, NODE_ID_LEN + 1);
      updateMyOwnPosition();
    }
  }

  // Status bar refresh
  if (millis() - lastBarUpdate >= BAR_UPDATE_MS) {
    lastBarUpdate = millis();
    if (currentMode == MENU || currentMode == MESSAGING) drawStatusBar();
  }

  // Chunk timeout
  for (int i = 0; i < MAX_CHUNKS; i++) {
    if (chunkBuffers[i].active && millis() - chunkBuffers[i].lastUpdate > 30000)
      chunkBuffers[i].active = false;
  }

  // SOS keeps display on
  if (currentMode == SOS_MODE) lastInputTime = millis();

  // Display auto-sleep
  if (displaySleepTimeout > 0 && !displayAsleep) {
      unsigned long idle = millis() - lastInputTime;
      if (idle > (unsigned long)displaySleepTimeout * 1000UL) {
          analogWrite(BOARD_TFT_BL, 0);
          displayAsleep = true;
      } else if (idle > (unsigned long)displaySleepTimeout * 500UL) {
          analogWrite(BOARD_TFT_BL, brightness / 4);
      }
  }

  // Notification overlay
  drawNotification();

  // Mode dispatch
  switch (currentMode) {
    case MENU:
      handleMenu();
      break;

    case MESSAGING: {
      char kb = readKeyboard();
      char tb = readTrackball();
      if (kb == 0x1B) { currentMode = MENU; drawCurrentMenu(); break; }
      if (kb == 'r' && lastSender.length()) {
        strncpy(targetID, lastSender.c_str(), NODE_ID_LEN);
        targetID[NODE_ID_LEN] = '\0';
        saveIDsToEEPROM();
        startTextInput("Reply to " + lastSender, INPUT_MSG);
        break;
      }
      if (kb == 'n') { startTextInput("New Message", INPUT_MSG); break; }
      if (tb == 'U') { chatScroll++; drawChatView(); }
      if (tb == 'D' && chatScroll > 0) { chatScroll--; drawChatView(); }
      break;
    }

    case TEXT_INPUT:
      handleTextInput(readKeyboard());
      break;

    case TRACKING:
      handleTracking();
      break;

    case SOS_MODE:
      handleSOS();
      break;

    case MESHVIEW:
      handleMeshView();
      break;

    case FIRST_BOOT:
      handleFirstBoot();
      break;
  }

  yield();
}
