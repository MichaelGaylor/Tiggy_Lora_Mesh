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

void MeshCore::buildIV(const String& seed, byte* iv) {
    for (int i = 0; i < 16; i++)
        iv[i] = (i < (int)seed.length()) ? seed[i] : (seed[i % seed.length()] ^ i);
}

String MeshCore::encryptMsg(const String& msg, const String& iv_seed) {
    int mlen = msg.length();
    if (mlen + 2 > MAX_MSG_LEN) return "";

    uint8_t buffer[MAX_MSG_LEN + 2];
    memcpy(buffer, msg.c_str(), mlen);
    uint16_t crc = crc16(buffer, mlen);
    buffer[mlen]     = (crc >> 8) & 0xFF;
    buffer[mlen + 1] =  crc       & 0xFF;
    int totalLen = mlen + 2;

    byte iv[16];
    buildIV(iv_seed, iv);
    CTR<AES128> ctr;
    ctr.setKey(getAESKey(), AES_KEY_LEN);
    ctr.setIV(iv, sizeof(iv));

    uint8_t outbuf[MAX_MSG_LEN + 2];
    ctr.encrypt(outbuf, buffer, totalLen);
    return toHex(outbuf, totalLen);
}

String MeshCore::decryptMsg(const String& hexstr, const String& iv_seed) {
    uint8_t enc[MAX_MSG_LEN + 2];
    int elen;
    hexToBytes(hexstr, enc, elen);
    if (elen < 3) return "";

    byte iv[16];
    buildIV(iv_seed, iv);
    CTR<AES128> ctr;
    ctr.setKey(getAESKey(), AES_KEY_LEN);
    ctr.setIV(iv, sizeof(iv));

    uint8_t dec[MAX_MSG_LEN + 2];
    ctr.decrypt(dec, enc, elen);

    int payloadLen = elen - 2;
    uint16_t recvCrc = (dec[payloadLen] << 8) | dec[payloadLen + 1];
    uint16_t calcCrc = crc16(dec, payloadLen);
    if (recvCrc != calcCrc) return "";

    return String((char*)dec, payloadLen);
}

String MeshCore::generateMsgID() {
    String s;
    for (int i = 0; i < 4; i++) s += (char)random(65, 91);
    return s;
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
    // 10% duty cycle = 360 seconds per hour
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

void MeshCore::transmitPacket(uint16_t dest, const String& payload) {
    if (!canTransmit() || !onTransmitRaw) return;

    uint16_t src = strtol(localID, nullptr, 16);
    uint16_t seq = seqCounter++;
    int pktLen = 7 + payload.length();
    uint8_t pkt[256];
    pkt[0] = dest >> 8;    pkt[1] = dest & 0xFF;
    pkt[2] = src >> 8;     pkt[3] = src & 0xFF;
    pkt[4] = seq >> 8;     pkt[5] = seq & 0xFF;
    pkt[6] = TTL_DEFAULT;
    memcpy(pkt + 7, payload.c_str(), payload.length());

    // Listen-before-talk
    waitForClearChannel(200);

    unsigned long txStart = millis();
    onTransmitRaw(pkt, pktLen);
    txTimeThisHour += (millis() - txStart);
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
    if (!canTransmit()) return;

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
            transmitPacket(pendingFwds[i].dest, pendingFwds[i].payload);
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
    packetsReceived++;
    uint16_t myAddr = strtol(localID, nullptr, 16);

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
            // Forward ACK
            String payload = pkt.payload;
            transmitPacket(pkt.dest, payload);
            packetsForwarded++;
        }
        return;
    }

    // ─── Heartbeat ───────────────────────────────────────────
    if (pkt.payload.startsWith("HB,")) {
        String hbFrom = pkt.payload.substring(3);
        hbFrom.trim();
        if (isValidNodeID(hbFrom)) {
            addNode(hbFrom);
            updateRouting(hbFrom, hbFrom, lastRSSI);
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
        String plain = decryptMsg(msg.encrypted, msg.mid);
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
