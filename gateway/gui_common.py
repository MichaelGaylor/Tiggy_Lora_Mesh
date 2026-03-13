"""
TiggyOpenMesh Gateway — Shared GUI Utilities
═════════════════════════════════════════
Color palette, packet parser, AES decryption, helpers.
Used by both gateway_hub_gui.py and gateway_gui.py.
"""

import struct
import time
from typing import Optional

import serial.tools.list_ports

# ─── Color Palette (matches TFT theme from src/main.cpp) ────

COLORS = {
    "bg":        "#000000",
    "panel":     "#101820",
    "header":    "#0a1a30",
    "accent":    "#00E5FF",   # Cyan
    "text":      "#FFFFFF",
    "dim":       "#808080",
    "faint":     "#444444",
    "good":      "#00FF00",
    "warn":      "#FFA500",
    "bad":       "#FF0000",
    "bubble_in": "#0a3040",
    "bubble_out":"#0a3010",
    "selected":  "#1a2a3a",
    "cursor":    "#FFE000",
}


# ─── RSSI → Color Gradient ──────────────────────────────────

def rssi_to_color(rssi: int) -> str:
    """Map RSSI value to a hex color (green → yellow → red)."""
    # -50 dBm = excellent (green), -100 dBm = terrible (red)
    clamped = max(-100, min(-40, rssi))
    t = (clamped + 100) / 60.0  # 0.0 (bad) to 1.0 (good)
    if t > 0.5:
        # green to yellow
        r = int(255 * (1.0 - (t - 0.5) * 2))
        g = 255
    else:
        # yellow to red
        r = 255
        g = int(255 * t * 2)
    return f"#{r:02X}{g:02X}00"


# ─── Helpers ─────────────────────────────────────────────────

def format_uptime(seconds: float) -> str:
    """Format seconds into human-readable uptime."""
    s = int(seconds)
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m {s % 60}s"
    h = s // 3600
    m = (s % 3600) // 60
    return f"{h}h {m:02d}m"


def detect_serial_ports() -> list[tuple[str, str]]:
    """Return list of (device, description) for available serial ports."""
    return [(p.device, f"{p.device} — {p.description}") for p in serial.tools.list_ports.comports()]


# ─── CRC-16-CCITT (matches MeshCore::crc16) ─────────────────

def crc16_ccitt(data: bytes, seed: int = 0xFFFF) -> int:
    """CRC-16-CCITT matching the C++ firmware implementation."""
    crc = seed & 0xFFFF
    for b in data:
        crc ^= (b << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ─── Packet Parser (Python port of MeshCore) ────────────────

class HexPacketParser:
    """Parse raw hex-encoded mesh packets."""

    @staticmethod
    def parse_raw(hex_data: str) -> Optional[dict]:
        """Parse raw hex into structured packet.

        Packet format:
          [0-1] dest (uint16 big-endian)
          [2-3] src  (uint16 big-endian)
          [4-5] seq  (uint16 big-endian)
          [6]   ttl  (uint8)
          [7..] payload (ASCII)
        """
        try:
            raw = bytes.fromhex(hex_data)
        except ValueError:
            return None
        if len(raw) < 7:
            return None

        dest = (raw[0] << 8) | raw[1]
        src = (raw[2] << 8) | raw[3]
        seq = (raw[4] << 8) | raw[5]
        ttl = raw[6]
        payload = raw[7:].decode("ascii", errors="replace")

        result = {
            "dest": f"{dest:04X}",
            "src": f"{src:04X}",
            "seq": seq,
            "ttl": ttl,
            "payload_raw": payload,
            "size": len(raw),
            "hex": hex_data[:40],
            "type": "DATA",
            "timestamp": time.time(),
        }

        if payload.startswith("ACK,"):
            result["type"] = "ACK"
            parts = payload[4:].split(",", 1)
            if len(parts) == 2:
                result["ack_from"] = parts[0]
                result["ack_mid"] = parts[1]
        elif payload.startswith("HB,"):
            result["type"] = "HB"
            result["hb_from"] = payload[3:].strip()
        else:
            fields = HexPacketParser.parse_payload_fields(payload)
            if fields:
                result["type"] = "MSG"
                result.update(fields)

        return result

    @staticmethod
    def parse_payload_fields(payload: str) -> Optional[dict]:
        """Parse message payload: from,to,mid,ttl,route,encrypted."""
        parts = payload.split(",")
        if len(parts) < 6:
            return None
        # Route can contain commas; encrypted is the last field
        last_comma = payload.rfind(",")
        second_to_last = payload.rfind(",", 0, last_comma)
        if second_to_last < 0:
            return None

        try:
            return {
                "msg_from": parts[0],
                "msg_to": parts[1],
                "msg_id": parts[2],
                "msg_ttl": int(parts[3]),
                "route": ",".join(parts[4:-1]),
                "encrypted": parts[-1],
            }
        except (ValueError, IndexError):
            return None


# ─── AES Decryption (matches MeshCore::decryptMsg) ──────────

def build_iv(seed: str) -> bytes:
    """Build 16-byte IV from seed string (matches MeshCore::buildIV)."""
    iv = bytearray(16)
    seed_bytes = seed.encode("ascii", errors="replace")
    seed_len = len(seed_bytes)
    if seed_len == 0:
        return bytes(iv)
    for i in range(16):
        if i < seed_len:
            iv[i] = seed_bytes[i]
        else:
            iv[i] = seed_bytes[i % seed_len] ^ i
    return bytes(iv)


def decrypt_message(encrypted_hex: str, msg_id: str, aes_key: str) -> Optional[str]:
    """Decrypt an AES-128-CTR encrypted message.

    Returns plaintext string, or None if decryption/CRC fails.
    """
    try:
        from Crypto.Cipher import AES
        from Crypto.Util import Counter
    except ImportError:
        return None

    try:
        enc = bytes.fromhex(encrypted_hex)
    except ValueError:
        return None

    if len(enc) < 3 or len(aes_key) != 16:
        return None

    iv = build_iv(msg_id)
    # AES-128-CTR: initial counter value from IV
    ctr_val = int.from_bytes(iv, "big")
    ctr = Counter.new(128, initial_value=ctr_val)
    cipher = AES.new(aes_key.encode("ascii"), AES.MODE_CTR, counter=ctr)
    dec = cipher.decrypt(enc)

    payload_len = len(dec) - 2
    if payload_len < 1:
        return None

    recv_crc = (dec[payload_len] << 8) | dec[payload_len + 1]
    calc_crc = crc16_ccitt(dec[:payload_len])

    if recv_crc != calc_crc:
        return None

    return dec[:payload_len].decode("ascii", errors="replace")
