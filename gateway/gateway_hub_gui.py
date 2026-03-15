#!/usr/bin/env python3
"""
TiggyOpenMesh Gateway Hub — GUI Dashboard
══════════════════════════════════════
Visual dashboard for the central hub relay server.
Shows connected gateways, packet flow, live statistics.

Double-click to run or:  python gateway_hub_gui.py
"""

import asyncio
import json
import math
import os
import queue
import random
import threading
import time

import customtkinter as ctk
import aiohttp
from aiohttp import web

from gui_common import COLORS, HexPacketParser, format_uptime, decrypt_message

# ─── Theme ───────────────────────────────────────────────────

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("dark-blue")


# ─── Hub with Event Queue ───────────────────────────────────

class PacketDedup:
    def __init__(self):
        self._seen: dict[str, float] = {}

    def is_duplicate(self, hex_data: str) -> bool:
        now = time.time()
        if len(self._seen) > 512:
            self._seen = {k: v for k, v in self._seen.items() if now - v < 30}
        if hex_data in self._seen and now - self._seen[hex_data] < 30:
            return True
        self._seen[hex_data] = now
        return False


class ConnectedGateway:
    def __init__(self, ws, gateway_id="pending", name=""):
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


class GUIGatewayHub:
    """Gateway hub that pushes events to a GUI queue."""

    def __init__(self, listen_port: int, auth_key: str | None, event_queue: queue.Queue):
        self.listen_port = listen_port
        self.auth_key = auth_key if auth_key else None
        self.eq = event_queue
        self.dedup = PacketDedup()
        self.gateways: dict = {}
        self.running = True
        self.total_packets_relayed = 0
        self.total_connections = 0

        # Gateway registry (persisted)
        self.registry_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gateways.json")
        self.registry: dict[str, dict] = {}
        self._load_registry()

        # Sensor history — written by GUI when it decrypts SDATA packets
        self.sensor_history: dict[str, list[tuple[float, int]]] = {}

    def _load_registry(self):
        try:
            if os.path.exists(self.registry_path):
                with open(self.registry_path, "r") as f:
                    self.registry = json.load(f)
        except Exception:
            self.registry = {}

    def _save_registry(self):
        try:
            with open(self.registry_path, "w") as f:
                json.dump(self.registry, f, indent=2)
        except Exception:
            pass

    def _update_registry(self, gw: ConnectedGateway):
        self.registry[gw.gateway_id] = {
            "name": gw.name,
            "lat": gw.lat,
            "lon": gw.lon,
            "antenna_type": gw.antenna_type,
            "antenna_height": gw.antenna_height,
            "last_seen": time.time(),
        }
        self._save_registry()

    def _get_gateways_json(self):
        result = {}
        for gw_id, info in self.registry.items():
            if info.get("lat", 0) == 0 and info.get("lon", 0) == 0:
                continue
            result[gw_id] = {
                "id": gw_id, "name": info.get("name", gw_id),
                "lat": info.get("lat", 0), "lon": info.get("lon", 0),
                "antenna_type": info.get("antenna_type", 0),
                "antenna_height": info.get("antenna_height", 2.0),
                "online": False, "uptime": 0, "packets": 0,
            }
        for gw in self.gateways.values():
            if not gw.authenticated or (gw.lat == 0 and gw.lon == 0):
                continue
            result[gw.gateway_id] = {
                "id": gw.gateway_id, "name": gw.name,
                "lat": gw.lat, "lon": gw.lon,
                "antenna_type": gw.antenna_type,
                "antenna_height": gw.antenna_height,
                "online": True,
                "uptime": int(time.time() - gw.connected_at),
                "packets": gw.packets_relayed,
            }
        return list(result.values())

    async def handle_api_gateways(self, request):
        return web.json_response(self._get_gateways_json())

    async def handle_api_sensors(self, request):
        """Return sensor data for the web dashboard."""
        result = {}
        for key, readings in self.sensor_history.items():
            if readings:
                result[key] = {
                    "current": readings[-1][1],
                    "min": min(v for _, v in readings),
                    "max": max(v for _, v in readings),
                    "count": len(readings),
                    "history": [{"t": t, "v": v} for t, v in readings[-60:]],
                }
        return web.json_response(result)

    def _emit(self, event_type: str, **kwargs):
        self.eq.put({"type": event_type, "time": time.time(), **kwargs})

    async def ws_handler(self, request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)

        remote = str(request.remote)
        self.total_connections += 1
        gw = ConnectedGateway(ws)
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
                self._emit("gateway_disconnected", name=gw.name, gw_id=gw.gateway_id)
                # Notify others
                await self.broadcast_event({
                    "type": "peer_left", "name": gw.name,
                }, exclude=ws)
            self._emit("stats", **self._stats())

        return ws

    async def handle_message(self, raw: str, source_ws):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return
        gw = self.gateways.get(source_ws)
        if not gw:
            return

        msg_type = msg.get("type", "")

        if msg_type == "auth":
            gw.gateway_id = msg.get("id", msg.get("gateway_id", "?"))
            gw.name = msg.get("name", gw.gateway_id)
            gw.lat = float(msg.get("lat", 0.0))
            gw.lon = float(msg.get("lon", 0.0))
            gw.antenna_type = int(msg.get("antenna", 0))
            gw.antenna_height = float(msg.get("height", 2.0))
            key = msg.get("key", "")
            if self.auth_key and key != self.auth_key:
                await source_ws.send_str(json.dumps({"type": "auth_result", "success": False, "error": "Invalid key"}))
                await source_ws.close()
                return
            gw.authenticated = True
            self._update_registry(gw)
            peer_count = sum(1 for g in self.gateways.values() if g.authenticated and g.ws is not source_ws)
            await source_ws.send_str(json.dumps({"type": "auth_result", "success": True, "peers": peer_count}))
            await self.broadcast_event({"type": "peer_joined", "gateway_id": gw.gateway_id, "name": gw.name, "peers": peer_count + 1}, exclude=source_ws)
            self._emit("gateway_connected", name=gw.name, gw_id=gw.gateway_id)
            self._emit("stats", **self._stats())

        elif msg_type == "pkt":
            if self.auth_key and not gw.authenticated:
                return
            hex_data = msg.get("data", "")
            origin = msg.get("origin", "")
            if not hex_data or self.dedup.is_duplicate(hex_data):
                return
            gw.packets_relayed += 1
            self.total_packets_relayed += 1

            relay_msg = json.dumps({"type": "pkt", "data": hex_data, "origin": origin})
            relay_count = 0
            targets = []
            for ws, other in list(self.gateways.items()):
                if ws is source_ws or (self.auth_key and not other.authenticated):
                    continue
                try:
                    await ws.send_str(relay_msg)
                    relay_count += 1
                    targets.append(other.name)
                except (ConnectionError, ConnectionResetError):
                    pass

            parsed = HexPacketParser.parse_raw(hex_data)
            self._emit("packet_relayed", source=gw.name, source_id=gw.gateway_id,
                        targets=targets, relay_count=relay_count,
                        size=len(hex_data) // 2, parsed=parsed,
                        hex_preview=hex_data[:24])
            self._emit("stats", **self._stats())

        elif msg_type == "status":
            authenticated = [g for g in self.gateways.values() if g.authenticated]
            await source_ws.send_str(json.dumps({
                "type": "hub_status",
                "gateways": [{"id": g.gateway_id, "name": g.name,
                              "uptime": int(time.time() - g.connected_at),
                              "packets": g.packets_relayed} for g in authenticated],
                "total_relayed": self.total_packets_relayed,
                "total_connections": self.total_connections,
            }))

    async def broadcast_event(self, msg: dict, exclude=None):
        raw = json.dumps(msg)
        for ws, gw in list(self.gateways.items()):
            if ws is exclude or not gw.authenticated:
                continue
            try:
                await ws.send_str(raw)
            except (ConnectionError, ConnectionResetError):
                pass

    def _stats(self) -> dict:
        auth = [g for g in self.gateways.values() if g.authenticated]
        return {
            "gateways_online": len(auth),
            "total_relayed": self.total_packets_relayed,
            "total_connections": self.total_connections,
            "gateway_list": [{"name": g.name, "id": g.gateway_id,
                              "uptime": time.time() - g.connected_at,
                              "packets": g.packets_relayed} for g in auth],
        }

    async def handle_root(self, request):
        """GET / — WebSocket upgrade → gateway handler; normal GET → map.html."""
        if request.headers.get("Upgrade", "").lower() == "websocket":
            return await self.ws_handler(request)
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
        map_file = os.path.join(web_dir, "map.html")
        if os.path.exists(map_file):
            return web.FileResponse(map_file)
        return web.Response(text="map.html not found", status=404)

    async def run(self):
        app = web.Application()
        app.router.add_get("/ws", self.ws_handler)
        app.router.add_get("/api/gateways", self.handle_api_gateways)
        app.router.add_get("/api/sensors", self.handle_api_sensors)
        web_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
        if os.path.isdir(web_dir):
            app.router.add_static("/static/", web_dir)
        app.router.add_get("/", self.handle_root)

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "0.0.0.0", self.listen_port)
        await site.start()

        self._emit("hub_started", port=self.listen_port)
        try:
            while self.running:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass
        finally:
            await runner.cleanup()

    def shutdown(self):
        self.running = False


# ─── Animated Particle ──────────────────────────────────────

class Particle:
    def __init__(self, x1, y1, x2, y2):
        self.x1, self.y1 = x1, y1
        self.x2, self.y2 = x2, y2
        self.born = time.time()
        self.duration = 0.6

    @property
    def alive(self):
        return time.time() - self.born < self.duration

    @property
    def pos(self):
        t = min(1.0, (time.time() - self.born) / self.duration)
        # Ease-out cubic
        t = 1.0 - (1.0 - t) ** 3
        return self.x1 + (self.x2 - self.x1) * t, self.y1 + (self.y2 - self.y1) * t


# ─── Hub GUI App ────────────────────────────────────────────

class HubGUIApp:
    def __init__(self):
        self.root = ctk.CTk()
        self.root.title("TiggyOpenMesh Gateway Hub")
        self.root.geometry("900x800")
        self.root.configure(fg_color=COLORS["bg"])
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.event_queue = queue.Queue()
        self.hub: GUIGatewayHub | None = None
        self.async_thread: threading.Thread | None = None
        self.loop: asyncio.AbstractEventLoop | None = None
        self.gateway_nodes: dict[str, dict] = {}  # id -> {name, x, y, angle}
        self.particles: list[Particle] = []
        self.hub_running = False

        # Sensor monitoring
        self.sensor_data: dict[str, list[tuple[float, int]]] = {}  # "node:pin" -> [(ts, val)]
        self.max_sensor_history = 120

        self.build_ui()
        self.root.after(50, self.poll_events)
        self.root.after(33, self.animate_canvas)
        self.root.after(2000, self.redraw_sensors)

    # ─── UI Construction ────────────────────────────────────

    def build_ui(self):
        # Title bar
        title_frame = ctk.CTkFrame(self.root, fg_color=COLORS["header"], height=40, corner_radius=0)
        title_frame.pack(fill="x")
        title_frame.pack_propagate(False)
        ctk.CTkLabel(title_frame, text="  TiggyOpenMesh Gateway Hub", font=("Consolas", 16, "bold"),
                      text_color=COLORS["accent"]).pack(side="left", padx=10)
        self.status_label = ctk.CTkLabel(title_frame, text="Stopped", font=("Consolas", 12),
                                          text_color=COLORS["bad"])
        self.status_label.pack(side="right", padx=15)

        # Controls
        ctrl_frame = ctk.CTkFrame(self.root, fg_color=COLORS["panel"], corner_radius=8)
        ctrl_frame.pack(fill="x", padx=10, pady=(10, 5))

        ctk.CTkLabel(ctrl_frame, text="Listen Port:", text_color=COLORS["dim"]).pack(side="left", padx=(10, 5))
        self.port_entry = ctk.CTkEntry(ctrl_frame, width=80, placeholder_text="9000")
        self.port_entry.insert(0, "9000")
        self.port_entry.pack(side="left", padx=5)

        ctk.CTkLabel(ctrl_frame, text="Auth Key:", text_color=COLORS["dim"]).pack(side="left", padx=(15, 5))
        self.key_entry = ctk.CTkEntry(ctrl_frame, width=120, placeholder_text="(optional)")
        self.key_entry.pack(side="left", padx=5)

        ctk.CTkLabel(ctrl_frame, text="AES Key:", text_color=COLORS["dim"]).pack(side="left", padx=(10, 5))
        self.aes_entry = ctk.CTkEntry(ctrl_frame, width=140, placeholder_text="16-char decrypt")
        self.aes_entry.pack(side="left", padx=5)

        self.start_btn = ctk.CTkButton(ctrl_frame, text="START", width=80, fg_color=COLORS["good"],
                                         text_color="#000", hover_color="#00CC00", command=self.start_hub)
        self.start_btn.pack(side="left", padx=(15, 5))
        self.stop_btn = ctk.CTkButton(ctrl_frame, text="STOP", width=80, fg_color=COLORS["bad"],
                                        text_color="#FFF", hover_color="#CC0000", command=self.stop_hub, state="disabled")
        self.stop_btn.pack(side="left", padx=5, pady=8)

        # Middle: Stats + Gateway Table
        mid_frame = ctk.CTkFrame(self.root, fg_color="transparent")
        mid_frame.pack(fill="both", padx=10, pady=5)
        mid_frame.grid_columnconfigure(1, weight=1)
        mid_frame.grid_rowconfigure(0, weight=1)

        # Stats cards
        stats_frame = ctk.CTkFrame(mid_frame, fg_color=COLORS["panel"], width=160, corner_radius=8)
        stats_frame.grid(row=0, column=0, sticky="ns", padx=(0, 5))
        stats_frame.grid_propagate(False)
        ctk.CTkLabel(stats_frame, text="Statistics", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"]).pack(pady=(10, 5))

        self.stat_relayed = ctk.CTkLabel(stats_frame, text="0", font=("Consolas", 24, "bold"),
                                          text_color=COLORS["accent"])
        self.stat_relayed.pack(pady=(10, 0))
        ctk.CTkLabel(stats_frame, text="Packets Relayed", text_color=COLORS["dim"],
                      font=("Consolas", 9)).pack()

        self.stat_gateways = ctk.CTkLabel(stats_frame, text="0", font=("Consolas", 24, "bold"),
                                           text_color=COLORS["good"])
        self.stat_gateways.pack(pady=(15, 0))
        ctk.CTkLabel(stats_frame, text="Gateways Online", text_color=COLORS["dim"],
                      font=("Consolas", 9)).pack()

        self.stat_connections = ctk.CTkLabel(stats_frame, text="0", font=("Consolas", 24, "bold"),
                                              text_color=COLORS["warn"])
        self.stat_connections.pack(pady=(15, 0))
        ctk.CTkLabel(stats_frame, text="Total Connections", text_color=COLORS["dim"],
                      font=("Consolas", 9)).pack()

        # Gateway table
        gw_frame = ctk.CTkFrame(mid_frame, fg_color=COLORS["panel"], corner_radius=8)
        gw_frame.grid(row=0, column=1, sticky="nsew")

        ctk.CTkLabel(gw_frame, text="  Connected Gateways", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(10, 0))

        header = ctk.CTkFrame(gw_frame, fg_color=COLORS["header"], height=25, corner_radius=0)
        header.pack(fill="x", padx=10, pady=(5, 0))
        header.pack_propagate(False)
        for text, w in [("Status", 50), ("Name", 140), ("ID", 100), ("Uptime", 80), ("Packets", 80)]:
            ctk.CTkLabel(header, text=text, font=("Consolas", 9, "bold"), text_color=COLORS["dim"],
                          width=w, anchor="w").pack(side="left", padx=2)

        self.gw_list_frame = ctk.CTkScrollableFrame(gw_frame, fg_color=COLORS["bg"], corner_radius=0)
        self.gw_list_frame.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        # Flow canvas
        canvas_frame = ctk.CTkFrame(self.root, fg_color=COLORS["panel"], corner_radius=8, height=160)
        canvas_frame.pack(fill="x", padx=10, pady=5)
        canvas_frame.pack_propagate(False)
        ctk.CTkLabel(canvas_frame, text="  Cross-Island Packet Flow", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(5, 0))
        self.canvas = ctk.CTkCanvas(canvas_frame, bg=COLORS["bg"], highlightthickness=0, height=120)
        self.canvas.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        # Sensor dashboard
        sensor_frame = ctk.CTkFrame(self.root, fg_color=COLORS["panel"], corner_radius=8, height=140)
        sensor_frame.pack(fill="x", padx=10, pady=5)
        sensor_frame.pack_propagate(False)
        ctk.CTkLabel(sensor_frame, text="  Sensor Monitor", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(5, 0))
        self.sensor_canvas = ctk.CTkCanvas(sensor_frame, bg=COLORS["bg"], highlightthickness=0, height=100)
        self.sensor_canvas.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        # Packet log
        log_frame = ctk.CTkFrame(self.root, fg_color=COLORS["panel"], corner_radius=8)
        log_frame.pack(fill="both", expand=True, padx=10, pady=(5, 10))
        ctk.CTkLabel(log_frame, text="  Packet Log", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(5, 0))
        self.log_text = ctk.CTkTextbox(log_frame, font=("Consolas", 10), fg_color=COLORS["bg"],
                                        text_color=COLORS["dim"], height=120, state="disabled")
        self.log_text.pack(fill="both", expand=True, padx=10, pady=(0, 10))

    # ─── Hub Control ────────────────────────────────────────

    def start_hub(self):
        try:
            port = int(self.port_entry.get())
        except ValueError:
            port = 9000
        key = self.key_entry.get().strip() or None

        self.hub = GUIGatewayHub(port, key, self.event_queue)
        self.hub_running = True

        def run_async():
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)
            self.loop.run_until_complete(self.hub.run())

        self.async_thread = threading.Thread(target=run_async, daemon=True)
        self.async_thread.start()

        self.status_label.configure(text=f"Running on :{port}", text_color=COLORS["good"])
        self.start_btn.configure(state="disabled")
        self.stop_btn.configure(state="normal")
        self.port_entry.configure(state="disabled")
        self.key_entry.configure(state="disabled")

    def stop_hub(self):
        if self.hub:
            self.hub.shutdown()
        self.hub_running = False
        self.status_label.configure(text="Stopped", text_color=COLORS["bad"])
        self.start_btn.configure(state="normal")
        self.stop_btn.configure(state="disabled")
        self.port_entry.configure(state="normal")
        self.key_entry.configure(state="normal")

    # ─── Event Processing ───────────────────────────────────

    def poll_events(self):
        try:
            while True:
                event = self.event_queue.get_nowait()
                self.handle_event(event)
        except queue.Empty:
            pass
        self.root.after(50, self.poll_events)

    def handle_event(self, event):
        etype = event.get("type", "")

        if etype == "stats":
            self.stat_relayed.configure(text=str(event.get("total_relayed", 0)))
            self.stat_gateways.configure(text=str(event.get("gateways_online", 0)))
            self.stat_connections.configure(text=str(event.get("total_connections", 0)))
            self.update_gateway_table(event.get("gateway_list", []))

        elif etype == "gateway_connected":
            name = event.get("name", "?")
            gw_id = event.get("gw_id", "?")
            self.log_append(f"Gateway connected: {name} ({gw_id})", COLORS["good"])
            # Add to flow canvas
            angle = len(self.gateway_nodes) * (2 * math.pi / max(1, len(self.gateway_nodes) + 1))
            self.gateway_nodes[gw_id] = {"name": name, "angle": angle}
            self.layout_canvas_nodes()

        elif etype == "gateway_disconnected":
            name = event.get("name", "?")
            gw_id = event.get("gw_id", "?")
            self.log_append(f"Gateway disconnected: {name}", COLORS["bad"])
            self.gateway_nodes.pop(gw_id, None)
            self.layout_canvas_nodes()

        elif etype == "packet_relayed":
            src = event.get("source", "?")
            targets = event.get("targets", [])
            size = event.get("size", 0)
            parsed = event.get("parsed")
            ptype = parsed.get("type", "?") if parsed else "?"
            dest_str = ", ".join(targets) if targets else "ALL"
            self.log_append(f"{src} → {dest_str}  {size}B  [{ptype}]  relay OK", COLORS["accent"])

            # Try to decrypt and extract SDATA
            aes_key = self.aes_entry.get().strip()
            if len(aes_key) == 16 and parsed and parsed.get("type") == "MSG":
                encrypted = parsed.get("encrypted", "")
                msg_id = parsed.get("msg_id", "")
                if encrypted:
                    plain = decrypt_message(encrypted, msg_id, aes_key)
                    if plain and plain.startswith("SDATA,"):
                        self._parse_sdata(plain)

            # Fire particles on canvas
            src_id = event.get("source_id", "")
            if src_id in self.gateway_nodes:
                sx, sy = self.get_node_pos(src_id)
                cx, cy = self.get_hub_pos()
                self.particles.append(Particle(sx, sy, cx, cy))
                # Hub to targets
                for tname in targets:
                    for tid, tdata in self.gateway_nodes.items():
                        if tdata["name"] == tname:
                            tx, ty = self.get_node_pos(tid)
                            self.particles.append(Particle(cx, cy, tx, ty))

        elif etype == "hub_started":
            self.log_append(f"Hub started on port {event.get('port', '?')}", COLORS["good"])

    def update_gateway_table(self, gw_list: list):
        for widget in self.gw_list_frame.winfo_children():
            widget.destroy()
        for gw in gw_list:
            row = ctk.CTkFrame(self.gw_list_frame, fg_color=COLORS["panel"], height=28, corner_radius=4)
            row.pack(fill="x", pady=1)
            row.pack_propagate(False)
            ctk.CTkLabel(row, text="●", text_color=COLORS["good"], width=50, font=("Consolas", 14)).pack(side="left", padx=2)
            ctk.CTkLabel(row, text=gw["name"], text_color=COLORS["text"], width=140,
                          font=("Consolas", 10), anchor="w").pack(side="left", padx=2)
            ctk.CTkLabel(row, text=gw["id"][:8], text_color=COLORS["dim"], width=100,
                          font=("Consolas", 10), anchor="w").pack(side="left", padx=2)
            ctk.CTkLabel(row, text=format_uptime(gw["uptime"]), text_color=COLORS["dim"], width=80,
                          font=("Consolas", 10), anchor="w").pack(side="left", padx=2)
            ctk.CTkLabel(row, text=str(gw["packets"]), text_color=COLORS["accent"], width=80,
                          font=("Consolas", 10), anchor="w").pack(side="left", padx=2)

    def log_append(self, text: str, color: str = COLORS["dim"]):
        ts = time.strftime("%H:%M:%S")
        self.log_text.configure(state="normal")
        self.log_text.insert("end", f"{ts}  {text}\n")
        # Keep last 200 lines
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 200:
            self.log_text.delete("1.0", f"{lines - 200}.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    # ─── Flow Canvas ────────────────────────────────────────

    def get_hub_pos(self):
        w = self.canvas.winfo_width() or 400
        h = self.canvas.winfo_height() or 120
        return w // 2, h // 2

    def get_node_pos(self, gw_id: str):
        w = self.canvas.winfo_width() or 400
        h = self.canvas.winfo_height() or 120
        cx, cy = w // 2, h // 2
        node = self.gateway_nodes.get(gw_id)
        if not node:
            return cx, cy
        radius = min(w, h) * 0.35
        return int(cx + radius * math.cos(node["angle"])), int(cy + radius * math.sin(node["angle"]))

    def layout_canvas_nodes(self):
        n = len(self.gateway_nodes)
        for i, (gw_id, data) in enumerate(self.gateway_nodes.items()):
            data["angle"] = i * (2 * math.pi / max(1, n))

    def animate_canvas(self):
        self.canvas.delete("all")
        w = self.canvas.winfo_width() or 400
        h = self.canvas.winfo_height() or 120
        cx, cy = w // 2, h // 2

        # Draw edges to hub
        for gw_id in self.gateway_nodes:
            nx, ny = self.get_node_pos(gw_id)
            self.canvas.create_line(cx, cy, nx, ny, fill=COLORS["faint"], width=1, dash=(4, 4))

        # Draw hub node
        self.canvas.create_oval(cx - 18, cy - 18, cx + 18, cy + 18, fill=COLORS["header"],
                                 outline=COLORS["accent"], width=2)
        self.canvas.create_text(cx, cy, text="HUB", fill=COLORS["accent"], font=("Consolas", 8, "bold"))

        # Draw gateway nodes
        for gw_id, data in self.gateway_nodes.items():
            nx, ny = self.get_node_pos(gw_id)
            # Glow
            self.canvas.create_oval(nx - 16, ny - 16, nx + 16, ny + 16, fill=COLORS["panel"],
                                     outline=COLORS["good"], width=2)
            self.canvas.create_text(nx, ny, text=data["name"][:6], fill=COLORS["text"],
                                     font=("Consolas", 7))

        # Draw particles
        self.particles = [p for p in self.particles if p.alive]
        for p in self.particles:
            px, py = p.pos
            self.canvas.create_oval(px - 4, py - 4, px + 4, py + 4, fill=COLORS["accent"], outline="")

        # No gateways message
        if not self.gateway_nodes and self.hub_running:
            self.canvas.create_text(cx, cy + 30, text="Waiting for gateways...",
                                     fill=COLORS["dim"], font=("Consolas", 10))

        self.root.after(33, self.animate_canvas)

    # ─── Sensor Dashboard ─────────────────────────────────

    def _parse_sdata(self, line: str):
        """Parse SDATA,<nodeId>,<pin>:<val>,... and store readings."""
        parts = line.split(",")
        if len(parts) < 3:
            return
        node_id = parts[1]
        now = time.time()
        for part in parts[2:]:
            pv = part.split(":")
            if len(pv) != 2:
                continue
            try:
                pin, val = int(pv[0]), int(pv[1])
            except ValueError:
                continue
            key = f"{node_id}:{pin}"
            if key not in self.sensor_data:
                self.sensor_data[key] = []
            self.sensor_data[key].append((now, val))
            if len(self.sensor_data[key]) > self.max_sensor_history:
                self.sensor_data[key] = self.sensor_data[key][-self.max_sensor_history:]
            # Also write to hub's sensor_history for the API
            if self.hub:
                if key not in self.hub.sensor_history:
                    self.hub.sensor_history[key] = []
                self.hub.sensor_history[key].append((now, val))
                if len(self.hub.sensor_history[key]) > self.max_sensor_history:
                    self.hub.sensor_history[key] = self.hub.sensor_history[key][-self.max_sensor_history:]
        self.log_append(f"SDATA from {node_id}: {len(parts)-2} sensors", COLORS["good"])

    def redraw_sensors(self):
        """Redraw sensor sparkline canvas."""
        c = self.sensor_canvas
        c.delete("all")
        w = c.winfo_width() or 400
        h = c.winfo_height() or 100

        if not self.sensor_data:
            aes_key = self.aes_entry.get().strip()
            hint = "Enter 16-char AES key to decode sensor data" if len(aes_key) != 16 else "Waiting for SDATA packets..."
            c.create_text(w // 2, h // 2, text=hint,
                           fill=COLORS["dim"], font=("Consolas", 10))
            self.root.after(2000, self.redraw_sensors)
            return

        # Layout: evenly divide width among sensor keys
        keys = sorted(self.sensor_data.keys())
        n = len(keys)
        chart_w = max(60, (w - 20) // min(n, 6))  # max 6 per row
        chart_h = 60
        palette = ["#00E5FF", "#00E676", "#FF9100", "#448AFF", "#FF5252", "#FFE000"]

        for idx, key in enumerate(keys[:12]):  # max 12 sensors
            col = idx % 6
            row = idx // 6
            x0 = 10 + col * chart_w
            y0 = 5 + row * (chart_h + 20)

            readings = self.sensor_data[key]
            values = [v for _, v in readings]

            # Label
            c.create_text(x0 + chart_w // 2, y0, text=key, fill=COLORS["text"],
                           font=("Consolas", 8), anchor="n")

            # Current value
            c.create_text(x0 + chart_w - 5, y0, text=str(values[-1]),
                           fill=palette[idx % len(palette)], font=("Consolas", 9, "bold"), anchor="ne")

            # Sparkline
            if len(values) >= 2:
                max_v = max(values)
                min_v = min(values)
                vrange = max(max_v - min_v, 1)
                step = (chart_w - 10) / (len(values) - 1)
                sy = y0 + 12
                points = []
                for i, v in enumerate(values):
                    px = x0 + 5 + i * step
                    py = sy + chart_h - 12 - ((v - min_v) / vrange * (chart_h - 16))
                    points.extend([px, py])
                if len(points) >= 4:
                    c.create_line(points, fill=palette[idx % len(palette)], width=2, smooth=True)

        self.root.after(2000, self.redraw_sensors)

    # ─── Lifecycle ──────────────────────────────────────────

    def on_close(self):
        self.stop_hub()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


# ─── Entry Point ─────────────────────────────────────────────

if __name__ == "__main__":
    app = HubGUIApp()
    app.run()
