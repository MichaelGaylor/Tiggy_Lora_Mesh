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
import queue
import random
import threading
import time

import customtkinter as ctk
import websockets
from websockets.asyncio.server import serve

from gui_common import COLORS, HexPacketParser, format_uptime

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

    def _emit(self, event_type: str, **kwargs):
        self.eq.put({"type": event_type, "time": time.time(), **kwargs})

    async def ws_handler(self, websocket):
        remote = str(websocket.remote_address)
        self.total_connections += 1
        gw = ConnectedGateway(websocket)
        self.gateways[websocket] = gw

        try:
            async for message in websocket:
                await self.handle_message(message, websocket)
        except websockets.ConnectionClosed:
            pass
        finally:
            if websocket in self.gateways:
                gw = self.gateways.pop(websocket)
                self._emit("gateway_disconnected", name=gw.name, gw_id=gw.gateway_id)
                # Notify others
                await self.broadcast_event({
                    "type": "peer_left", "name": gw.name,
                }, exclude=websocket)
            self._emit("stats", **self._stats())

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
            gw.gateway_id = msg.get("gateway_id", "?")
            gw.name = msg.get("name", gw.gateway_id)
            key = msg.get("key", "")
            if self.auth_key and key != self.auth_key:
                await source_ws.send(json.dumps({"type": "auth_result", "success": False, "error": "Invalid key"}))
                await source_ws.close()
                return
            gw.authenticated = True
            peer_count = sum(1 for g in self.gateways.values() if g.authenticated and g.ws is not source_ws)
            await source_ws.send(json.dumps({"type": "auth_result", "success": True, "peers": peer_count}))
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
                    await ws.send(relay_msg)
                    relay_count += 1
                    targets.append(other.name)
                except websockets.ConnectionClosed:
                    pass

            parsed = HexPacketParser.parse_raw(hex_data)
            self._emit("packet_relayed", source=gw.name, source_id=gw.gateway_id,
                        targets=targets, relay_count=relay_count,
                        size=len(hex_data) // 2, parsed=parsed,
                        hex_preview=hex_data[:24])
            self._emit("stats", **self._stats())

        elif msg_type == "status":
            authenticated = [g for g in self.gateways.values() if g.authenticated]
            await source_ws.send(json.dumps({
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
                await ws.send(raw)
            except websockets.ConnectionClosed:
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

    async def run(self):
        server = await serve(self.ws_handler, "0.0.0.0", self.listen_port)
        self._emit("hub_started", port=self.listen_port)
        try:
            while self.running:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass
        finally:
            server.close()

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
        self.root.geometry("900x700")
        self.root.configure(fg_color=COLORS["bg"])
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.event_queue = queue.Queue()
        self.hub: GUIGatewayHub | None = None
        self.async_thread: threading.Thread | None = None
        self.loop: asyncio.AbstractEventLoop | None = None
        self.gateway_nodes: dict[str, dict] = {}  # id -> {name, x, y, angle}
        self.particles: list[Particle] = []
        self.hub_running = False

        self.build_ui()
        self.root.after(50, self.poll_events)
        self.root.after(33, self.animate_canvas)

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
        self.key_entry = ctk.CTkEntry(ctrl_frame, width=180, placeholder_text="(optional)")
        self.key_entry.pack(side="left", padx=5)

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
