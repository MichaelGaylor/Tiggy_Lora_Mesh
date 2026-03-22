#!/usr/bin/env python3
"""
TiggyOpenMesh Gateway Server
═════════════════════════
Bridges isolated LoRa mesh islands over the internet.

Two connection modes:
  1. Hub mode (recommended):  gateway --port COM3 --hub ws://hub:9000
     All gateways connect to a central hub server (gateway_hub.py).
     Simplest setup — gateways only need the hub address.

  2. Peer mode (advanced):    gateway --port COM3 --peers ws://peer:9000
     Direct peer-to-peer WebSocket connections between gateways.
     No central server needed, but each gateway must know every peer.

Architecture:
  Repeater ←USB Serial→ Gateway ←WebSocket→ Hub/Peers ←WebSocket→ Gateway ←USB Serial→ Repeater

Packets remain AES-128-CTR encrypted end-to-end.
The server never decrypts — it is an opaque relay.

Serial protocol:
  Repeater → Server:  PKT,<hex_encoded_raw_bytes>
  Server → Repeater:  PKT,<hex_encoded_raw_bytes>

WebSocket protocol:
  JSON messages: {"type": "pkt", "data": "<hex>", "origin": "<gateway_id>"}
"""

import argparse
import asyncio
import json
import logging
import secrets
import signal
import sys
import time
from collections import deque

import serial
import serial.tools.list_ports
import websockets
from websockets.asyncio.server import serve

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("gateway")

# ─── Dedup ───────────────────────────────────────────────────
DEDUP_SIZE = 256
DEDUP_TTL = 30  # seconds

class PacketDedup:
    """Prevents packet loops between gateways using a time-expiring hash set."""

    def __init__(self):
        self._seen: dict[str, float] = {}

    def is_duplicate(self, hex_data: str) -> bool:
        now = time.time()
        # Prune expired
        if len(self._seen) > DEDUP_SIZE:
            self._seen = {k: v for k, v in self._seen.items() if now - v < DEDUP_TTL}
        if hex_data in self._seen and now - self._seen[hex_data] < DEDUP_TTL:
            return True
        self._seen[hex_data] = now
        return False


# ─── Gateway Server ──────────────────────────────────────────

class GatewayServer:
    def __init__(self, serial_port: str, baud: int, listen_port: int,
                 peers: list[str], hub_url: str | None = None,
                 hub_key: str | None = None, name: str | None = None):
        self.serial_port = serial_port
        self.baud = baud
        self.listen_port = listen_port
        self.peer_urls = peers
        self.hub_url = hub_url
        self.hub_key = hub_key
        self.gateway_id = secrets.token_hex(4)
        self.name = name or f"gw-{self.gateway_id[:6]}"

        self.dedup = PacketDedup()
        self.peer_connections: set[websockets.WebSocketProtocol] = set()
        self.client_connections: set[websockets.WebSocketProtocol] = set()  # inbound from other gateways
        self.hub_ws = None  # WebSocket connection to hub
        self.serial_conn: serial.Serial | None = None
        self.running = True

        # Stats
        self.packets_from_radio = 0
        self.packets_from_peers = 0
        self.packets_to_radio = 0
        self.packets_to_peers = 0

    # ─── Serial ──────────────────────────────────────────────

    def open_serial(self):
        """Open serial connection to the repeater."""
        try:
            self.serial_conn = serial.Serial(self.serial_port, self.baud, timeout=0.1)
            log.info(f"Serial opened: {self.serial_port} @ {self.baud}")
            # Enable gateway mode on the repeater
            time.sleep(2)  # wait for boot
            self.serial_conn.write(b"GATEWAY ON\n")
            time.sleep(0.5)
            # Drain the response
            while self.serial_conn.in_waiting:
                line = self.serial_conn.readline().decode("ascii", errors="replace").strip()
                if line:
                    log.info(f"Serial: {line}")
        except serial.SerialException as e:
            log.error(f"Cannot open {self.serial_port}: {e}")
            sys.exit(1)

    async def serial_reader(self):
        """Read PKT lines from serial and broadcast to all peers."""
        loop = asyncio.get_event_loop()
        buf = b""
        while self.running:
            try:
                data = await loop.run_in_executor(None, self._serial_read_chunk)
                if not data:
                    continue
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line_str = line.decode("ascii", errors="replace").strip()
                    if line_str.startswith("PKT,"):
                        hex_data = line_str[4:]
                        if self.dedup.is_duplicate(hex_data):
                            continue
                        self.packets_from_radio += 1
                        log.info(f"Radio→Net: {len(hex_data)//2}B [{hex_data[:16]}...]")
                        await self.broadcast_to_peers(hex_data)
                    elif line_str:
                        log.debug(f"Serial: {line_str}")
            except Exception as e:
                log.error(f"Serial read error: {e}")
                await asyncio.sleep(1)

    def _serial_read_chunk(self) -> bytes:
        """Blocking read from serial (runs in executor)."""
        if self.serial_conn and self.serial_conn.in_waiting:
            return self.serial_conn.read(self.serial_conn.in_waiting)
        time.sleep(0.05)
        return b""

    def inject_to_radio(self, hex_data: str):
        """Send a packet to the local mesh via serial."""
        if not self.serial_conn:
            return
        try:
            cmd = f"PKT,{hex_data}\n"
            self.serial_conn.write(cmd.encode("ascii"))
            self.packets_to_radio += 1
            log.info(f"Net→Radio: {len(hex_data)//2}B [{hex_data[:16]}...]")
        except serial.SerialException as e:
            log.error(f"Serial write error: {e}")

    # ─── WebSocket Server (accept inbound peers) ─────────────

    async def ws_handler(self, websocket):
        """Handle an inbound WebSocket connection from a peer gateway."""
        remote = websocket.remote_address
        log.info(f"Peer connected: {remote}")
        self.client_connections.add(websocket)
        try:
            async for message in websocket:
                await self.handle_peer_message(message, websocket)
        except websockets.ConnectionClosed:
            pass
        finally:
            self.client_connections.discard(websocket)
            log.info(f"Peer disconnected: {remote}")

    async def handle_peer_message(self, raw: str, source_ws):
        """Process a message from a peer gateway."""
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return

        if msg.get("type") == "pkt":
            hex_data = msg.get("data", "")
            origin = msg.get("origin", "")

            # Don't echo back our own packets
            if origin == self.gateway_id:
                return

            if self.dedup.is_duplicate(hex_data):
                return

            self.packets_from_peers += 1

            # Inject into local mesh
            self.inject_to_radio(hex_data)

            # Re-broadcast to other peers (not back to source)
            await self.broadcast_to_peers(hex_data, exclude=source_ws)

    async def broadcast_to_peers(self, hex_data: str, exclude=None):
        """Send a packet to all connected peer gateways and/or the hub."""
        msg = json.dumps({
            "type": "pkt",
            "data": hex_data,
            "origin": self.gateway_id,
        })
        self.packets_to_peers += 1

        # Send to hub if connected
        if self.hub_ws and self.hub_ws is not exclude:
            try:
                await self.hub_ws.send(msg)
            except websockets.ConnectionClosed:
                self.hub_ws = None

        # Send to direct peers
        all_peers = self.peer_connections | self.client_connections
        for ws in list(all_peers):
            if ws is exclude:
                continue
            try:
                await ws.send(msg)
            except websockets.ConnectionClosed:
                self.peer_connections.discard(ws)
                self.client_connections.discard(ws)

    # ─── Outbound Peer Connections ───────────────────────────

    async def connect_to_peer(self, url: str):
        """Maintain a persistent connection to a peer gateway."""
        while self.running:
            try:
                log.info(f"Connecting to peer: {url}")
                async with websockets.connect(url) as ws:
                    self.peer_connections.add(ws)
                    log.info(f"Connected to peer: {url}")
                    async for message in ws:
                        await self.handle_peer_message(message, ws)
            except (ConnectionRefusedError, OSError, websockets.ConnectionClosed) as e:
                log.warning(f"Peer {url} unavailable: {e}. Retrying in 10s...")
            finally:
                self.peer_connections.discard(ws)
            await asyncio.sleep(10)

    # ─── Hub Connection ──────────────────────────────────────

    async def connect_to_hub(self):
        """Maintain a persistent connection to the central hub server."""
        while self.running:
            try:
                log.info(f"Connecting to hub: {self.hub_url}")
                async with websockets.connect(self.hub_url) as ws:
                    self.hub_ws = ws

                    # Authenticate with hub
                    auth_data = {
                        "type": "auth",
                        "gateway_id": self.gateway_id,
                        "name": self.name,
                        "key": self.hub_key or "",
                    }
                    # Only send location if actually configured
                    lat = getattr(self, "lat", 0.0)
                    lon = getattr(self, "lon", 0.0)
                    if lat != 0.0 or lon != 0.0:
                        auth_data["lat"] = lat
                        auth_data["lon"] = lon
                        auth_data["height"] = getattr(self, "antenna_height", 2.0)
                        auth_data["antenna"] = getattr(self, "antenna_type", 0)
                    auth_msg = json.dumps(auth_data)
                    await ws.send(auth_msg)

                    # Wait for auth result
                    response = await ws.recv()
                    result = json.loads(response)
                    if result.get("type") == "auth_result":
                        if result.get("success"):
                            peers = result.get("peers", 0)
                            log.info(f"Hub connected! {peers} other gateway(s) online")
                        else:
                            log.error(f"Hub auth failed: {result.get('error', 'unknown')}")
                            await asyncio.sleep(10)
                            continue

                    # Listen for relayed packets
                    async for message in ws:
                        try:
                            msg = json.loads(message)
                        except json.JSONDecodeError:
                            continue

                        if msg.get("type") == "pkt":
                            hex_data = msg.get("data", "")
                            origin = msg.get("origin", "")

                            if origin == self.gateway_id:
                                continue
                            if self.dedup.is_duplicate(hex_data):
                                continue

                            self.packets_from_peers += 1
                            self.inject_to_radio(hex_data)

                        elif msg.get("type") == "peer_joined":
                            log.info(f"Peer joined: {msg.get('name', '?')} ({msg.get('peers', '?')} total)")

                        elif msg.get("type") == "peer_left":
                            log.info(f"Peer left: {msg.get('name', '?')}")

            except (ConnectionRefusedError, OSError, websockets.ConnectionClosed) as e:
                log.warning(f"Hub {self.hub_url} unavailable: {e}. Retrying in 10s...")
            finally:
                self.hub_ws = None
            await asyncio.sleep(10)

    # ─── Status Printer ──────────────────────────────────────

    async def status_printer(self):
        """Print stats every 60 seconds."""
        while self.running:
            await asyncio.sleep(60)
            peers = len(self.peer_connections) + len(self.client_connections)
            log.info(
                f"Stats: radio_rx={self.packets_from_radio} "
                f"peer_rx={self.packets_from_peers} "
                f"radio_tx={self.packets_to_radio} "
                f"peer_tx={self.packets_to_peers} "
                f"peers={peers}"
            )

    # ─── Main ────────────────────────────────────────────────

    async def run(self):
        """Start all gateway tasks."""
        self.open_serial()

        log.info(f"Gateway: {self.name} (ID: {self.gateway_id})")
        if self.hub_url:
            log.info(f"Hub mode: {self.hub_url}")
        if self.peer_urls:
            log.info(f"Direct peers: {', '.join(self.peer_urls)}")
        if not self.hub_url and not self.peer_urls:
            log.info(f"Listening on ws://0.0.0.0:{self.listen_port} (waiting for inbound peers)")

        tasks = []

        # WebSocket server for inbound peers (also useful in hub mode for status queries)
        server = await serve(self.ws_handler, "0.0.0.0", self.listen_port)
        tasks.append(asyncio.create_task(self._serve_forever(server)))

        # Serial reader
        tasks.append(asyncio.create_task(self.serial_reader()))

        # Hub connection (recommended mode)
        if self.hub_url:
            tasks.append(asyncio.create_task(self.connect_to_hub()))

        # Outbound direct peer connections
        for url in self.peer_urls:
            tasks.append(asyncio.create_task(self.connect_to_peer(url)))

        # Status printer
        tasks.append(asyncio.create_task(self.status_printer()))

        # Wait for shutdown
        try:
            await asyncio.gather(*tasks)
        except asyncio.CancelledError:
            pass

    async def _serve_forever(self, server):
        """Keep the WebSocket server running."""
        await server.wait_closed()

    def shutdown(self):
        """Graceful shutdown."""
        log.info("Shutting down...")
        self.running = False
        if self.serial_conn:
            try:
                self.serial_conn.write(b"GATEWAY OFF\n")
                time.sleep(0.2)
                self.serial_conn.close()
            except Exception:
                pass


# ─── CLI ─────────────────────────────────────────────────────

def list_serial_ports():
    """List available serial ports."""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No serial ports found.")
    else:
        print("Available serial ports:")
        for p in ports:
            print(f"  {p.device} — {p.description}")


def main():
    parser = argparse.ArgumentParser(
        description="TiggyOpenMesh Gateway — bridge mesh islands over the internet",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # List available serial ports
  python gateway.py --list-ports

  # Connect to a central hub (recommended)
  python gateway.py --port COM3 --hub ws://myserver.com:9000
  python gateway.py --port COM3 --hub ws://myserver.com:9000 --hub-key MySecret

  # Give this gateway a friendly name
  python gateway.py --port COM3 --hub ws://hub:9000 --name "Base Camp"

  # Direct peer-to-peer mode (no hub needed)
  python gateway.py --port COM3 --listen 9000 --peers ws://192.168.1.50:9000
""",
    )
    parser.add_argument("--port", "-p", help="Serial port (e.g., COM3, /dev/ttyUSB0)")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--listen", "-l", type=int, default=9000, help="WebSocket listen port (default: 9000)")
    parser.add_argument("--hub", type=str, default=None, help="Hub server URL (e.g., ws://myserver.com:9000)")
    parser.add_argument("--hub-key", type=str, default=None, help="Hub authentication key")
    parser.add_argument("--name", "-n", type=str, default=None, help="Friendly name for this gateway")
    parser.add_argument("--lat", type=float, default=0.0, help="Gateway latitude (for hub map)")
    parser.add_argument("--lon", type=float, default=0.0, help="Gateway longitude (for hub map)")
    parser.add_argument("--height", type=float, default=2.0, help="Antenna height in metres (for hub map)")
    parser.add_argument("--antenna", type=int, default=0, help="Antenna type: 0=omni, 1=directional")
    parser.add_argument("--peers", nargs="*", default=[], help="Direct peer gateway URLs (advanced)")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports and exit")
    args = parser.parse_args()

    if args.list_ports:
        list_serial_ports()
        return

    if not args.port:
        print("Error: --port is required. Use --list-ports to see available ports.")
        sys.exit(1)

    server = GatewayServer(
        args.port, args.baud, args.listen, args.peers,
        hub_url=args.hub, hub_key=args.hub_key, name=args.name,
    )
    server.lat = args.lat
    server.lon = args.lon
    server.antenna_type = args.antenna
    server.antenna_height = args.height

    # Handle Ctrl+C gracefully
    def on_signal(*_):
        server.shutdown()

    signal.signal(signal.SIGINT, on_signal)
    if sys.platform != "win32":
        signal.signal(signal.SIGTERM, on_signal)

    try:
        asyncio.run(server.run())
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    main()
