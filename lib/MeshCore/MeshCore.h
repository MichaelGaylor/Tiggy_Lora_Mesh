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

#define EEPROM_SIZE     4096  // Expanded for PLC primitive persistence (was 2048 for beacons)
#define MAX_NODES       50      // Each node tracks up to 50 peers (was 20)
#define NODE_ID_LEN     4
#define MAX_MSG_LEN     200
#define AES_KEY_LEN     16
#define TTL_DEFAULT     5         // Increased from 3 for larger meshes

// ACK wait window per attempt. Was 3000ms but the LoRa round-trip after a
// SET command at SF9/125kHz takes ~2s on a quiet link (CMD,RSP TX ~600ms,
// then ACK TX ~200ms, plus propagation/processing) and longer when the
// remote is in solar/light-sleep or mid-transmission of something else.
// At 3s the gateway often started its OWN retry TX while the remote's
// ACK was still in the air — half-duplex collision, both packets lost.
// Verified by running 10 manual sends in succession: some timed out even
// with no Logic Builder activity. 8s × 5 retries = ~40s total patience,
// well under the GUI's 60s backup. Eliminates the retry/ACK collision.
#define ACK_TIMEOUT     8000
#define MAX_RETRIES     5
#define CFG_SWITCH_DELAY 2000  // ms delay after CFGGO before applying SF change
// Heartbeat cadence — compile-time defaults. A board's Pins.h may
// `#define HB_INTERVAL <ms>` before this header is included to override
// per-board, and the runtime can override further via the HB_INTERVAL
// serial command + EEPROM persistence (see HbCfgEEPROM in repeater.cpp).
// Runtime values live in MeshCore::hbIntervalMs / hbIntervalSolarMs
// below; these macros are only the fallback if neither Pins.h nor
// EEPROM has supplied an override.
#ifndef HB_INTERVAL
  #define HB_INTERVAL     30000UL
#endif
#ifndef HB_INTERVAL_SOLAR
  #define HB_INTERVAL_SOLAR 60000UL // Solar mode: 60s heartbeat (saves power)
#endif
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

    // Called for every heartbeat with the parsed extras (boardCode, flags,
    // batteryMv). batteryMv is -1 when the sender doesn't include one.
    // Use this in gateway firmware to forward node telemetry to the GUI.
    typedef void (*HeartbeatExtraFunc)(const String& from, int rssi,
                                       const String& boardCode,
                                       const String& flags,
                                       int batteryMv);
    HeartbeatExtraFunc onHeartbeatExtra = nullptr;

    // Board identification code (set by firmware, sent in heartbeat)
    char boardCode[4] = "";  // e.g., "V3", "V4", "L32", "XS3", "TD", "V2"
    char statusFlags[8] = "";  // Compact flags: S=solar, B=beacon scan, G=gateway

    // Battery voltage in millivolts (set by firmware if it has a battery
    // monitor; -1 means "not reporting"). Included in every outgoing
    // heartbeat as a tagged field "B<mv>".
    int16_t batteryMv = -1;

    // PLC runtime counters — populated by the firmware just before each
    // heartbeat from its plcStats struct (MeshCore stays oblivious to PLC
    // internals). Appended as ",P<scans>:<fires>" when either is non-zero
    // so the gateway can drive the "rule is alive / firing" WORKING
    // indicator from the heartbeat instead of polling. Zero or missing
    // tag = no deployed PLC primitives on this node; the gateway falls
    // back to its previous "deployed = working until heartbeat stops"
    // semantics.
    uint32_t plcScans = 0;
    uint32_t plcFires = 0;

    // 8-bit "state changed" signal for gateway reconciliation. Repeater
    // firmware sets this to its `stateGeneration` global before each
    // heartbeat; sendHeartbeat() rides it as ",G<hex2>" (3 chars).
    // Zero = never-changed baseline (fresh boot before any deploy). The
    // gateway queries PLC,STATUS to fetch the full 32-bit fingerprint
    // only when this value changes — so the frequent HB stays cheap and
    // the expensive fingerprint round-trip only happens on real events.
    uint8_t stateGeneration = 0;

    // Runtime heartbeat interval (ms). Initialised from the compile-time
    // defaults via the inline initialisers below; the firmware reads
    // an EEPROM override at boot (HbCfgEEPROM in repeater.cpp) and
    // applies it here. The scheduling loop in repeater.cpp reads these
    // each tick — that lets a serial HB_INTERVAL command take effect
    // on the very next scheduled heartbeat without a reboot.
    unsigned long hbIntervalMs      = HB_INTERVAL;
    unsigned long hbIntervalSolarMs = HB_INTERVAL_SOLAR;

    // Adaptive heartbeat backoff multiplier. Returns 1, 2, 4, or 8 based
    // on how close txTimeThisHour is to the local 7 % cap (252 s). The
    // scheduling loop multiplies the configured HB interval by this
    // before computing the next-fire time, so under heavy mesh load the
    // node automatically reduces its HB cadence and stops contributing
    // to the cap blowing. Returns 1 (no backoff) on a quiet mesh.
    uint8_t hbIntervalMultiplier();

    // ─── Duty Cycle Visibility ───────────────────────────────
    // Live TX time this hour (ms), with lazy hour-rollover applied so
    // callers always see a fresh, monotonically-sensible value. Callers
    // MUST use these getters — the raw txTimeThisHour field stays private
    // because it is only correct AFTER the 3600 s rollover check runs.
    unsigned long txMsThisHour();
    // Duty cycle as a % of the 7 % LOCAL cap (252 s). Clamped 0..100.
    // Used by HEALTH so operators see 0-100 mapped against the throttle
    // line the node actually starts dropping at, not the 10 % legal cap.
    uint8_t       dcPercent();
    // Monotonic-per-boot counter of packets dropped by canTransmit() /
    // canForward(). Split into two so the HEALTH tick can tell whether
    // the drop was our own traffic or repeat traffic. Clears on reboot;
    // the gateway HEALTH tick tracks deltas against its own snapshot.
    uint16_t      txThrottledLocalCount();
    uint16_t      txThrottledFwdCount();

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
    // Throttle-drop counters — bumped from inside transmitPacket() /
    // forwardPacket() / smartForward() when canTransmit()/canForward()
    // gates return false. Exposed via txThrottledLocalCount() /
    // txThrottledFwdCount() so the gateway HEALTH tick can compare
    // deltas without exposing the raw gate. Not persisted — a boot
    // cleanly resets both to zero.
    uint16_t txThrottledLocal = 0;
    uint16_t txThrottledFwd   = 0;
    // Anchor for X-tag delta accounting on HB TX. Only advances when the
    // outgoing HB actually ships (canTransmit()==true) so drop episodes
    // encoded inside a dropped HB are re-reported on the next successful HB.
    uint16_t txThrottledLocalLastHb = 0;

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
