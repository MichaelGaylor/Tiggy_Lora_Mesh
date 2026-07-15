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
//
// Defaults bumped 2026-07-15 for customer-deployment scaling:
//   HB_INTERVAL       30 s → 300 s (5 min)
//   HB_INTERVAL_SOLAR 60 s → 600 s (10 min)
//
// Rationale: at the old 30 s HB, an N-node mesh generates ~2N HB
// broadcasts per minute — 100 HBs/min at 50 nodes, saturating the
// airway well before customer-target 30-50 node deployments. At
// 300 s HB, 50 nodes generate 10 HBs/min — well within budget. The
// adaptive hbIntervalMultiplier (mult 2×/4×/8× at 50/70/90 % budget)
// stretches this further under load.
//
// Existing deployed nodes with a persisted HbCfgEEPROM record are
// unaffected — loadHbCfg reads their EEPROM override before these
// defaults apply (see repeater.cpp loadHbCfg). Only fresh units
// and firmware upgrades WITHOUT prior HB_INTERVAL config see the
// new defaults.
#ifndef HB_INTERVAL
  #define HB_INTERVAL     300000UL  // 5 min (was 30 s)
#endif
#ifndef HB_INTERVAL_SOLAR
  #define HB_INTERVAL_SOLAR 600000UL // 10 min (was 60 s) — solar saves more power
#endif
// Bumped alongside HB_INTERVAL 30 s→300 s. Must be several HB intervals
// so nodes running clean don't get pruned as stale. 3× HB = 900 s
// covers the normal case, and the firmware's adaptive hb_mult can
// stretch effective HB up to 8× (2400 s) under load — so 1800 s (30 min)
// gives some headroom without dragging pruning out too long.
#define STALE_TIMEOUT   1800000UL  // 30 min (was 2 min)
#define ROUTE_TIMEOUT   1800000UL  // 30 min (was 2 min)

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

    // v4.7 — proper CAD-based LBT for LOCAL originations only.
    // Distinct from onChannelFree above: the RSSI-history version at
    // repeater.cpp:radioChannelFree() is still wired to onChannelFree
    // for the shared _doTransmit path (its return is ignored by
    // waitForClearChannel — best-effort wait). onChannelFreeCad
    // performs a real SX1262 CAD probe (~17 ms at SF9) and IS
    // honoured by transmitPacket() as a single-shot LBT gate.
    // Kept as a separate callback so:
    //   1. Forward path (_doTransmit / smartForward) is bit-for-bit
    //      unchanged — the CAD's ~17 ms RX-blackout side effect never
    //      hits the forwarder's polling loop.
    //   2. Firmware without SX126x scanChannel() support (SX127x-only
    //      boards) can leave this null and get no-op LBT gracefully.
    ChannelFreeFunc onChannelFreeCad = nullptr;

    // v4.7 — SF-derived jitter window for smartForward.
    // Fixed FWD_JITTER_MIN/MAX (50/500 ms) is fine at SF7-SF9 where
    // packet airtime is < 1.2 s. At SF11-SF12 packet airtime is
    // 2-4.5 s and the 500 ms window is too narrow to spread N peers'
    // forwards — collisions still occur. These callbacks let firmware
    // return SF-derived bounds. Callback-not-set = use the #define
    // fallbacks unchanged (safe for older firmware + test rigs).
    // Design point: repeater.cpp's helpers preserve [50, 500] at
    // SF7-SF9 exactly, only widening the window at SF10+. Default-SF
    // deployments see zero latency regression.
    typedef uint16_t (*JitterFunc)();
    JitterFunc onGetJitterMinMs = nullptr;
    JitterFunc onGetJitterMaxMs = nullptr;

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
    // on how close the current bucket usage is to the local 7 % cap
    // (252 s). Under load the node reduces its HB cadence and stops
    // contributing to the cap blowing; during silence the bucket
    // drains and the multiplier recovers automatically. Returns 1 on
    // a quiet mesh.
    uint8_t hbIntervalMultiplier();

    // ─── Duty Cycle Visibility ───────────────────────────────
    // Current bucket usage in ms (0..252000+), with a fresh refill
    // applied so silence credit is always counted. Function name kept
    // as txMsThisHour() for external ABI stability; semantics changed
    // from "cumulative in fixed hour" to "current bucket usage after
    // continuous silence credit". Callers MUST use this getter — the
    // raw txBucketMs field stays private because it is only correct
    // after refillBucket() runs.
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

    // Phase H — role setter (public API). Field itself is private
    // (below in the counter block). Called from repeater's
    // loadRoleCfg/saveRoleCfg so runtime CMD,ROLE takes effect live.
    void setLeafMode(bool on);

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

    // Duty cycle tracking — token bucket (v4.8, 2026-07-15).
    // txBucketMs is current bucket usage in ms (0 = full 252 s credit,
    // 252000 = local 7 % cap reached). Refills at 7 % of elapsed real
    // time (252 s per 3600 s) inside refillBucket(). Silence periods
    // BANK credit for future bursts up to the 252 s ceiling — replaces
    // the fixed-hour bucket that reset to 0 at every hour boundary and
    // gave zero recovery during silence. Both fields default-initialise
    // to 0 → full credit at boot.
    unsigned long txBucketMs = 0;
    unsigned long bucketLastLeakMs = 0;
    unsigned long lastTxDuration = 0;
    // Refill the bucket by 7 % of the time elapsed since the previous
    // refill call. Idempotent — safe to call from every gate check.
    // First call after boot (or any call finding bucketLastLeakMs==0)
    // just initialises the timestamp and returns without leaking.
    void refillBucket();
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
    // Cancel-on-Heard-ACK counter. Every time this node sees an ACK for
    // a packet it had queued to forward, the forward is suppressed and
    // this counter bumps. Exposed so we can measure how often the fix is
    // saving airtime once deployed (bench should see near-100 %
    // suppression on tight 1-hop meshes).
    uint16_t forwardsSuppressed = 0;

    // Phase J — authenticated-forwarding drop counter. Bumped every time
    // a data packet fails AES-GCM auth (wrong shared key, corrupted
    // ciphertext, or foreign-mesh traffic on the same freq/SF/BW). Foreign
    // packets are then dropped rather than cross-forwarded — prevents
    // parallel meshes on the same channel from wasting each other's
    // airtime, and defends against on-channel DoS amplification. Ignored
    // by pre-Phase-J firmware which forwarded blindly.
    uint16_t foreignDropped = 0;

    // Phase H — leaf/router role for large-deployment scaling. When
    // leafMode is true, this node skips ALL relay work (smartForward
    // for data packets, and CFG/CFGACK/CFGGO/ACK forwards) — it
    // remains a full endpoint: originates its own traffic, ACKs
    // packets addressed to it, and applies received CFG/CFGGO
    // locally. Cuts forward traffic ~5-10× in customer deployments
    // where most nodes are edge sensors that don't need to relay.
    // Default false = ROUTER (backwards compat with pre-Phase-H).
    // leafSuppressed counts forward-events skipped by this gate so
    // operators can measure the airtime saving in production.
    bool     leafMode = false;
    uint16_t leafSuppressed = 0;
    // setLeafMode() declared in the public API section above; body in .cpp.

    // Jitter state for pending forwards
    struct PendingForward {
        String payload;
        String mid;                     // for cancel-on-heard-ACK lookup
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
