#!/usr/bin/env python3
"""
TiggyOpenMesh Gateway Hub
═════════════════════
Central relay server that connects all gateway nodes together.

Each gateway connects to this hub via WebSocket. The hub relays
packets between all connected gateways. No serial port needed —
the hub is a pure network relay.

Packets remain AES-128-GCM encrypted end-to-end.
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

import aiohttp
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
        self.gateways: dict[web.WebSocketResponse, ConnectedGateway] = {}
        self.running = True

        # Stats
        self.total_packets_relayed = 0
        self.total_connections = 0

        # Encrypted sensor packet store — ring buffer of raw MSG payloads
        # Each entry: {from, to, mid, encrypted, timestamp}
        # Hub never decrypts — web clients decrypt in-browser with user's AES key
        self.encrypted_packets: list[dict] = []
        self.MAX_ENCRYPTED_PACKETS = 500

        # Latest decoded sensor values for web gauge feature
        # Key: "nodeId:pin" e.g. "5041:15"
        self.sensor_latest: dict[str, dict] = {}

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

    async def ws_handler(self, request):
        """Handle an inbound WebSocket connection from a gateway (aiohttp)."""
        ws = web.WebSocketResponse()
        await ws.prepare(request)

        remote = request.remote
        log.info(f"Connection from {remote}")
        self.total_connections += 1

        gw = ConnectedGateway(ws, gateway_id="pending")
        self.gateways[ws] = gw

        try:
            async for msg in ws:
                if msg.type == aiohttp.WSMsgType.TEXT:
                    await self.handle_message(msg.data, ws)
                elif msg.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSE):
                    break
        finally:
            if ws in self.gateways:
                gw = self.gateways.pop(ws)
                log.info(f"Gateway disconnected: {gw.name} ({remote}) — relayed {gw.packets_relayed} packets")

        return ws

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
            # Only update location if non-zero values sent (preserve saved registry)
            new_lat = float(msg.get("lat", 0.0))
            new_lon = float(msg.get("lon", 0.0))
            if new_lat != 0.0 or new_lon != 0.0:
                gw.lat = new_lat
                gw.lon = new_lon
            elif gw.gateway_id in self.registry:
                # Restore from saved registry
                gw.lat = self.registry[gw.gateway_id].get("lat", 0.0)
                gw.lon = self.registry[gw.gateway_id].get("lon", 0.0)
            new_height = float(msg.get("height", 0))
            if new_height > 0:
                gw.antenna_height = new_height
            gw.antenna_type = int(msg.get("antenna", gw.antenna_type))
            key = msg.get("key", "")

            if self.auth_key and key != self.auth_key:
                log.warning(f"Auth FAILED from {gw.name}")
                await source_ws.send_str(json.dumps({
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
            await source_ws.send_str(json.dumps({
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
                await source_ws.send_str(json.dumps({
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

            # Extract MSG fields for encrypted sensor API (never decrypts)
            self._extract_msg_from_packet(hex_data)

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
                    await ws.send_str(relay_msg)
                    relay_count += 1
                except (ConnectionError, ConnectionResetError):
                    pass

            log.info(f"Relay: {gw.name} → {relay_count} peers ({len(hex_data)//2}B)")

        # ─── Status Request ──────────────────────────────────
        elif msg_type == "status":
            authenticated = [g for g in self.gateways.values() if g.authenticated]
            await source_ws.send_str(json.dumps({
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

        # ─── Sensor Value (for web gauge) ────────────────────
        elif msg_type == "sensor":
            key = f"{msg.get('node', '')}:{msg.get('pin', '')}"
            self.sensor_latest[key] = {
                "value": msg.get("value", 0),
                "ts": time.time(),
                "node": msg.get("node", ""),
                "pin": msg.get("pin", ""),
                "label": msg.get("label", ""),
                "unit": msg.get("unit", ""),
            }

    async def broadcast_event(self, msg: dict, exclude=None):
        """Send an event to all authenticated gateways."""
        raw = json.dumps(msg)
        for ws, gw in list(self.gateways.items()):
            if ws is exclude:
                continue
            if not gw.authenticated:
                continue
            try:
                await ws.send_str(raw)
            except (ConnectionError, ConnectionResetError):
                pass

    # ─── Encrypted Packet Extraction ─────────────────────────

    def _extract_msg_from_packet(self, hex_data: str):
        """Extract MSG fields from a raw hex packet (never decrypts).
        Packet binary format: dest(2) + src(2) + seq(2) + ttl(1) + payload(N) + crc(2)
        Payload text: MSG,<from>,<to>,<mid>,<ttl>,<route>,<encrypted_hex>
        """
        try:
            raw = bytes.fromhex(hex_data)
            if len(raw) < 9:
                return
            payload_bytes = raw[7:-2]  # strip header(7) and CRC(2)
            payload = payload_bytes.decode("ascii", errors="replace")
            if not payload.startswith("MSG,"):
                return
            parts = payload.split(",", 6)  # MSG,from,to,mid,ttl,route,encrypted
            if len(parts) < 7:
                return
            self.encrypted_packets.append({
                "from": parts[1],
                "to": parts[2],
                "mid": parts[3],
                "encrypted": parts[6],
                "ts": time.time(),
            })
            # Trim ring buffer
            if len(self.encrypted_packets) > self.MAX_ENCRYPTED_PACKETS:
                self.encrypted_packets = self.encrypted_packets[-self.MAX_ENCRYPTED_PACKETS:]
        except Exception:
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

    async def handle_api_sensors(self, request):
        """GET /api/sensors — encrypted MSG packets for client-side decryption.
        Query params: since=<timestamp> to get only recent packets.
        The hub never decrypts — the web client does it in-browser with the user's AES key.
        """
        since = float(request.query.get("since", 0))
        packets = [p for p in self.encrypted_packets if p["ts"] > since]
        return web.json_response({"packets": packets})

    async def handle_api_sensor(self, request):
        """GET /api/sensor/{node}/{pin} — latest decoded sensor value as JSON."""
        node = request.match_info["node"]
        pin = request.match_info["pin"]
        key = f"{node}:{pin}"
        data = self.sensor_latest.get(key, {})
        if not data:
            return web.json_response({"error": "No data"}, status=404)
        return web.json_response(data)

    async def handle_gauge(self, request):
        """GET /gauge/{node}/{pin} — self-contained HTML gauge page for any sensor."""
        node = request.match_info["node"]
        pin = request.match_info["pin"]
        label = request.query.get("label", f"Node {node} Pin {pin}")
        unit = request.query.get("unit", "")
        min_val = request.query.get("min", "0")
        max_val = request.query.get("max", "1024")
        html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>{label} — TiggyOpenMesh Gauge</title>
<style>
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  body {{
    background: #0a0a0a;
    color: #e0e0e0;
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    min-height: 100vh;
    overflow: hidden;
  }}
  .label {{
    font-size: 1.4rem;
    font-weight: 600;
    color: #b0b0b0;
    margin-bottom: 0.5rem;
    text-transform: uppercase;
    letter-spacing: 0.1em;
  }}
  .gauge-wrap {{
    position: relative;
    width: 300px;
    height: 300px;
  }}
  @media (min-width: 500px) {{
    .gauge-wrap {{ width: 380px; height: 380px; }}
    .label {{ font-size: 1.6rem; }}
  }}
  .gauge-wrap svg {{
    width: 100%;
    height: 100%;
  }}
  .arc-bg {{
    fill: none;
    stroke: #1a1a1a;
    stroke-width: 18;
    stroke-linecap: round;
  }}
  .arc-fg {{
    fill: none;
    stroke: url(#gaugeGrad);
    stroke-width: 18;
    stroke-linecap: round;
    transition: stroke-dashoffset 0.8s cubic-bezier(0.4, 0, 0.2, 1);
  }}
  .tick-marks {{
    fill: none;
    stroke: #333;
    stroke-width: 1.5;
  }}
  .value-text {{
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    text-align: center;
  }}
  .value-num {{
    font-size: 4rem;
    font-weight: 700;
    color: #00E676;
    line-height: 1;
    transition: color 0.3s;
  }}
  @media (min-width: 500px) {{
    .value-num {{ font-size: 5rem; }}
  }}
  .value-unit {{
    font-size: 1.2rem;
    color: #888;
    margin-top: 0.2rem;
  }}
  .range-labels {{
    display: flex;
    justify-content: space-between;
    width: 260px;
    margin-top: -1.5rem;
    font-size: 0.85rem;
    color: #555;
  }}
  @media (min-width: 500px) {{
    .range-labels {{ width: 330px; }}
  }}
  .status {{
    margin-top: 1.5rem;
    font-size: 0.9rem;
    color: #555;
    text-align: center;
  }}
  .status.stale {{ color: #FF5252; }}
  .brand {{
    position: fixed;
    bottom: 1rem;
    font-size: 0.75rem;
    color: #333;
    letter-spacing: 0.05em;
  }}
  .waiting {{
    color: #555;
    font-size: 1.5rem;
    animation: pulse 2s ease-in-out infinite;
  }}
  @keyframes pulse {{
    0%, 100% {{ opacity: 0.4; }}
    50% {{ opacity: 1; }}
  }}
</style>
</head>
<body>
<div class="label" id="label">{label}</div>
<div class="gauge-wrap">
  <svg viewBox="0 0 200 200">
    <defs>
      <linearGradient id="gaugeGrad" x1="0%" y1="0%" x2="100%" y2="0%">
        <stop offset="0%" stop-color="#00E676"/>
        <stop offset="70%" stop-color="#00E676"/>
        <stop offset="100%" stop-color="#FFEA00"/>
      </linearGradient>
    </defs>
    <!-- tick marks -->
    <g class="tick-marks" id="ticks"></g>
    <!-- background arc -->
    <path class="arc-bg" id="arcBg"/>
    <!-- foreground arc (value) -->
    <path class="arc-fg" id="arcFg"/>
  </svg>
  <div class="value-text">
    <div class="value-num" id="valNum">--</div>
    <div class="value-unit" id="valUnit">{unit}</div>
  </div>
</div>
<div class="range-labels">
  <span id="minLabel">{min_val}</span>
  <span id="maxLabel">{max_val}</span>
</div>
<div class="status" id="status">Connecting...</div>
<div class="brand">TiggyOpenMesh</div>

<script>
(function() {{
  const MIN = {min_val};
  const MAX = {max_val};
  const API = "/api/sensor/{node}/{pin}";

  // Arc geometry — 240-degree sweep, centred at bottom
  const CX = 100, CY = 100, R = 80;
  const START_ANGLE = 150;   // degrees (from 12 o'clock CW)
  const SWEEP = 240;

  function polarToCart(cx, cy, r, deg) {{
    const rad = (deg - 90) * Math.PI / 180;
    return {{ x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) }};
  }}

  function describeArc(cx, cy, r, startDeg, endDeg) {{
    const s = polarToCart(cx, cy, r, startDeg);
    const e = polarToCart(cx, cy, r, endDeg);
    const large = (endDeg - startDeg) > 180 ? 1 : 0;
    return `M ${{s.x}} ${{s.y}} A ${{r}} ${{r}} 0 ${{large}} 1 ${{e.x}} ${{e.y}}`;
  }}

  // Draw background arc
  const bgPath = describeArc(CX, CY, R, START_ANGLE, START_ANGLE + SWEEP);
  document.getElementById('arcBg').setAttribute('d', bgPath);

  // Draw tick marks
  const tickG = document.getElementById('ticks');
  for (let i = 0; i <= 10; i++) {{
    const deg = START_ANGLE + (SWEEP * i / 10);
    const inner = polarToCart(CX, CY, R - 12, deg);
    const outer = polarToCart(CX, CY, R - 8, deg);
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', inner.x);
    line.setAttribute('y1', inner.y);
    line.setAttribute('x2', outer.x);
    line.setAttribute('y2', outer.y);
    if (i % 5 === 0) {{ line.style.strokeWidth = '2.5'; line.style.stroke = '#555'; }}
    tickG.appendChild(line);
  }}

  // Compute total arc length for dash-offset animation
  const arcFg = document.getElementById('arcFg');
  arcFg.setAttribute('d', bgPath);
  const totalLen = arcFg.getTotalLength();
  arcFg.style.strokeDasharray = totalLen;
  arcFg.style.strokeDashoffset = totalLen;  // fully hidden

  const valNum = document.getElementById('valNum');
  const statusEl = document.getElementById('status');
  let lastTs = 0;

  function setGauge(value) {{
    const clamped = Math.max(MIN, Math.min(MAX, value));
    const frac = (clamped - MIN) / (MAX - MIN || 1);
    arcFg.style.strokeDashoffset = totalLen * (1 - frac);
    valNum.textContent = Math.round(value);
    // Colour shift: green → yellow → red
    if (frac > 0.85) {{
      valNum.style.color = '#FF5252';
    }} else if (frac > 0.65) {{
      valNum.style.color = '#FFEA00';
    }} else {{
      valNum.style.color = '#00E676';
    }}
  }}

  async function poll() {{
    try {{
      const resp = await fetch(API);
      if (!resp.ok) {{
        statusEl.textContent = 'Waiting for data\u2026';
        statusEl.className = 'status';
        return;
      }}
      const d = await resp.json();
      lastTs = d.ts || 0;
      setGauge(d.value);
      updateAge();
    }} catch(e) {{
      statusEl.textContent = 'Connection error';
      statusEl.className = 'status stale';
    }}
  }}

  function updateAge() {{
    if (!lastTs) return;
    const ago = Math.round(Date.now() / 1000 - lastTs);
    if (ago < 0 || ago > 86400) {{
      statusEl.textContent = 'Stale data';
      statusEl.className = 'status stale';
    }} else if (ago > 120) {{
      statusEl.textContent = `Last updated: ${{Math.round(ago/60)}}m ago`;
      statusEl.className = 'status stale';
    }} else {{
      statusEl.textContent = `Last updated: ${{ago}}s ago`;
      statusEl.className = 'status';
    }}
  }}

  poll();
  setInterval(poll, 5000);
  setInterval(updateAge, 1000);
}})();
</script>
</body>
</html>"""
        return web.Response(text=html, content_type="text/html")

    async def handle_root(self, request):
        """GET / — WebSocket upgrade → gateway handler; normal GET → map.html."""
        # If this is a WebSocket upgrade request, handle as gateway connection
        if request.headers.get("Upgrade", "").lower() == "websocket":
            return await self.ws_handler(request)
        # Otherwise serve the map page
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
        map_file = os.path.join(web_dir, "map.html")
        if os.path.exists(map_file):
            return web.FileResponse(map_file)
        return web.Response(text="map.html not found — place it in gateway/web/", status=404)

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
        """Start the hub — single port serves WebSocket + HTTP + map."""
        log.info(f"TiggyOpenMesh Gateway Hub starting on port {self.listen_port}")
        log.info(f"  WebSocket: ws://0.0.0.0:{self.listen_port}/ws")
        log.info(f"  Map:       http://0.0.0.0:{self.listen_port}/")
        log.info(f"  API:       http://0.0.0.0:{self.listen_port}/api/gateways")
        log.info(f"  Sensors:   http://0.0.0.0:{self.listen_port}/api/sensors")
        log.info(f"  Gauge:     http://0.0.0.0:{self.listen_port}/gauge/{{node}}/{{pin}}")
        if self.auth_key:
            log.info(f"  Authentication: ENABLED (key required)")
        else:
            log.info(f"  Authentication: DISABLED (open relay)")

        app = web.Application()
        # Explicit /ws path for gateways
        app.router.add_get("/ws", self.ws_handler)
        # HTTP endpoints for map API
        app.router.add_get("/api/gateways", self.handle_api_gateways)
        app.router.add_get("/api/sensors", self.handle_api_sensors)
        app.router.add_get("/api/sensor/{node}/{pin}", self.handle_api_sensor)
        app.router.add_get("/gauge/{node}/{pin}", self.handle_gauge)
        # Static files from gateway/web/
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
        if os.path.isdir(web_dir):
            app.router.add_static("/static/", web_dir)
        # Root: WebSocket upgrade → gateway; normal GET → map.html
        # This allows old gateways connecting to ws://hub:9000/ to still work
        app.router.add_get("/", self.handle_root)

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "0.0.0.0", self.listen_port)
        await site.start()

        status_task = asyncio.create_task(self.status_printer())

        try:
            while self.running:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass
        finally:
            await runner.cleanup()

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

  # All on one port:
  #   WebSocket: ws://host:9000/  or  ws://host:9000/ws
  #   Map:       http://host:9000/
  #   API:       http://host:9000/api/gateways
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
