#!/usr/bin/env python3
"""
TiggyOpenMesh ↔ Meshtastic Bridge
==================================
Standalone bridge that connects a TiggyOpenMesh repeater and a Meshtastic
device (both via USB serial) and translates messages between the two networks.

Usage:
    python meshtastic_bridge.py --tom COM18 --mesh COM4 --key DebdaleLodge2401

Requires: pip install meshtastic pyserial pycryptodome
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import threading
import time
from collections import OrderedDict
from typing import Optional

import serial

# ─── Meshtastic imports ──────────────────────────────────────
try:
    import meshtastic
    import meshtastic.serial_interface
    from pubsub import pub
except ImportError:
    print("ERROR: meshtastic not installed. Run: pip install meshtastic")
    sys.exit(1)

# ─── Reuse TOM encryption/parsing from gui_common ───────────
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_common import HexPacketParser, crc16_ccitt, decrypt_message

GCM_NONCE_LEN = 12
GCM_TAG_LEN = 16

# ═════════════════════════════════════════════════════════════
# Logging
# ═════════════════════════════════════════════════════════════

LOG_FILE = None

def log(direction: str, msg: str):
    ts = time.strftime("%H:%M:%S")
    line = f"[{ts}] {direction:10s} {msg}"
    print(line)
    if LOG_FILE:
        try:
            with open(LOG_FILE, "a") as f:
                f.write(line + "\n")
        except OSError:
            pass

# ═════════════════════════════════════════════════════════════
# Dedup Ring Buffer
# ═════════════════════════════════════════════════════════════

class DedupCache:
    """256-entry hash ring with 30-second TTL."""

    def __init__(self, capacity=256, ttl=30.0):
        self.capacity = capacity
        self.ttl = ttl
        self._cache: OrderedDict[str, float] = OrderedDict()

    def _hash(self, data: str) -> str:
        return hashlib.sha256(data.encode("utf-8", errors="replace")).hexdigest()[:16]

    def is_duplicate(self, data: str) -> bool:
        h = self._hash(data)
        now = time.time()
        # Prune expired
        expired = [k for k, t in self._cache.items() if now - t > self.ttl]
        for k in expired:
            del self._cache[k]
        if h in self._cache:
            return True
        self._cache[h] = now
        if len(self._cache) > self.capacity:
            self._cache.popitem(last=False)
        return False

# ═════════════════════════════════════════════════════════════
# AES-GCM Encryption (for Mesh→TOM direction)
# ═════════════════════════════════════════════════════════════

def encrypt_message(plaintext: str, aes_key: str) -> Optional[str]:
    """Encrypt plaintext with AES-128-GCM, return hex blob (nonce+ct+tag)."""
    try:
        from Crypto.Cipher import AES
        from Crypto.Random import get_random_bytes
    except ImportError:
        return None
    if len(aes_key) != 16:
        return None
    nonce = get_random_bytes(GCM_NONCE_LEN)
    cipher = AES.new(aes_key.encode("ascii"), AES.MODE_GCM, nonce=nonce)
    ciphertext, tag = cipher.encrypt_and_digest(plaintext.encode("utf-8"))
    blob = nonce + ciphertext + tag
    return blob.hex()

# ═════════════════════════════════════════════════════════════
# Node ID Mapping
# ═════════════════════════════════════════════════════════════

class NodeMapper:
    """Maps between TOM 16-bit hex IDs and Meshtastic 32-bit int IDs."""

    def __init__(self, persist_path="bridge_nodes.json"):
        self.persist_path = persist_path
        self.tom_to_mesh: dict[str, int] = {}
        self.mesh_to_tom: dict[int, str] = {}
        self.mesh_names: dict[int, str] = {}  # Meshtastic node names
        self._load()

    def _load(self):
        if os.path.exists(self.persist_path):
            try:
                with open(self.persist_path) as f:
                    data = json.load(f)
                self.tom_to_mesh = data.get("tom_to_mesh", {})
                self.mesh_to_tom = {int(k): v for k, v in data.get("mesh_to_tom", {}).items()}
                self.mesh_names = {int(k): v for k, v in data.get("mesh_names", {}).items()}
            except (json.JSONDecodeError, OSError):
                pass

    def save(self):
        try:
            data = {
                "tom_to_mesh": self.tom_to_mesh,
                "mesh_to_tom": {str(k): v for k, v in self.mesh_to_tom.items()},
                "mesh_names": {str(k): v for k, v in self.mesh_names.items()},
            }
            with open(self.persist_path, "w") as f:
                json.dump(data, f, indent=2)
        except OSError:
            pass

    def get_tom_id(self, mesh_id: int) -> str:
        """Get or assign a TOM ID for a Meshtastic node."""
        if mesh_id in self.mesh_to_tom:
            return self.mesh_to_tom[mesh_id]
        tom_id = f"F{mesh_id & 0xFFF:03X}"
        self.mesh_to_tom[mesh_id] = tom_id
        self.tom_to_mesh[tom_id] = mesh_id
        self.save()
        return tom_id

    def get_mesh_id(self, tom_id: str) -> int:
        """Get or assign a Meshtastic ID for a TOM node."""
        tom_id = tom_id.upper()
        if tom_id in self.tom_to_mesh:
            return self.tom_to_mesh[tom_id]
        mesh_id = 0xFF000000 | int(tom_id, 16)
        self.tom_to_mesh[tom_id] = mesh_id
        self.mesh_to_tom[mesh_id] = tom_id
        self.save()
        return mesh_id

    def set_mesh_name(self, mesh_id: int, name: str):
        if name and name != "Unknown":
            self.mesh_names[mesh_id] = name

    def get_mesh_name(self, mesh_id: int) -> str:
        return self.mesh_names.get(mesh_id, f"!{mesh_id:08x}")

# ═════════════════════════════════════════════════════════════
# TiggyOpenMesh Interface
# ═════════════════════════════════════════════════════════════

class TOMInterface:
    """Serial connection to a TiggyOpenMesh repeater in gateway mode."""

    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.conn: Optional[serial.Serial] = None
        self.running = False
        self.on_packet = None  # Callback: fn(parsed_dict)
        self._thread: Optional[threading.Thread] = None

    def connect(self):
        self.conn = serial.Serial(self.port, self.baud, timeout=1)
        time.sleep(1)
        # Enable gateway mode for raw packet forwarding
        self.conn.write(b"GATEWAY ON\n")
        time.sleep(0.3)
        # Drain startup messages
        while self.conn.in_waiting:
            self.conn.readline()
        log("TOM", f"Connected on {self.port}")

    def start(self):
        self.running = True
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def stop(self):
        self.running = False
        if self.conn:
            self.conn.close()

    def _reader(self):
        while self.running:
            try:
                if not self.conn or not self.conn.in_waiting:
                    time.sleep(0.05)
                    continue
                line = self.conn.readline().decode("ascii", errors="replace").strip()
                if not line:
                    continue
                if line.startswith("PKT,"):
                    self._handle_pkt(line)
            except serial.SerialException:
                log("TOM", "Serial disconnected — reconnecting...")
                time.sleep(2)
                try:
                    self.connect()
                except Exception:
                    pass
            except Exception as e:
                log("TOM", f"Reader error: {e}")
                time.sleep(0.5)

    def _handle_pkt(self, line: str):
        # PKT,<hex>,<rssi>  or  PKT,<hex>
        parts = line.split(",")
        if len(parts) < 2:
            return
        hex_data = parts[1]
        rssi = int(parts[2]) if len(parts) > 2 else 0
        parsed = HexPacketParser.parse_raw(hex_data)
        if parsed and self.on_packet:
            parsed["rssi"] = rssi
            self.on_packet(parsed)

    def inject_packet(self, hex_data: str):
        """Inject a raw hex packet into the TOM mesh via serial."""
        if self.conn:
            self.conn.write(f"PKT,{hex_data}\n".encode("ascii"))

    def send_message(self, text: str):
        """Send a plaintext message via the TOM node's broadcast."""
        if self.conn:
            self.conn.write(f"MSG,FFFF,{text}\n".encode("ascii"))

# ═════════════════════════════════════════════════════════════
# Meshtastic Interface
# ═════════════════════════════════════════════════════════════

class MeshInterface:
    """Wraps meshtastic-python SerialInterface."""

    def __init__(self, port: str):
        self.port = port
        self.interface: Optional[meshtastic.serial_interface.SerialInterface] = None
        self.on_text = None      # fn(from_id, text, packet)
        self.on_position = None  # fn(from_id, lat, lon, alt, packet)
        self.on_nodeinfo = None  # fn(from_id, name, packet)

    def connect(self):
        self.interface = meshtastic.serial_interface.SerialInterface(
            devPath=self.port, noProto=False
        )
        # Subscribe to receive events
        pub.subscribe(self._on_receive, "meshtastic.receive")
        pub.subscribe(self._on_connection, "meshtastic.connection.established")
        log("MESH", f"Connected on {self.port}")

    def _on_connection(self, interface, topic=pub.AUTO_TOPIC):
        log("MESH", "Connection established")

    def _on_receive(self, packet, interface):
        try:
            decoded = packet.get("decoded", {})
            portnum = decoded.get("portnum", "")
            from_id = packet.get("fromId", packet.get("from", 0))

            # Normalise from_id to int
            if isinstance(from_id, str) and from_id.startswith("!"):
                from_id = int(from_id[1:], 16)
            elif isinstance(from_id, str):
                from_id = int(from_id)

            if portnum == "TEXT_MESSAGE_APP":
                text = decoded.get("text", "")
                if text and self.on_text:
                    self.on_text(from_id, text, packet)

            elif portnum == "POSITION_APP":
                pos = decoded.get("position", {})
                lat = pos.get("latitude", pos.get("latitudeI", 0))
                lon = pos.get("longitude", pos.get("longitudeI", 0))
                alt = pos.get("altitude", 0)
                # latitudeI/longitudeI are in 1e-7 degrees
                if isinstance(lat, int) and abs(lat) > 1000:
                    lat = lat / 1e7
                if isinstance(lon, int) and abs(lon) > 1000:
                    lon = lon / 1e7
                if lat and lon and self.on_position:
                    self.on_position(from_id, lat, lon, alt, packet)

            elif portnum == "NODEINFO_APP":
                user = decoded.get("user", {})
                name = user.get("longName", user.get("shortName", ""))
                if name and self.on_nodeinfo:
                    self.on_nodeinfo(from_id, name, packet)

        except Exception as e:
            log("MESH", f"Receive error: {e}")

    def stop(self):
        if self.interface:
            self.interface.close()

    def send_text(self, text: str, destination=None):
        if not self.interface:
            return
        if destination:
            self.interface.sendText(text, destinationId=destination)
        else:
            self.interface.sendText(text)

    def send_position(self, lat: float, lon: float, alt: int = 0):
        if not self.interface:
            return
        self.interface.sendPosition(
            latitude=lat, longitude=lon, altitude=alt
        )

# ═════════════════════════════════════════════════════════════
# Bridge
# ═════════════════════════════════════════════════════════════

class Bridge:
    """Bidirectional TiggyOpenMesh ↔ Meshtastic bridge."""

    def __init__(self, tom_port: str, mesh_port: str,
                 aes_key: str = "", bridge_id: str = "F000",
                 bridge_pos: bool = True, bridge_sos: bool = True):
        self.aes_key = aes_key
        self.bridge_id = bridge_id.upper()
        self.bridge_pos = bridge_pos
        self.bridge_sos = bridge_sos

        self.tom = TOMInterface(tom_port)
        self.mesh = MeshInterface(mesh_port)
        self.mapper = NodeMapper(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "bridge_nodes.json")
        )
        self.dedup = DedupCache()

        # Stats
        self.tom_to_mesh_count = 0
        self.mesh_to_tom_count = 0

    def start(self):
        log("BRIDGE", "Starting TiggyOpenMesh <-> Meshtastic bridge...")
        log("BRIDGE", f"  TOM port:   {self.tom.port}")
        log("BRIDGE", f"  Mesh port:  {self.mesh.port}")
        log("BRIDGE", f"  AES key:    {'set' if self.aes_key else 'none'}")
        log("BRIDGE", f"  Bridge ID:  {self.bridge_id}")
        log("BRIDGE", f"  Bridge POS: {self.bridge_pos}")
        log("BRIDGE", f"  Bridge SOS: {self.bridge_sos}")

        # Connect TOM
        self.tom.connect()
        self.tom.on_packet = self._on_tom_packet
        self.tom.start()

        # Connect Meshtastic
        self.mesh.connect()
        self.mesh.on_text = self._on_mesh_text
        self.mesh.on_position = self._on_mesh_position
        self.mesh.on_nodeinfo = self._on_mesh_nodeinfo

        log("BRIDGE", "Bridge running. Press Ctrl+C to stop.")

        # Status report every 5 minutes
        try:
            while True:
                time.sleep(300)
                log("STATS", f"TOM→Mesh: {self.tom_to_mesh_count}  "
                             f"Mesh→TOM: {self.mesh_to_tom_count}  "
                             f"Nodes mapped: {len(self.mapper.mesh_to_tom)}")
        except KeyboardInterrupt:
            log("BRIDGE", "Shutting down...")
            self.tom.stop()
            self.mesh.stop()

    # ─── TOM → Meshtastic ──────────────────────────────────

    def _on_tom_packet(self, parsed: dict):
        ptype = parsed.get("type", "")
        src = parsed.get("src", "")
        payload = parsed.get("payload_raw", "")

        # Don't re-bridge our own injected packets
        if src == self.bridge_id:
            return

        if ptype == "MSG":
            self._bridge_tom_msg(parsed)
        elif ptype == "HB":
            hb_from = parsed.get("hb_from", "")
            if hb_from:
                log("TOM→MESH", f"HB from {hb_from} (tracked)")
        elif payload.startswith("POS,") and self.bridge_pos:
            self._bridge_tom_pos(parsed, payload)
        elif payload.startswith("SOS,") and self.bridge_sos:
            self._bridge_tom_sos(parsed, payload)

    def _bridge_tom_msg(self, parsed: dict):
        msg_from = parsed.get("msg_from", parsed.get("src", "????"))
        encrypted_hex = parsed.get("encrypted", "")
        msg_id = parsed.get("msg_id", "0")

        # Try to decrypt
        plaintext = None
        if self.aes_key and encrypted_hex:
            plaintext = decrypt_message(encrypted_hex, msg_id, self.aes_key)

        if plaintext:
            # Check for CMD/ internal messages — don't bridge those
            if plaintext.startswith("CMD,") or plaintext.startswith("SDATA,"):
                return
            # Strip MSG, prefix if present
            if plaintext.startswith("MSG,"):
                plaintext = plaintext[4:]
            text = f"[TOM {msg_from}] {plaintext}"
        else:
            text = f"[TOM {msg_from}] (encrypted)"

        if self.dedup.is_duplicate(text):
            log("DEDUP", f"Skipped TOM MSG (already bridged)")
            return

        log("TOM→MESH", f"MSG from {msg_from}: {text[:60]}")
        self.mesh.send_text(text)
        self.tom_to_mesh_count += 1

    def _bridge_tom_pos(self, parsed: dict, payload: str):
        src = parsed.get("src", "????")
        parts = payload.split(",")
        if len(parts) >= 3:
            try:
                lat = float(parts[1])
                lon = float(parts[2])
            except ValueError:
                return
            dedup_key = f"POS:{src}:{lat:.4f},{lon:.4f}"
            if self.dedup.is_duplicate(dedup_key):
                return
            log("TOM→MESH", f"POS from {src}: {lat:.6f}, {lon:.6f}")
            self.mesh.send_position(lat, lon)
            self.tom_to_mesh_count += 1

    def _bridge_tom_sos(self, parsed: dict, payload: str):
        src = parsed.get("src", "????")
        parts = payload.split(",")
        if len(parts) >= 3:
            text = f"SOS ALERT from TOM node {src}: {parts[1]},{parts[2]}"
        else:
            text = f"SOS ALERT from TOM node {src}: NO FIX"

        if self.dedup.is_duplicate(text):
            return
        log("TOM→MESH", f"SOS from {src}")
        self.mesh.send_text(text)
        self.tom_to_mesh_count += 1

    # ─── Meshtastic → TOM ──────────────────────────────────

    def _on_mesh_text(self, from_id: int, text: str, packet: dict):
        # Don't re-bridge messages that came from us
        if text.startswith("[TOM "):
            return

        tom_id = self.mapper.get_tom_id(from_id)
        name = self.mapper.get_mesh_name(from_id)

        dedup_key = f"MESH:{from_id}:{text}"
        if self.dedup.is_duplicate(dedup_key):
            log("DEDUP", "Skipped Mesh TEXT (already bridged)")
            return

        display_text = f"[Mesh {name}] {text}"
        log("MESH→TOM", f"TEXT from {name} ({tom_id}): {text[:60]}")

        # Send as broadcast message via the TOM node
        self.tom.send_message(display_text)
        self.mesh_to_tom_count += 1

    def _on_mesh_position(self, from_id: int, lat: float, lon: float,
                          alt: int, packet: dict):
        if not self.bridge_pos:
            return

        tom_id = self.mapper.get_tom_id(from_id)
        name = self.mapper.get_mesh_name(from_id)

        dedup_key = f"MPOS:{from_id}:{lat:.4f},{lon:.4f}"
        if self.dedup.is_duplicate(dedup_key):
            return

        log("MESH→TOM", f"POS from {name} ({tom_id}): {lat:.6f}, {lon:.6f}")
        # Inject POS into TOM mesh
        if self.tom.conn:
            self.tom.conn.write(f"POS,{lat:.6f},{lon:.6f}\n".encode("ascii"))
        self.mesh_to_tom_count += 1

    def _on_mesh_nodeinfo(self, from_id: int, name: str, packet: dict):
        self.mapper.set_mesh_name(from_id, name)
        tom_id = self.mapper.get_tom_id(from_id)
        log("MESH→TOM", f"Node discovered: {name} → TOM ID {tom_id}")
        self.mapper.save()

# ═════════════════════════════════════════════════════════════
# Main
# ═════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="TiggyOpenMesh <-> Meshtastic Bridge",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python meshtastic_bridge.py --tom COM18 --mesh COM4
  python meshtastic_bridge.py --tom COM18 --mesh COM4 --key DebdaleLodge2401
  python meshtastic_bridge.py --tom /dev/ttyUSB0 --mesh /dev/ttyACM0 --no-pos
        """
    )
    parser.add_argument("--tom", required=True, help="TiggyOpenMesh serial port (e.g., COM18)")
    parser.add_argument("--mesh", required=True, help="Meshtastic serial port (e.g., COM4)")
    parser.add_argument("--key", default="", help="TOM AES-128 key (16 chars) for decrypt/encrypt")
    parser.add_argument("--bridge-id", default="F000", help="TOM node ID the bridge appears as")
    parser.add_argument("--log", default=None, help="Log file path")
    parser.add_argument("--no-pos", action="store_true", help="Don't bridge position messages")
    parser.add_argument("--no-sos", action="store_true", help="Don't bridge SOS messages")

    args = parser.parse_args()

    global LOG_FILE
    LOG_FILE = args.log

    bridge = Bridge(
        tom_port=args.tom,
        mesh_port=args.mesh,
        aes_key=args.key,
        bridge_id=args.bridge_id,
        bridge_pos=not args.no_pos,
        bridge_sos=not args.no_sos,
    )
    bridge.start()


if __name__ == "__main__":
    main()
