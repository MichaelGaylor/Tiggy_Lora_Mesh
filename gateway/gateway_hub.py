#!/usr/bin/env python3
"""
TiggyOpenMesh Gateway Hub
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
import os
import signal
import sys
import time

import websockets
from websockets.asyncio.server import serve
from aiohttp import web

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
        self.lat = 0.0
        self.lon = 0.0
        self.antenna_type = 0
        self.antenna_height = 2.0


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

        # Gateway registry (persisted to disk so offline gateways appear on map)
        self.registry_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gateways.json")
        self.registry: dict[str, dict] = {}  # id -> {name, lat, lon, ...}
        self._load_registry()

    def _load_registry(self):
        """Load gateway registry from disk."""
        try:
            if os.path.exists(self.registry_path):
                with open(self.registry_path, "r") as f:
                    self.registry = json.load(f)
                log.info(f"Loaded {len(self.registry)} gateways from registry")
        except Exception as e:
            log.warning(f"Failed to load registry: {e}")
            self.registry = {}

    def _save_registry(self):
        """Save gateway registry to disk."""
        try:
            with open(self.registry_path, "w") as f:
                json.dump(self.registry, f, indent=2)
        except Exception as e:
            log.warning(f"Failed to save registry: {e}")

    def _update_registry(self, gw: ConnectedGateway):
        """Update registry entry for a gateway."""
        self.registry[gw.gateway_id] = {
            "name": gw.name,
            "lat": gw.lat,
            "lon": gw.lon,
            "antenna_type": gw.antenna_type,
            "antenna_height": gw.antenna_height,
            "last_seen": time.time(),
        }
        self._save_registry()

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
            gw.gateway_id = msg.get("id", msg.get("gateway_id", "unknown"))
            gw.name = msg.get("name", gw.gateway_id)
            gw.lat = float(msg.get("lat", 0.0))
            gw.lon = float(msg.get("lon", 0.0))
            gw.antenna_type = int(msg.get("antenna", 0))
            gw.antenna_height = float(msg.get("height", 2.0))
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
            self._update_registry(gw)
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

    # ─── HTTP API (serves map + gateway data) ────────────────

    def _get_gateways_json(self):
        """Build JSON array of all gateways (live + registry)."""
        # Start with registry (offline gateways)
        result = {}
        for gw_id, info in self.registry.items():
            # Skip gateways with no location configured
            if info.get("lat", 0) == 0 and info.get("lon", 0) == 0:
                continue
            result[gw_id] = {
                "id": gw_id,
                "name": info.get("name", gw_id),
                "lat": info.get("lat", 0),
                "lon": info.get("lon", 0),
                "antenna_type": info.get("antenna_type", 0),
                "antenna_height": info.get("antenna_height", 2.0),
                "online": False,
                "uptime": 0,
                "packets": 0,
            }

        # Overlay live gateway data
        for gw in self.gateways.values():
            if not gw.authenticated:
                continue
            if gw.lat == 0 and gw.lon == 0:
                continue
            result[gw.gateway_id] = {
                "id": gw.gateway_id,
                "name": gw.name,
                "lat": gw.lat,
                "lon": gw.lon,
                "antenna_type": gw.antenna_type,
                "antenna_height": gw.antenna_height,
                "online": True,
                "uptime": int(time.time() - gw.connected_at),
                "packets": gw.packets_relayed,
            }

        return list(result.values())

    async def handle_api_gateways(self, request):
        """GET /api/gateways — JSON array of gateway info."""
        return web.json_response(self._get_gateways_json())

    async def handle_map_page(self, request):
        """GET / — serve map.html."""
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
        map_file = os.path.join(web_dir, "map.html")
        if os.path.exists(map_file):
            return web.FileResponse(map_file)
        return web.Response(text="map.html not found — place it in gateway/web/", status=404)

    async def start_http_server(self):
        """Start aiohttp server on listen_port + 1."""
        app = web.Application()
        app.router.add_get("/", self.handle_map_page)
        app.router.add_get("/api/gateways", self.handle_api_gateways)
        # Serve static files from gateway/web/
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
        if os.path.isdir(web_dir):
            app.router.add_static("/static/", web_dir)

        http_port = self.listen_port + 1
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "0.0.0.0", http_port)
        await site.start()
        log.info(f"Map server: http://0.0.0.0:{http_port}")
        return runner

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
        log.info(f"TiggyOpenMesh Gateway Hub starting on ws://0.0.0.0:{self.listen_port}")
        if self.auth_key:
            log.info(f"Authentication: ENABLED (key required)")
        else:
            log.info(f"Authentication: DISABLED (open relay)")

        server = await serve(self.ws_handler, "0.0.0.0", self.listen_port)

        # Start HTTP map server
        http_runner = await self.start_http_server()

        status_task = asyncio.create_task(self.status_printer())

        try:
            await server.wait_closed()
        except asyncio.CancelledError:
            pass
        finally:
            await http_runner.cleanup()

    def shutdown(self):
        """Graceful shutdown."""
        log.info("Hub shutting down...")
        self.running = False


# ─── CLI ─────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="TiggyOpenMesh Gateway Hub — central relay for all gateway nodes",
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
