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
        if (it == routingTable.end() ||
            newScore < ((it->second.cost * COST_WEIGHT) - it->second.rssi)) {
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

bool MeshCore::canTransmit() {
    unsigned long now = millis();
    // Reset counter every hour
    if (now - hourStart > 3600000UL) {
        hourStart = now;
        txTimeThisHour = 0;
    }
    // Local traffic capped at 7% (252s/hr) — reserves 3% for forwarding
    return txTimeThisHour < 252000UL;
}

bool MeshCore::canForward() {
    unsigned long now = millis();
    if (now - hourStart > 3600000UL) {
        hourStart = now;
        txTimeThisHour = 0;
    }
    // Forwarding allowed up to the full 10% legal limit (360s/hr)
    // Repeating other nodes' packets must NEVER be starved by local traffic
    return txTimeThisHour < 360000UL;
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
    int pktLen = 7 + payload.length();
    uint8_t pkt[256];
    pkt[0] = dest >> 8;    pkt[1] = dest & 0xFF;
    pkt[2] = src >> 8;     pkt[3] = src & 0xFF;
    pkt[4] = seq >> 8;     pkt[5] = seq & 0xFF;
    pkt[6] = TTL_DEFAULT;
    memcpy(pkt + 7, payload.c_str(), payload.length());

    waitForClearChannel(200);

    unsigned long txStart = millis();
    onTransmitRaw(pkt, pktLen);
    txTimeThisHour += (millis() - txStart);
}

// Local-originated traffic (messages, heartbeats, setpoints) — capped at 7%
void MeshCore::transmitPacket(uint16_t dest, const String& payload) {
    if (!canTransmit()) return;
    _doTransmit(dest, payload);
}

// Forwarded traffic (repeating others' packets) — full 10% budget
void MeshCore::forwardPacket(uint16_t dest, const String& payload) {
    if (!canForward()) return;
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
    if (!canForward()) return;  // Forwarding uses full 10% budget — never starved by local TX

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

    // Queue with jitter delay
    unsigned long jitter = random(FWD_JITTER_MIN, FWD_JITTER_MAX);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pendingFwds[i].active) {
            pendingFwds[i].payload = payload;
            pendingFwds[i].dest = fwdDest;
            pendingFwds[i].sendAt = millis() + jitter;
            pendingFwds[i].active = true;
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
        if (pkt.dest == myAddr && onAck) {
            // ACK for us
            int c1 = pkt.payload.indexOf(',', 4);
            if (c1 > 0) {
                String ackFrom = pkt.payload.substring(4, c1);
                String ackMid  = pkt.payload.substring(c1 + 1);
                onAck(ackFrom, ackMid);
            }
        } else if (pkt.dest != myAddr && pkt.dest != 0xFFFF && pkt.ttl > 1) {
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

                // Rebroadcast CFG so other nodes hear it
                if (pkt.ttl > 1) {
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
        } else if (pkt.dest != myAddr && pkt.dest != 0xFFFF && pkt.ttl > 1) {
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

                // Rebroadcast CFGGO so all nodes hear it
                if (pkt.ttl > 1) {
                    forwardPacket(0xFFFF, pkt.payload);
                    packetsForwarded++;
                }
            }
        }
        return;
    }

    // ─── Heartbeat ───────────────────────────────────────────
    if (pkt.payload.startsWith("HB,")) {
        String hbFrom = pkt.payload.substring(3);
        hbFrom.trim();
        if (isValidNodeID(hbFrom)) {
            // Collision: another node is using our ID
            if (hbFrom == String(localID)) {
                idConflictDetected = true;
                if (onIdConflict) onIdConflict(hbFrom, lastRSSI);
                return;  // Don't add ourselves as a peer
            }
            addNode(hbFrom);
            updateRouting(hbFrom, hbFrom, lastRSSI);
            if (onHeartbeat) onHeartbeat(hbFrom, lastRSSI);
        }
        return;
    }

    // ─── Data packets ────────────────────────────────────────
    MeshMessage msg;
    if (!parsePayloadFields(pkt.payload, msg)) return;

    // Dedup
    if (isDuplicate(msg.mid)) return;
    markSeen(msg.mid);

    // Update routing from observed route
    addNode(msg.from);
    updateRouting(msg.from, msg.route, lastRSSI);

    // For us? Decrypt and dispatch
    if (pkt.dest == myAddr || pkt.dest == 0xFFFF) {
        String plain = decryptMsg(msg.encrypted);
        if (plain.length() == 0 && pkt.dest == myAddr) {
            Serial.println("WARN: decrypt failed from " + msg.from + " mid=" + msg.mid +
                " keyLen=" + String(strlen(aes_key_string)) +
                " key[0..3]=" + String(aes_key_string[0]) + String(aes_key_string[1]) + String(aes_key_string[2]) + String(aes_key_string[3]) +
                " encLen=" + String(msg.encrypted.length()));
        }
        if (plain.length() > 0) {
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
    }

    // Forward if not exclusively for us
    if (pkt.dest != myAddr) {
        smartForward(msg.from, msg.to, msg.mid, msg.ttl,
                     msg.route, msg.encrypted, pkt.dest);
    }
}

// ═══════════════════════════════════════════════════════════════
// Heartbeat
// ═══════════════════════════════════════════════════════════════

void MeshCore::sendHeartbeat() {
    transmitPacket(0xFFFF, "HB," + String(localID));
}
