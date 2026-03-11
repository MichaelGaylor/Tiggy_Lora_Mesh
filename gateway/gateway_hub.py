#!/usr/bin/env python3
"""
LoRa Mesh Gateway Hub
═════════════════════
Central relay server that connects all gateway nodes together.

Each gateway connects to this hub via WebSocket. The hub relays
packets between all connected gateways. No serial port needed —
the hub is a pure network relay.

Packets remain AES-128-CTR encrypted end-to-end.
The hub never decrypts — it is an opaque relay.

Architecture:
  [Gateway A] ──┐
  [Gateway B] ──┼── ws://hub:9000 ──► Hub relays to all others
  [Gateway C] ──┘

Usage:
  python gateway_hub.py --listen 9000
  python gateway_hub.py --listen 9000 --key MySecretKey123

The optional --key flag requires gateways to authenticate with a
shared key before packets are relayed. This prevents unauthorized
gateways from joining your mesh network.
"""

import argparse
import asyncio
import json
import logging
import signal
import sys
import time

import websockets
from websockets.asyncio.server import serve

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("hub")

# ─── Dedup ───────────────────────────────────────────────────
DEDUP_SIZE = 512
DEDUP_TTL = 30  # seconds


class PacketDedup:
    """Prevents packet loops using a time-expiring hash set."""

    def __init__(self):
        self._seen: dict[str, float] = {}

    def is_duplicate(self, hex_data: str) -> bool:
        now = time.time()
        if len(self._seen) > DEDUP_SIZE:
            self._seen = {k: v for k, v in self._seen.items() if now - v < DEDUP_TTL}
        if hex_data in self._seen and now - self._seen[hex_data] < DEDUP_TTL:
            return True
        self._seen[hex_data] = now
        return False


# ─── Connected Gateway ───────────────────────────────────────

class ConnectedGateway:
    """Tracks a connected gateway client."""

    def __init__(self, ws, gateway_id: str, name: str = ""):
        self.ws = ws
        self.gateway_id = gateway_id
        self.name = name or gateway_id
        self.connected_at = time.time()
        self.packets_relayed = 0
        self.authenticated = False


# ─── Hub Server ──────────────────────────────────────────────

class GatewayHub:
    def __init__(self, listen_port: int, auth_key: str | None = None):
        self.listen_port = listen_port
        self.auth_key = auth_key
        self.dedup = PacketDedup()
        self.gateways: dict[websockets.WebSocketProtocol, ConnectedGateway] = {}
        self.running = True

        # Stats
        self.total_packets_relayed = 0
        self.total_connections = 0

    async def ws_handler(self, websocket):
        """Handle an inbound WebSocket connection from a gateway."""
        remote = websocket.remote_address
        log.info(f"Connection from {remote}")
        self.total_connections += 1

        gw = ConnectedGateway(websocket, gateway_id="pending")
        self.gateways[websocket] = gw

        try:
            async for message in websocket:
                await self.handle_message(message, websocket)
        except websockets.ConnectionClosed:
            pass
        finally:
            if websocket in self.gateways:
                gw = self.gateways.pop(websocket)
                log.info(f"Gateway disconnected: {gw.name} ({remote}) — relayed {gw.packets_relayed} packets")

    async def handle_message(self, raw: str, source_ws):
        """Process a message from a gateway."""
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return

        gw = self.gateways.get(source_ws)
        if not gw:
            return

        msg_type = msg.get("type", "")

        # ─── Authentication ──────────────────────────────────
        if msg_type == "auth":
            gw.gateway_id = msg.get("gateway_id", "unknown")
            gw.name = msg.get("name", gw.gateway_id)
            key = msg.get("key", "")

            if self.auth_key and key != self.auth_key:
                log.warning(f"Auth FAILED from {gw.name} ({source_ws.remote_address})")
                await source_ws.send(json.dumps({
                    "type": "auth_result",
                    "success": False,
                    "error": "Invalid key",
                }))
                await source_ws.close()
                return

            gw.authenticated = True
            peer_count = sum(1 for g in self.gateways.values() if g.authenticated and g.ws is not source_ws)
            log.info(f"Gateway authenticated: {gw.name} (peers online: {peer_count})")
            await source_ws.send(json.dumps({
                "type": "auth_result",
                "success": True,
                "peers": peer_count,
            }))

            # Notify other gateways
            await self.broadcast_event({
                "type": "peer_joined",
                "gateway_id": gw.gateway_id,
                "name": gw.name,
                "peers": peer_count + 1,
            }, exclude=source_ws)
            return

        # ─── Packet Relay ────────────────────────────────────
        if msg_type == "pkt":
            # Must be authenticated (or no auth required)
            if self.auth_key and not gw.authenticated:
                await source_ws.send(json.dumps({
                    "type": "error",
                    "error": "Not authenticated. Send auth message first.",
                }))
                return

            hex_data = msg.get("data", "")
            origin = msg.get("origin", "")

            if not hex_data:
                return

            if self.dedup.is_duplicate(hex_data):
                return

            gw.packets_relayed += 1
            self.total_packets_relayed += 1

            # Relay to all other authenticated gateways
            relay_msg = json.dumps({
                "type": "pkt",
                "data": hex_data,
                "origin": origin,
            })

            relay_count = 0
            for ws, other_gw in list(self.gateways.items()):
                if ws is source_ws:
                    continue
                if self.auth_key and not other_gw.authenticated:
                    continue
                try:
                    await ws.send(relay_msg)
                    relay_count += 1
                except websockets.ConnectionClosed:
                    pass

            log.info(f"Relay: {gw.name} → {relay_count} peers ({len(hex_data)//2}B)")

        # ─── Status Request ──────────────────────────────────
        elif msg_type == "status":
            authenticated = [g for g in self.gateways.values() if g.authenticated]
            await source_ws.send(json.dumps({
                "type": "hub_status",
                "gateways": [
                    {
                        "id": g.gateway_id,
                        "name": g.name,
                        "uptime": int(time.time() - g.connected_at),
                        "packets": g.packets_relayed,
                    }
                    for g in authenticated
                ],
                "total_relayed": self.total_packets_relayed,
                "total_connections": self.total_connections,
            }))

    async def broadcast_event(self, msg: dict, exclude=None):
        """Send an event to all authenticated gateways."""
        raw = json.dumps(msg)
        for ws, gw in list(self.gateways.items()):
            if ws is exclude:
                continue
            if not gw.authenticated:
                continue
            try:
                await ws.send(raw)
            except websockets.ConnectionClosed:
                pass

    # ─── Status Printer ──────────────────────────────────────

    async def status_printer(self):
        """Print stats every 60 seconds."""
        while self.running:
            await asyncio.sleep(60)
            authenticated = sum(1 for g in self.gateways.values() if g.authenticated)
            log.info(
                f"Hub stats: gateways={authenticated} "
                f"total_relayed={self.total_packets_relayed} "
                f"total_connections={self.total_connections}"
            )

    # ─── Main ────────────────────────────────────────────────

    async def run(self):
        """Start the hub server."""
        log.info(f"LoRa Mesh Gateway Hub starting on ws://0.0.0.0:{self.listen_port}")
        if self.auth_key:
            log.info(f"Authentication: ENABLED (key required)")
        else:
            log.info(f"Authentication: DISABLED (open relay)")

        server = await serve(self.ws_handler, "0.0.0.0", self.listen_port)

        status_task = asyncio.create_task(self.status_printer())

        try:
            await server.wait_closed()
        except asyncio.CancelledError:
            pass

    def shutdown(self):
        """Graceful shutdown."""
        log.info("Hub shutting down...")
        self.running = False


# ─── CLI ─────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="LoRa Mesh Gateway Hub — central relay for all gateway nodes",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Start hub on port 9000 (open, no authentication)
  python gateway_hub.py --listen 9000

  # Start hub with authentication key
  python gateway_hub.py --listen 9000 --key MySecretMeshKey

  # Use a custom port
  python gateway_hub.py --listen 8765 --key TopSecret!
""",
    )
    parser.add_argument("--listen", "-l", type=int, default=9000,
                        help="WebSocket listen port (default: 9000)")
    parser.add_argument("--key", "-k", type=str, default=None,
                        help="Authentication key (gateways must provide this to connect)")
    args = parser.parse_args()

    hub = GatewayHub(args.listen, args.key)

    def on_signal(*_):
        hub.shutdown()

    signal.signal(signal.SIGINT, on_signal)
    if sys.platform != "win32":
        signal.signal(signal.SIGTERM, on_signal)

    try:
        asyncio.run(hub.run())
    except KeyboardInterrupt:
        hub.shutdown()


if __name__ == "__main__":
    main()
