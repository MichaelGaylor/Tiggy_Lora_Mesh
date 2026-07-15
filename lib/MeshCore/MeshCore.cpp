// ═══════════════════════════════════════════════════════════════
// MeshCore - Shared mesh networking implementation
// ═══════════════════════════════════════════════════════════════
#include "MeshCore.h"

// Global instance
MeshCore mesh;

// ═══════════════════════════════════════════════════════════════
// CRC-16-CCITT
// ═══════════════════════════════════════════════════════════════

uint16_t MeshCore::crc16(const uint8_t* data, size_t len, uint16_t seed) {
    uint16_t crc = seed;
    while (len--) {
        crc ^= (*data++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

// ═══════════════════════════════════════════════════════════════
// Crypto helpers
// ═══════════════════════════════════════════════════════════════

byte* MeshCore::getAESKey() { return (byte*)aes_key_string; }

String MeshCore::toHex(const byte* data, int len) {
    String s;
    s.reserve(len * 2);
    const char hd[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        s += hd[(data[i] >> 4) & 0xF];
        s += hd[data[i] & 0xF];
    }
    return s;
}

void MeshCore::hexToBytes(const String& hex, byte* out, int& outLen) {
    int l = hex.length() / 2;
    for (int i = 0; i < l; i++) {
        char hi = hex.charAt(2 * i), lo = hex.charAt(2 * i + 1);
        out[i] = ((hi <= '9' ? hi - '0' : toupper(hi) - 'A' + 10) << 4)
               | (lo <= '9' ? lo - '0' : toupper(lo) - 'A' + 10);
    }
    outLen = l;
}

// Generate random nonce using ESP32 hardware RNG
void MeshCore::generateNonce(byte* nonce) {
    for (int i = 0; i < GCM_NONCE_LEN; i += 4) {
        uint32_t r = esp_random();
        int remaining = (GCM_NONCE_LEN - i < 4) ? GCM_NONCE_LEN - i : 4;
        memcpy(nonce + i, &r, remaining);
    }
}

// AES-128-GCM encrypt — output: hex(nonce || ciphertext || tag)
String MeshCore::encryptMsg(const String& msg) {
    int mlen = msg.length();
    if (mlen > MAX_MSG_LEN || mlen == 0) return "";

    byte nonce[GCM_NONCE_LEN];
    generateNonce(nonce);

    GCM<AES128> gcm;
    gcm.setKey(getAESKey(), AES_KEY_LEN);
    gcm.setIV(nonce, GCM_NONCE_LEN);

    uint8_t ciphertext[MAX_MSG_LEN];
    gcm.encrypt(ciphertext, (const uint8_t*)msg.c_str(), mlen);

    byte tag[GCM_TAG_LEN];
    gcm.computeTag(tag, GCM_TAG_LEN);
    gcm.clear();

    // Assemble: nonce(12) || ciphertext(N) || tag(16)
    int blobLen = GCM_NONCE_LEN + mlen + GCM_TAG_LEN;
    byte blob[GCM_NONCE_LEN + MAX_MSG_LEN + GCM_TAG_LEN];
    memcpy(blob, nonce, GCM_NONCE_LEN);
    memcpy(blob + GCM_NONCE_LEN, ciphertext, mlen);
    memcpy(blob + GCM_NONCE_LEN + mlen, tag, GCM_TAG_LEN);

    return toHex(blob, blobLen);
}

// AES-128-GCM decrypt — input: hex(nonce || ciphertext || tag)
String MeshCore::decryptMsg(const String& hexstr) {
    uint8_t blob[GCM_NONCE_LEN + MAX_MSG_LEN + GCM_TAG_LEN];
    int blobLen;
    hexToBytes(hexstr, blob, blobLen);

    // Minimum: nonce(12) + 1 byte ciphertext + tag(16) = 29
    if (blobLen < GCM_NONCE_LEN + GCM_TAG_LEN + 1) return "";

    int cipherLen = blobLen - GCM_NONCE_LEN - GCM_TAG_LEN;
    byte* nonce      = blob;
    byte* ciphertext = blob + GCM_NONCE_LEN;
    byte* tag        = blob + GCM_NONCE_LEN + cipherLen;

    GCM<AES128> gcm;
    gcm.setKey(getAESKey(), AES_KEY_LEN);
    gcm.setIV(nonce, GCM_NONCE_LEN);

    uint8_t plaintext[MAX_MSG_LEN];
    gcm.decrypt(plaintext, ciphertext, cipherLen);

    if (!gcm.checkTag(tag, GCM_TAG_LEN)) {
        gcm.clear();
        return "";  // Authentication failed — tampered or wrong key
    }
    gcm.clear();

    return String((char*)plaintext, cipherLen);
}

String MeshCore::generateMsgID() {
    String s;
    for (int i = 0; i < MSG_ID_LEN; i++) s += (char)random(65, 91);
    return s;
}

// ═══════════════════════════════════════════════════════════════
// CFG Auth — key-group verification for config change packets
// ═══════════════════════════════════════════════════════════════

String MeshCore::cfgAuthTag(const String& changeId) {
    // GCM-based MAC: encrypt zero bytes with changeId as AAD
    GCM<AES128> gcm;
    gcm.setKey(getAESKey(), AES_KEY_LEN);
    // Deterministic IV from changeId (reproducible for verification)
    byte iv[GCM_NONCE_LEN];
    memset(iv, 0, GCM_NONCE_LEN);
    int copyLen = (int)changeId.length() < GCM_NONCE_LEN ? (int)changeId.length() : GCM_NONCE_LEN;
    memcpy(iv, changeId.c_str(), copyLen);
    gcm.setIV(iv, GCM_NONCE_LEN);
    gcm.addAuthData(changeId.c_str(), changeId.length());
    byte tag[GCM_TAG_LEN];
    gcm.computeTag(tag, GCM_TAG_LEN);
    gcm.clear();
    // Return first 4 bytes = 8 hex chars (stronger than old 4 hex)
    return toHex(tag, 4);
}

bool MeshCore::cfgAuthValid(const String& changeId, const String& tag) {
    return tag == cfgAuthTag(changeId);
}

// ═══════════════════════════════════════════════════════════════
// Validation
// ═══════════════════════════════════════════════════════════════

bool MeshCore::isValidNodeID(const String& s) {
    if ((int)s.length() != NODE_ID_LEN) return false;
    for (int i = 0; i < NODE_ID_LEN; i++) {
        char c = s.charAt(i);
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Dedup — hash-based for O(1) lookup, handles 3x more MIDs
// ═══════════════════════════════════════════════════════════════

uint32_t MeshCore::hashMid(const String& mid) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (int i = 0; i < (int)mid.length(); i++) {
        h ^= (uint8_t)mid[i];
        h *= 16777619u;
    }
    return h | 1;  // Never zero (zero = empty slot)
}

bool MeshCore::isDuplicate(const String& mid) {
    uint32_t h = hashMid(mid);
    for (int i = 0; i < DEDUP_SIZE; i++)
        if (dedupHashes[i] == h) return true;
    return false;
}

void MeshCore::markSeen(const String& mid) {
    dedupHashes[dedupIdx] = hashMid(mid);
    dedupIdx = (dedupIdx + 1) % DEDUP_SIZE;
}

// ═══════════════════════════════════════════════════════════════
// Node tracking
// ═══════════════════════════════════════════════════════════════

void MeshCore::addNode(const String& id) {
    String incoming = id.substring(0, NODE_ID_LEN);
    incoming.trim();
    if (!isValidNodeID(incoming)) return;
    for (uint8_t i = 0; i < knownCount; i++)
        if (String(knownNodes[i]) == incoming) return;
    if (knownCount < MAX_NODES) {
        strncpy(knownNodes[knownCount], incoming.c_str(), NODE_ID_LEN);
        knownNodes[knownCount][NODE_ID_LEN] = '\0';
        knownCount++;
        if (onNodeDiscovered) onNodeDiscovered(incoming, lastRSSI);
    }
}

// ═══════════════════════════════════════════════════════════════
// Routing — RSSI-weighted with directed forwarding
// ═══════════════════════════════════════════════════════════════

std::vector<String> MeshCore::splitRoute(const String& route) {
    std::vector<String> nodes;
    int start = 0, end;
    while ((end = route.indexOf(',', start)) != -1) {
        nodes.push_back(route.substring(start, end));
        start = end + 1;
    }
    nodes.push_back(route.substring(start));
    return nodes;
}

void MeshCore::updateRouting(const String& from, const String& route, int rssi) {
    std::vector<String> nodes = splitRoute(route);
    for (size_t i = 0; i < nodes.size(); i++) {
        String dest = nodes[i];
        dest.trim();
        if (!isValidNodeID(dest) || dest == String(localID)) continue;
        String nextHop = (i == 0) ? from : nodes[i - 1];
        uint16_t cost = nodes.size() - i;
        int newScore = (cost * COST_WEIGHT) - rssi;
        auto it = routingTable.find(dest);
        if (it == routingTable.end()) {
            // New route — check table isn't full
            if (routingTable.size() >= MAX_NODES) pruneStale();
            if (routingTable.size() >= MAX_NODES) continue;  // Still full after prune
            routingTable[dest] = { nextHop, cost, (uint32_t)millis(), rssi };
        } else if (newScore < ((it->second.cost * COST_WEIGHT) - it->second.rssi)) {
            routingTable[dest] = { nextHop, cost, (uint32_t)millis(), rssi };
        }
    }
}

void MeshCore::pruneStale() {
    unsigned long now = millis();
    for (auto it = routingTable.begin(); it != routingTable.end(); )
        it = (now - it->second.lastSeen > STALE_TIMEOUT) ? routingTable.erase(it) : ++it;
}

Route* MeshCore::bestRoute(const String& dest) {
    auto it = routingTable.find(dest);
    if (it == routingTable.end()) return nullptr;
    if (millis() - it->second.lastSeen > ROUTE_TIMEOUT) return nullptr;
    return &it->second;
}

// ═══════════════════════════════════════════════════════════════
// Duty Cycle Tracking (legal compliance)
// ═══════════════════════════════════════════════════════════════

// v4.8 — token-bucket duty-cycle gate.
//
// txBucketMs holds current bucket usage in ms. Silence periods leak the
// bucket back down at 7 % of real elapsed time (refillBucket), so a
// burst-then-quiet pattern recovers smoothly instead of waiting for a
// fixed hour boundary. Boot state is 0 (full credit). Legally still
// ≤ 10 % in any hour because the leak rate caps net TX at 7 % (local)
// or 10 % (forward) of any hour containing continuous traffic.
bool MeshCore::canTransmit() {
    refillBucket();
    // Local traffic capped at 7% (252s of bucket) — reserves 3% for forwarding
    return txBucketMs < 252000UL;
}

bool MeshCore::canForward() {
    refillBucket();
    // Forwarding allowed up to the full 10% legal limit (360s of bucket).
    // Repeating other nodes' packets must NEVER be starved by local traffic.
    return txBucketMs < 360000UL;
}

// ═══════════════════════════════════════════════════════════════
// Listen-Before-Talk (channel busy detection)
// ═══════════════════════════════════════════════════════════════

bool MeshCore::waitForClearChannel(int maxWaitMs) {
    if (!onChannelFree) return true;  // No callback = assume clear
    unsigned long start = millis();
    while (millis() - start < (unsigned long)maxWaitMs) {
        if (onChannelFree()) return true;
        delay(5);
    }
    return false;  // Channel stayed busy
}

// ═══════════════════════════════════════════════════════════════
// Packet Building & Transmission
// ═══════════════════════════════════════════════════════════════

// Internal shared TX — builds packet and sends
void MeshCore::_doTransmit(uint16_t dest, const String& payload) {
    if (!onTransmitRaw) return;

    uint16_t src = strtol(localID, nullptr, 16);
    uint16_t seq = seqCounter++;
    uint8_t pkt[256];
    // Defensive: payload longer than the buffer minus header overflows
    // the stack canary and crashes with "Stack smashing protect failure".
    // Truncate (or drop) before memcpy. A LoRa frame can't carry more
    // than ~255 bytes anyway, so anything longer is already a logic
    // error in the caller — log + drop the packet rather than corrupt
    // the stack and reboot the node.
    const size_t HEADER = 7;
    size_t plen = payload.length();
    if (plen > sizeof(pkt) - HEADER) {
        // Cannot fit. Caller should have sized it themselves. Drop and
        // log over USB so the developer notices. (Don't transmit a
        // half-truncated packet — receiver couldn't decrypt it.)
        //
        // Note: gateway.py used to suppress this line to DEBUG level
        // because it doesn't start with "PKT,". gateway_gui.py surfaces
        // it explicitly via a _CONSOLE_DELIVERY_KEYWORDS entry so the
        // operator can see when their POLL/SETPOINT chunks are being
        // dropped by the size cap (this is how the P2/P4 mystery was
        // finally caught — every over-cap payload emits this line but
        // it was invisible above the console filter).
        Serial.print("TX_DROP: payload ");
        Serial.print(plen);
        Serial.print("B > frame budget ");
        Serial.println(sizeof(pkt) - HEADER);
        return;
    }
    int pktLen = HEADER + (int)plen;
    pkt[0] = dest >> 8;    pkt[1] = dest & 0xFF;
    pkt[2] = src >> 8;     pkt[3] = src & 0xFF;
    pkt[4] = seq >> 8;     pkt[5] = seq & 0xFF;
    pkt[6] = TTL_DEFAULT;
    memcpy(pkt + HEADER, payload.c_str(), plen);

    waitForClearChannel(200);

    unsigned long txStart = millis();
    onTransmitRaw(pkt, pktLen);
    // Refill (leak) happens through *now*, which includes the TX we just
    // sent — so 7 % of tx_duration is credited back immediately. Net
    // charge per packet is 0.93 * tx_duration. At steady 7 % duty this
    // leaves the bucket flat; at 10 % duty it climbs at 0.7 * airtime.
    // Both behaviours are what we want.
    refillBucket();
    txBucketMs += (millis() - txStart);
}

// Local-originated traffic (messages, heartbeats, setpoints) — capped at 7%
void MeshCore::transmitPacket(uint16_t dest, const String& payload) {
    if (!canTransmit()) {
        // Silent drop is a visibility bug — see ED53 blackout incident.
        // Bump a counter so the HEALTH tick in repeater.cpp can emit
        // TX_THROTTLED with the delta once per minute. Saturate rather
        // than wrap so a runaway loop stays at UINT16_MAX (65535 drops
        // in an hour is already pathological).
        if (txThrottledLocal < 0xFFFF) txThrottledLocal++;
        return;
    }
    // v4.7 — single-shot CAD-based LBT for local originations.
    //
    // ACK payloads bypass LBT: peers that queued smartForward of the
    // original message are running the Phase C cancel-on-heard-ACK
    // countdown against 50-500 ms jitter. Delaying the ACK by ~17 ms
    // (SF9 CAD) is fine, but a drop-and-count silent-return would
    // leave the peer's forward pending → fires → defeats Phase C.
    // Ship ACKs unconditionally; the caller (auto-ACK at processPacket
    // line ~690) is already immediate-after-RX and won't collide with
    // itself.
    //
    // Non-ACK local TX: one CAD probe. If channel is busy, drop and
    // count (surfaces via TX_THROTTLED HEALTH tick, same counter as
    // duty-cycle drops). No retry — a delay() here would block
    // pollDio1() in loop() and abort in-progress RX (design tradeoff
    // caught by adversarial verify). Single-shot keeps the LBT
    // benefit without the RX blackout cost.
    //
    // If onChannelFreeCad is null (SX127x-only board, unusual test
    // firmware, or intentional opt-out) the LBT is skipped — behaves
    // exactly like today.
    if (onChannelFreeCad && !payload.startsWith("ACK,")) {
        if (!onChannelFreeCad()) {
            if (txThrottledLocal < 0xFFFF) txThrottledLocal++;
            return;
        }
    }
    _doTransmit(dest, payload);
}

// Forwarded traffic (repeating others' packets) — full 10% budget
void MeshCore::forwardPacket(uint16_t dest, const String& payload) {
    if (!canForward()) {
        if (txThrottledFwd < 0xFFFF) txThrottledFwd++;
        return;
    }
    _doTransmit(dest, payload);
}

// ═══════════════════════════════════════════════════════════════
// Smart Forwarding — directed routing + jitter
// ═══════════════════════════════════════════════════════════════
// Instead of immediately rebroadcasting (flooding), we:
//  1. Check if we have a route to the destination
//  2. If yes: forward only to the next hop (directed)
//  3. If no: broadcast with jitter delay (controlled flood)
//  4. Random jitter prevents collision storms

void MeshCore::smartForward(const String& from, const String& to,
                            const String& mid, int ttl,
                            const String& route, const String& enc,
                            uint16_t rawDest) {
    if (ttl <= 1) return;
    // Phase H — leaf nodes never relay third-party data. Runs before
    // canForward() and any String allocation so a leaf costs zero on
    // every heard packet (single-branch early return). Endpoint
    // functions (own originations, auto-ACK, receiving CFG/CFGGO for
    // ourselves) are unaffected — they don't call smartForward.
    if (leafMode) {
        if (leafSuppressed < 0xFFFF) leafSuppressed++;
        return;
    }
    if (!canForward()) {
        // Same silent-drop bug that hits forwardPacket() — bump the
        // forward counter so smartForward drops surface in TX_THROTTLED
        // instead of vanishing into the mesh.
        if (txThrottledFwd < 0xFFFF) txThrottledFwd++;
        return;
    }

    String newRoute = route + "," + String(localID);
    String payload = from + "," + to + "," + mid + "," +
                     String(ttl - 1) + "," + newRoute + "," + enc;

    // Determine destination
    uint16_t fwdDest;
    Route* r = bestRoute(to);
    if (r && r->nextHop != from) {
        // Directed forward: send to specific next hop
        fwdDest = strtol(r->nextHop.c_str(), nullptr, 16);
    } else {
        // No route or route goes back to sender: broadcast
        fwdDest = 0xFFFF;
    }

    // Queue with jitter delay. SF-derived bounds when callbacks are
    // wired (see repeater.cpp:fwdJitterMinMs/fwdJitterMaxMs) — falls
    // back to the fixed [FWD_JITTER_MIN, FWD_JITTER_MAX] #defines
    // when unset. The `hi <= lo` guard covers any clamp-collision
    // corner from a mis-computed callback.
    uint16_t lo = onGetJitterMinMs ? onGetJitterMinMs() : FWD_JITTER_MIN;
    uint16_t hi = onGetJitterMaxMs ? onGetJitterMaxMs() : FWD_JITTER_MAX;
    if (hi <= lo) hi = lo + 50;
    unsigned long jitter = random(lo, hi);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pendingFwds[i].active) {
            pendingFwds[i].payload = payload;
            pendingFwds[i].mid     = mid;       // for cancel-on-heard-ACK
            pendingFwds[i].dest    = fwdDest;
            pendingFwds[i].sendAt  = millis() + jitter;
            pendingFwds[i].active  = true;
            return;
        }
    }
    // Queue full — drop (better than collision storm)
}

void MeshCore::processPendingForwards() {
    unsigned long now = millis();
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pendingFwds[i].active && now >= pendingFwds[i].sendAt) {
            forwardPacket(pendingFwds[i].dest, pendingFwds[i].payload);
            pendingFwds[i].active = false;
            packetsForwarded++;
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Packet Parsing
// ═══════════════════════════════════════════════════════════════

bool MeshCore::parseRawPacket(uint8_t* pkt, size_t len, MeshPacket& out) {
    if (len < 7) return false;
    out.dest    = (pkt[0] << 8) | pkt[1];
    out.src     = (pkt[2] << 8) | pkt[3];
    out.seq     = (pkt[4] << 8) | pkt[5];
    out.ttl     =  pkt[6];
    out.payload.reserve(len - 7);
    out.payload = "";
    for (size_t i = 7; i < len; i++) out.payload += (char)pkt[i];
    return true;
}

bool MeshCore::parsePayloadFields(const String& payload, MeshMessage& out) {
    // Format: from,to,mid,ttl,route,hexdata
    int c1 = payload.indexOf(',');
    if (c1 < 0) return false;
    int c2 = payload.indexOf(',', c1 + 1);
    if (c2 < 0) return false;
    int c3 = payload.indexOf(',', c2 + 1);
    if (c3 < 0) return false;
    int c4 = payload.indexOf(',', c3 + 1);
    if (c4 < 0) return false;
    int cLast = payload.lastIndexOf(',');
    if (cLast <= c4) return false;

    out.from      = payload.substring(0, c1);
    out.to        = payload.substring(c1 + 1, c2);
    out.mid       = payload.substring(c2 + 1, c3);
    out.ttl       = payload.substring(c3 + 1, c4).toInt();
    out.route     = payload.substring(c4 + 1, cLast);
    out.encrypted = payload.substring(cLast + 1);
    return true;
}

void MeshCore::processPacket(const MeshPacket& pkt) {
    uint16_t myAddr = strtol(localID, nullptr, 16);

    // Skip self-received packets — SX1262 DIO1 fires for TX_DONE too,
    // which sets rxFlag and causes us to read back our own transmission
    if (pkt.src == myAddr) return;

    packetsReceived++;

    // ─── ACK packets ─────────────────────────────────────────
    if (pkt.payload.startsWith("ACK,")) {
        // Cancel-on-Heard-ACK: any node that had queued a smartForward
        // of the original message will hear this ACK travelling back
        // (both endpoints already proved reachability to us — that's
        // why we heard the original in the first place). Suppress the
        // pending forward — the destination clearly got the packet.
        // Runs BEFORE the dest-split below so it fires whether the ACK
        // is for us, for a peer, or would be dedup-dropped later.
        // Scope note: only "ACK," is checked; CFG/CFGACK/CFGGO use
        // forwardPacket() directly (never touch pendingFwds[]) so
        // they're not at risk of accidental cancellation here.
        int _ackC = pkt.payload.indexOf(',', 4);
        if (_ackC > 0) {
            String ackedMid = pkt.payload.substring(_ackC + 1);
            for (int i = 0; i < MAX_PENDING; i++) {
                if (pendingFwds[i].active && pendingFwds[i].mid == ackedMid) {
                    pendingFwds[i].active = false;
                    if (forwardsSuppressed < 0xFFFF) forwardsSuppressed++;
                }
            }
        }
        if (pkt.dest == myAddr && onAck) {
            // ACK for us
            int c1 = pkt.payload.indexOf(',', 4);
            if (c1 > 0) {
                String ackFrom = pkt.payload.substring(4, c1);
                String ackMid  = pkt.payload.substring(c1 + 1);
                onAck(ackFrom, ackMid);
            }
        } else if (pkt.dest != myAddr && pkt.dest != 0xFFFF && pkt.ttl > 1
                   && !leafMode) {   // Phase H — leaves don't relay ACKs
            // Forward ACK — dedup to prevent infinite forwarding loops
            // ACK payload "ACK,from,mid" is unique per original message
            String ackKey = pkt.payload;
            if (!isDuplicate(ackKey)) {
                markSeen(ackKey);
                String payload = pkt.payload;
                forwardPacket(pkt.dest, payload);
                packetsForwarded++;
            }
        }
        return;
    }

    // ─── CFG packets (two-phase config change protocol) ──────
    // Auth tag appended: only nodes with matching AES key will accept
    if (pkt.payload.startsWith("CFG,")) {
        // Format: CFG,<type>,<value>,<changeId>,<initiatorID>,<authTag>
        int c1 = pkt.payload.indexOf(',', 4);
        int c2 = (c1 > 0) ? pkt.payload.indexOf(',', c1 + 1) : -1;
        int c3 = (c2 > 0) ? pkt.payload.indexOf(',', c2 + 1) : -1;
        int c4 = (c3 > 0) ? pkt.payload.indexOf(',', c3 + 1) : -1;
        if (c1 > 0 && c2 > 0 && c3 > 0 && c4 > 0) {
            String cfgType  = pkt.payload.substring(4, c1);
            String value    = pkt.payload.substring(c1 + 1, c2);
            String changeId = pkt.payload.substring(c2 + 1, c3);
            String from     = pkt.payload.substring(c3 + 1, c4);
            String authTag  = pkt.payload.substring(c4 + 1);

            // Verify auth — reject if wrong key group
            if (!cfgAuthValid(changeId, authTag)) return;

            if (!isDuplicate(changeId)) {
                markSeen(changeId);
                if (onCfg) onCfg(cfgType, value, changeId, from);

                // Send CFGACK back to the initiator
                String ackBody = "CFGACK," + cfgType + "," + value + "," + changeId + "," + String(localID);
                uint16_t destAddr = strtol(from.c_str(), nullptr, 16);
                transmitPacket(destAddr, ackBody);

                // Rebroadcast CFG so other nodes hear it. Phase H leaf
                // nodes skip this — they still apply the CFG locally
                // (onCfg above) but don't propagate to peers behind
                // them. Commissioning invariant: at least one router
                // per 2-hop cluster.
                if (pkt.ttl > 1 && !leafMode) {
                    forwardPacket(0xFFFF, pkt.payload);
                    packetsForwarded++;
                }
            }
        }
        return;
    }

    if (pkt.payload.startsWith("CFGACK,")) {
        // Format: CFGACK,<type>,<value>,<changeId>,<nodeId>
        if (pkt.dest == myAddr && onCfgAck) {
            int c1 = pkt.payload.indexOf(',', 7);
            int c2 = (c1 > 0) ? pkt.payload.indexOf(',', c1 + 1) : -1;
            int c3 = (c2 > 0) ? pkt.payload.indexOf(',', c2 + 1) : -1;
            if (c1 > 0 && c2 > 0 && c3 > 0) {
                String cfgType  = pkt.payload.substring(7, c1);
                String value    = pkt.payload.substring(c1 + 1, c2);
                String changeId = pkt.payload.substring(c2 + 1, c3);
                String nodeId   = pkt.payload.substring(c3 + 1);
                onCfgAck(cfgType, value, changeId, nodeId);
            }
        } else if (pkt.dest != myAddr && pkt.dest != 0xFFFF && pkt.ttl > 1
                   && !leafMode) {   // Phase H — leaves don't relay CFGACKs
            // Forward CFGACK toward the initiator
            forwardPacket(pkt.dest, pkt.payload);
            packetsForwarded++;
        }
        return;
    }

    if (pkt.payload.startsWith("CFGGO,")) {
        // Format: CFGGO,<type>,<value>,<changeId>,<authTag>
        int c1 = pkt.payload.indexOf(',', 6);
        int c2 = (c1 > 0) ? pkt.payload.indexOf(',', c1 + 1) : -1;
        int c3 = (c2 > 0) ? pkt.payload.indexOf(',', c2 + 1) : -1;
        if (c1 > 0 && c2 > 0 && c3 > 0) {
            String cfgType  = pkt.payload.substring(6, c1);
            String value    = pkt.payload.substring(c1 + 1, c2);
            String changeId = pkt.payload.substring(c2 + 1, c3);
            String authTag  = pkt.payload.substring(c3 + 1);

            // Verify auth — reject if wrong key group
            if (!cfgAuthValid(changeId, authTag)) return;

            String dedupKey = "GO" + changeId;
            if (!isDuplicate(dedupKey)) {
                markSeen(dedupKey);
                if (onCfgGo) onCfgGo(cfgType, value, changeId);

                // Rebroadcast CFGGO so all nodes hear it. Phase H
                // leaves apply the CFGGO locally (onCfgGo above) but
                // don't re-broadcast — same invariant as CFG.
                if (pkt.ttl > 1 && !leafMode) {
                    forwardPacket(0xFFFF, pkt.payload);
                    packetsForwarded++;
                }
            }
        }
        return;
    }

    // ─── Heartbeat ───────────────────────────────────────────
    if (pkt.payload.startsWith("HB,")) {
        String rest = pkt.payload.substring(3);
        rest.trim();
        // Parse: "A1B2" (basic) | "A1B2,V3" | "A1B2,V3,SR" | "A1B2,V3,SR,B4067"
        // Fields are positional except for tagged ones (currently "B<mv>" for
        // battery), so order/optionality of tagged fields doesn't matter.
        String hbFrom, hbBoard, hbFlags;
        int hbBatt = -1;
        {
            int idx = 0;
            int start = 0;
            while (true) {
                int comma = rest.indexOf(',', start);
                String tok = (comma < 0) ? rest.substring(start) : rest.substring(start, comma);
                tok.trim();
                if (tok.length() > 0) {
                    if (tok.charAt(0) == 'B' && tok.length() > 1) {
                        // Tagged battery field "B<mv>"
                        hbBatt = tok.substring(1).toInt();
                    } else if (idx == 0) {
                        hbFrom = tok;
                    } else if (hbBoard.length() == 0) {
                        hbBoard = tok;
                    } else if (hbFlags.length() == 0) {
                        hbFlags = tok;
                    }
                }
                idx++;
                if (comma < 0) break;
                start = comma + 1;
            }
        }
        if (isValidNodeID(hbFrom)) {
            // Collision: another node is using our ID
            if (hbFrom == String(localID)) {
                idConflictDetected = true;
                if (onIdConflict) onIdConflict(hbFrom, lastRSSI);
                return;  // Don't add ourselves as a peer
            }
            addNode(hbFrom);
            updateRouting(hbFrom, hbFrom, lastRSSI);
            if (onHeartbeat)      onHeartbeat(hbFrom, lastRSSI);
            if (onHeartbeatExtra) onHeartbeatExtra(hbFrom, lastRSSI, hbBoard, hbFlags, hbBatt);
        }
        return;
    }

    // ─── Data packets ────────────────────────────────────────
    MeshMessage msg;
    if (!parsePayloadFields(pkt.payload, msg)) return;

    // ─── Phase G-lite — cancel-on-heard-forward (DIRECTED↔DIRECTED) ───
    // If we had a pending DIRECTED (unicast-routed) forward queued for
    // this MID and just heard someone else rebroadcast it, our forward
    // is redundant — cancel it, same principle as Phase C's
    // cancel-on-heard-ACK. Runs BEFORE the isDuplicate return below
    // because the trigger for cancel IS the duplicate reception.
    //
    // BOTH sides must be DIRECTED for cancel to fire:
    //
    //   1. `pkt.dest != 0xFFFF` — the heard packet must be a directed
    //      forward, not a blind broadcast. A broadcast rebroadcast
    //      from a peer is NOT proof anyone with a route to the final
    //      destination handled it — could be a peer with no route
    //      that broadcast in hope. Cancelling on unproven effort
    //      risks losing the packet in mixed-topology cases like
    //      S→I1→G (I1 has route, DIRECTED) plus S→I2 (I2 no route,
    //      BROADCAST): if I2 fires first and G isn't in I2's radio
    //      range, I1's cancel would strand G. Design verifier
    //      finding #2 caught exactly this.
    //
    //   2. `pendingFwds[i].dest != 0xFFFF` — our own pending must
    //      also be directed. Broadcast forwards continue to flood
    //      to preserve parallel-diversity coverage in sparse meshes.
    //      Scale-win for broadcasts comes from Phase H (role
    //      separation) not cancel-on-heard.
    //
    // When both are directed and the MIDs match, cancel is safe:
    // the route was computed by bestRoute() at smartForward time,
    // so if someone else already forwarded it to their own chosen
    // next hop for the same final destination, we'd be pure
    // duplication. Same-MID matches also stay within the dedup
    // ring's random-string space (26⁶ collisions are astronomical),
    // so no cross-source false positives.
    if (pkt.dest != 0xFFFF) {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (!pendingFwds[i].active) continue;
            if (pendingFwds[i].mid  != msg.mid) continue;
            if (pendingFwds[i].dest == 0xFFFF)  continue;
            pendingFwds[i].active = false;
            if (forwardsSuppressed < 0xFFFF) forwardsSuppressed++;
        }
    }

    // Dedup
    if (isDuplicate(msg.mid)) return;
    markSeen(msg.mid);

    // ─── Phase J — authenticated forwarding ──────────────────────────
    // Attempt AES-GCM decrypt on EVERY data packet, not just those
    // destined for us. The auth tag verification is our only signal
    // that the packet belongs to our mesh (i.e., was written with our
    // shared AES key). If decrypt fails, the packet is either:
    //   (a) from a foreign mesh on the same freq/SF (different key),
    //   (b) tampered / RF-corrupted past the LoRa CRC,
    //   (c) an intentional on-channel DoS attempt.
    // In every case, we don't want to spread it — drop before addNode,
    // updateRouting, ACK, or smartForward can amplify the wasted work.
    //
    // CPU cost: ~600 µs per decrypt (Weatherley GCM<AES128>, no HW
    // accel). At 30-50 node meshes ~3000 pkts/hr → ~1.8 s CPU/hr =
    // 0.05 % duty. Negligible. Verified in Phase J design workflow.
    //
    // Scope note: HB packets are handled in a separate plaintext
    // branch above (~line 645) and are NOT auth-gated here. Foreign
    // HBs can still pollute knownNodes[] — Phase K (HB HMAC) would
    // close that if needed. Documented consciously.
    String plain = decryptMsg(msg.encrypted);
    if (plain.length() == 0) {
        // Foreign, corrupted, or tampered — drop silently. Bump the
        // counter so the operator can distinguish "quiet mesh" from
        // "mesh under sustained cross-channel interference".
        if (foreignDropped < 0xFFFF) foreignDropped++;
        if (pkt.dest == myAddr) {
            // Only WARN when the packet was addressed to us — a
            // foreign broadcast is expected noise and should stay quiet.
            Serial.println("WARN: decrypt failed from " + msg.from + " mid=" + msg.mid +
                " keyLen=" + String(strlen(aes_key_string)) +
                " encLen=" + String(msg.encrypted.length()));
        }
        return;
    }

    // Packet authenticated — safe to update routing and dispatch.
    addNode(msg.from);
    updateRouting(msg.from, msg.route, lastRSSI);

    // For us? Dispatch (decrypt already done above)
    if (pkt.dest == myAddr || pkt.dest == 0xFFFF) {
        if (plain.startsWith("CMD,") && onCmd) {
            onCmd(msg.from, plain.substring(4));
            cmdsExecuted++;
        } else if (onMessage) {
            onMessage(msg.from, plain, lastRSSI);
        }

        // Send ACK for directed messages
        if (pkt.dest == myAddr) {
            String ackBody = "ACK," + msg.from + "," + msg.mid;
            transmitPacket(strtol(msg.from.c_str(), nullptr, 16), ackBody);
        }
    }

    // Forward if not exclusively for us — packet is guaranteed
    // authenticated by the Phase J gate above, so we're not amplifying
    // foreign or malicious traffic.
    if (pkt.dest != myAddr) {
        smartForward(msg.from, msg.to, msg.mid, msg.ttl,
                     msg.route, msg.encrypted, pkt.dest);
    }
}

// ═══════════════════════════════════════════════════════════════
// Heartbeat
// ═══════════════════════════════════════════════════════════════

// Adaptive heartbeat backoff multiplier.
//
// Thresholds key off the LOCAL 7 % cap (252 s/hr) rather than the full
// 10 % forward cap (360 s/hr). Crossing 252 s is when this node's own
// heartbeats start getting silently dropped by canTransmit. We want
// the multiplier to ramp up BEFORE that line so the node sheds load
// itself rather than getting cut off. By 90 % of the local cap we're
// at 8× — combined with the 30 min ceiling enforced at the caller,
// the node still emits a heartbeat at least every 30 min even when
// the mesh is saturated, so the operator never loses visibility.
uint8_t MeshCore::hbIntervalMultiplier() {
    // Go through txMsThisHour() so a fresh silence-refill applies before
    // the threshold check — a node that fell quiet after a burst will
    // see its HB multiplier drop back down as the bucket drains,
    // rather than staying pinned to the peak until the next TX.
    unsigned long t = txMsThisHour();
    if (t < 126000UL) return 1;   // < 50 % of 252 s — plenty of headroom
    if (t < 176000UL) return 2;   // 50-70 % — slow down
    if (t < 227000UL) return 4;   // 70-90 % — slow more
    return 8;                     // ≥ 90 % — maximum backoff
}

// ─── Duty-cycle visibility getters ────────────────────────────
// v4.8 — token-bucket refill applied before every read so a caller
// polling only this getter (i.e. the HEALTH tick that hasn't seen a
// TX yet in the current window) still sees fresh silence credit.
unsigned long MeshCore::txMsThisHour() {
    refillBucket();
    return txBucketMs;
}

// ─── Token-bucket refill ─────────────────────────────────────
// Called by every gate check and by TX increment. Leaks the bucket by
// 7 % of elapsed real time since the previous refill, so silence banks
// credit at ~4.2 s per real minute. First call after boot initialises
// the timestamp without leaking — bucket already starts at 0 (full
// credit) so there's nothing to leak.
void MeshCore::refillBucket() {
    unsigned long now = millis();
    if (bucketLastLeakMs == 0) {
        // Sentinel bump if millis() happens to be 0 at this instant —
        // the next call will pick up from here without a huge phantom
        // elapsed value.
        bucketLastLeakMs = (now == 0) ? 1UL : now;
        return;
    }
    unsigned long elapsed = now - bucketLastLeakMs;
    if (elapsed == 0) return;
    // Leak = 7 % of elapsed. Integer math is fine at millisecond scale:
    // for elapsed up to 60 * 60 * 1000 ms (1 hour), leak fits easily in
    // an unsigned long. Refill is applied whether or not TX happened.
    unsigned long leak = (elapsed * 7UL) / 100UL;
    if (leak >= txBucketMs) txBucketMs = 0;
    else                    txBucketMs -= leak;
    bucketLastLeakMs = now;
}

uint8_t MeshCore::dcPercent() {
    // Percent of the 7 % LOCAL cap (252 000 ms). We deliberately key
    // off the local cap, not the 10 % legal cap, because canTransmit()
    // starts silently dropping at the local cap — that is the line
    // operators need to see approach. Clamped so a brief overshoot
    // (fwd traffic beyond 252 s while local is unblocked to 360 s)
    // reads as 100, not 142.
    unsigned long t = txMsThisHour();
    unsigned long pct = (t * 100UL) / 252000UL;
    if (pct > 100UL) pct = 100UL;
    return (uint8_t)pct;
}

uint16_t MeshCore::txThrottledLocalCount() { return txThrottledLocal; }
uint16_t MeshCore::txThrottledFwdCount()   { return txThrottledFwd;   }

// Phase H — one-line setter that flips the leaf flag. Called from
// repeater's loadRoleCfg/saveRoleCfg. No mutex needed — single-
// threaded RX/TX and callers only invoke from either setup() or
// a CMD verb inside the same loop() iteration.
void MeshCore::setLeafMode(bool on) { leafMode = on; }

void MeshCore::sendHeartbeat() {
    String hb = "HB," + String(localID);
    if (boardCode[0]) hb += "," + String(boardCode);
    if (statusFlags[0]) hb += "," + String(statusFlags);
    // Battery is tagged ("B<mv>") so its position doesn't break legacy
    // parsers that read boardCode/flags by index.
    if (batteryMv >= 0)  hb += ",B" + String((int)batteryMv);
    // PLC runtime counters, tagged ",P<scans>:<fires>". Omitted when
    // both are zero so the heartbeat from a node with no deployed PLC
    // primitives stays the same length it always was. Lets the gateway
    // drive its WORKING indicator from passively-received heartbeats
    // instead of an active poll loop — see PLC,STATUS auto-poll removal
    // on the gateway side.
    if (plcScans > 0 || plcFires > 0)
        hb += ",P" + String(plcScans) + ":" + String(plcFires);
    // Effective current heartbeat interval in seconds, tagged ",H<sec>".
    // Gateway uses this to size per-node offline detection: without it,
    // a node that bumped its HB to 5 min would be false-flagged offline
    // by the gateway's 120 s default. Reflects the live multiplier so
    // the gateway sees the actual current cadence, not just the
    // configured base. Capped at the same 30 min ceiling the scheduling
    // loop enforces so the value is always meaningful.
    unsigned long effMs = hbIntervalMs * (unsigned long)hbIntervalMultiplier();
    if (effMs > 1800000UL) effMs = 1800000UL;
    uint32_t effSec = (uint32_t)(effMs / 1000UL);
    if (effSec == 0) effSec = 1;   // defensive — never emit ,H0
    hb += ",H" + String(effSec);
    // Reconciliation counter (Option A). 2 hex chars zero-padded. Adds 4
    // chars to the HB — worth ~9 ms extra airtime at SF9/BW125. Gateway
    // treats a change in this value as "state may have changed on this
    // node" and issues a single PLC,STATUS query to fetch the full
    // fingerprint. See the plan in
    // .claude/plans/there-seems-to-be-cryptic-shore.md for the design.
    hb += ",G";
    if (stateGeneration < 0x10) hb += "0";
    hb += String(stateGeneration, HEX);
    // v4.6: remote-node duty-cycle visibility. D<pct> is the current dcPercent()
    // so the gateway can render a DC badge on the discovered-node card and
    // derive the same hbIntervalMultiplier() bucket without a separate tag.
    hb += ",D" + String(dcPercent());
    // v4.6: transient THROTTLED signal for remote nodes. X<delta> is the count
    // of canTransmit() drops since the last successful HB TX. Gateway lights
    // the transient THROTTLED badge whenever delta >= 1; badge auto-clears
    // after 60s of no X. Anchor advance is guarded by canTransmit() below.
    uint16_t txDropNow = txThrottledLocalCount();
    uint16_t txDelta   = (uint16_t)(txDropNow - txThrottledLocalLastHb);
    if (txDelta > 0) hb += ",X" + String(txDelta);
    // Only consume the delta if this HB will actually leave the antenna —
    // otherwise the drops encoded above are lost on a canTransmit() reject.
    if (canTransmit()) txThrottledLocalLastHb = txDropNow;
    transmitPacket(0xFFFF, hb);
}
