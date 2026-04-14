// ═══════════════════════════════════════════════════════════════
// MeshCore - Shared mesh networking library
// ═══════════════════════════════════════════════════════════════
// Used by both T-Deck Plus (main.cpp) and repeater nodes.
// All mesh protocol, crypto, routing, and packet handling
// lives here — one place to maintain.
// ═══════════════════════════════════════════════════════════════
#pragma once

#include <Arduino.h>
#include <AES.h>
#include <CTR.h>
#include <GCM.h>
#include <RadioLib.h>
#include <map>
#include <vector>
#include <deque>

// ─── Configuration (defaults — override per board in Pins.h) ─
#define LORA_FREQ       868.0
#define LORA_BW         125.0
#define LORA_SF         9
#define LORA_CR         5
#ifndef RADIO_POWER
#define RADIO_POWER     20
#endif
#define LORA_POWER      RADIO_POWER
#define LORA_PREAMBLE   8
#define LORA_SYNC       0x12
#ifndef RADIO_CURRENT_LIMIT
#define RADIO_CURRENT_LIMIT 140.0
#endif

#define EEPROM_SIZE     2048  // Expanded for beacon rules (was 512)
#define MAX_NODES       50      // Each node tracks up to 50 peers (was 20)
#define NODE_ID_LEN     4
#define MAX_MSG_LEN     200
#define AES_KEY_LEN     16
#define TTL_DEFAULT     5         // Increased from 3 for larger meshes

#define ACK_TIMEOUT     3000
#define MAX_RETRIES     3
#define CFG_SWITCH_DELAY 2000  // ms delay after CFGGO before applying SF change
#define HB_INTERVAL     30000UL
#define HB_INTERVAL_SOLAR 60000UL // Solar mode: 60s heartbeat (saves power)
#define STALE_TIMEOUT   120000UL  // 2min (was 60s) — less aggressive pruning
#define ROUTE_TIMEOUT   120000UL

#define DEDUP_SIZE      128       // Hash-based dedup ring
#define GCM_NONCE_LEN   12        // AES-GCM nonce size (96 bits)
#define GCM_TAG_LEN     16        // AES-GCM auth tag size (128 bits)
#define MSG_ID_LEN      6         // Message ID length (was 4, now 26^6 = ~309M)
#define COST_WEIGHT     10
#define EEPROM_KEY_ADDR 450
#define EEPROM_MAGIC_ADDR 508
#define EEPROM_MAGIC_VAL  0xA5

// Scalability tunables
#define FWD_JITTER_MIN  50        // Min random delay before forward (ms)
#define FWD_JITTER_MAX  500       // Max random delay before forward (ms)
#define CHANNEL_BUSY_MS 20        // Listen-before-talk threshold
#define MAX_AIRTIME_PCT 10        // Max 10% duty cycle per hour

// GPIO
#define MAX_RELAY_PINS  8
#define MAX_SENSOR_PINS 6

// ─── Data Structures ─────────────────────────────────────────

struct Route {
    String   nextHop;
    uint16_t cost;
    uint32_t lastSeen;
    int      rssi;
};

// Parsed packet header
struct MeshPacket {
    uint16_t dest;
    uint16_t src;
    uint16_t seq;
    uint8_t  ttl;
    String   payload;
};

// Parsed message fields (from payload string)
struct MeshMessage {
    String from;
    String to;
    String mid;
    int    ttl;
    String route;
    String encrypted;  // hex-encoded ciphertext
};

// ─── MeshCore Class ──────────────────────────────────────────

class MeshCore {
public:
    // ─── State (public for firmware access) ──────────────────
    char localID[NODE_ID_LEN + 1]  = "0001";
    char knownNodes[MAX_NODES][NODE_ID_LEN + 1];
    uint8_t knownCount = 0;
    char aes_key_string[AES_KEY_LEN + 1] = "";

    std::map<String, Route> routingTable;
    uint16_t seqCounter = 0;
    volatile bool rxFlag = false;
    int lastRSSI = 0;

    // Stats
    unsigned long packetsReceived = 0;
    unsigned long packetsForwarded = 0;
    unsigned long cmdsExecuted = 0;

    // ─── Callbacks (set by each firmware) ────────────────────
    // Called to physically transmit a raw packet
    typedef void (*TxFunc)(uint8_t* pkt, size_t len);
    TxFunc onTransmitRaw = nullptr;

    // Called to check if channel is free (for listen-before-talk)
    typedef bool (*ChannelFreeFunc)();
    ChannelFreeFunc onChannelFree = nullptr;

    // Called when a new node is discovered
    typedef void (*NodeDiscoverFunc)(const String& id, int rssi);
    NodeDiscoverFunc onNodeDiscovered = nullptr;

    // Called when a decrypted message arrives for us
    typedef void (*MessageFunc)(const String& from, const String& text, int rssi);
    MessageFunc onMessage = nullptr;

    // Called when a CMD arrives for us
    typedef void (*CmdFunc)(const String& from, const String& cmdBody);
    CmdFunc onCmd = nullptr;

    // Called when an ACK arrives
    typedef void (*AckFunc)(const String& from, const String& mid);
    AckFunc onAck = nullptr;

    // Called when a CFG config-change request arrives
    typedef void (*CfgFunc)(const String& cfgType, const String& value, const String& changeId, const String& from);
    CfgFunc onCfg = nullptr;

    // Called when a CFGACK arrives (initiator tracks which nodes responded)
    typedef void (*CfgAckFunc)(const String& cfgType, const String& value, const String& changeId, const String& nodeId);
    CfgAckFunc onCfgAck = nullptr;

    // Called when CFGGO arrives — node should apply the config change
    typedef void (*CfgGoFunc)(const String& cfgType, const String& value, const String& changeId);
    CfgGoFunc onCfgGo = nullptr;

    // Called when a heartbeat is received from a node using our own ID
    typedef void (*IdConflictFunc)(const String& conflictingId, int rssi);
    IdConflictFunc onIdConflict = nullptr;
    bool idConflictDetected = false;

    // Called when ANY heartbeat is received (not just new nodes)
    typedef void (*HeartbeatFunc)(const String& from, int rssi);
    HeartbeatFunc onHeartbeat = nullptr;

    // Board identification code (set by firmware, sent in heartbeat)
    char boardCode[4] = "";  // e.g., "V3", "V4", "L32", "XS3", "TD", "V2"
    char statusFlags[8] = "";  // Compact flags: S=solar, B=beacon scan, G=gateway

    // Runtime spreading factor (may differ from compile-time LORA_SF after a CFG change)
    uint8_t currentSF = LORA_SF;

    // ─── CRC ─────────────────────────────────────────────────
    static uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF);

    // ─── Crypto (AES-128-GCM AEAD) ─────────────────────────
    byte* getAESKey();
    String toHex(const byte* data, int len);
    void hexToBytes(const String& hex, byte* out, int& outLen);
    String encryptMsg(const String& msg);
    String decryptMsg(const String& hexstr);
    String generateMsgID();

    // ─── CFG Auth (GCM-based MAC) ────────────────────────────
    String cfgAuthTag(const String& changeId);
    bool   cfgAuthValid(const String& changeId, const String& tag);

    // ─── Validation ──────────────────────────────────────────
    bool isValidNodeID(const String& s);

    // ─── Dedup (improved: hash-based for O(1) lookup) ────────
    bool isDuplicate(const String& mid);
    void markSeen(const String& mid);

    // ─── Routing ─────────────────────────────────────────────
    void addNode(const String& id);
    std::vector<String> splitRoute(const String& route);
    void updateRouting(const String& from, const String& route, int rssi);
    void pruneStale();
    Route* bestRoute(const String& dest);

    // ─── Packet Building ─────────────────────────────────────
    void transmitPacket(uint16_t dest, const String& payload);   // Local traffic (7% budget)
    void forwardPacket(uint16_t dest, const String& payload);    // Forwarding (full 10% budget)

    // ─── Smart Forwarding (with jitter + directed routing) ───
    void smartForward(const String& from, const String& to,
                      const String& mid, int ttl,
                      const String& route, const String& enc,
                      uint16_t rawDest);

    // ─── Receive Processing ──────────────────────────────────
    // Call this from your ISR-flagged receive handler
    // Returns true if a packet was processed
    bool parseRawPacket(uint8_t* pkt, size_t len, MeshPacket& out);
    bool parsePayloadFields(const String& payload, MeshMessage& out);
    void processPacket(const MeshPacket& pkt);

    // ─── Heartbeat ───────────────────────────────────────────
    void sendHeartbeat();

    // ─── Duty Cycle Tracking ─────────────────────────────────
    bool canTransmit();          // Local traffic: throttled at 7% to reserve budget for forwarding
    bool canForward();           // Forwarding: allowed up to full 10% — repeating is never starved

private:
    // Dedup: hash ring for O(1) lookups
    uint32_t dedupHashes[DEDUP_SIZE] = {0};
    uint8_t dedupIdx = 0;

    // Nonce generation for AES-GCM
    void generateNonce(byte* nonce);

    // Internal TX implementation (shared by transmitPacket and forwardPacket)
    void _doTransmit(uint16_t dest, const String& payload);

    // Duty cycle tracking
    unsigned long txTimeThisHour = 0;
    unsigned long hourStart = 0;
    unsigned long lastTxDuration = 0;

    // Jitter state for pending forwards
    struct PendingForward {
        String payload;
        uint16_t dest;
        unsigned long sendAt;
        bool active = false;
    };
    static const int MAX_PENDING = 8;
    PendingForward pendingFwds[MAX_PENDING];

    uint32_t hashMid(const String& mid);
    bool waitForClearChannel(int maxWaitMs = 200);

public:
    // Process pending jittered forwards — call from loop()
    void processPendingForwards();
};

// Global instance
extern MeshCore mesh;
