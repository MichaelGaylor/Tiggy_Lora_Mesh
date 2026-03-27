#!/usr/bin/env python3
"""
TiggyOpenMesh Gateway Client — GUI
═══════════════════════════════
Visual gateway client with mesh topology, packet waterfall, and inspector.
Connects to a repeater via USB serial and to the hub via WebSocket.

Double-click to run or:  python gateway_gui.py
"""

import asyncio
import collections
import json
import math
import os
import queue
import secrets
import threading
import time

import tkinter
import customtkinter as ctk
import serial
import serial.tools.list_ports
import websockets

from gui_common import (COLORS, HexPacketParser, decrypt_message,
                         detect_serial_ports, format_uptime, rssi_to_color)
from automation_engine import AutomationEngine, BLOCK_DEFS
from automation_canvas import AutomationCanvas, CATEGORY_BLOCKS
from sensor_dashboard import SensorDashboard

# ─── Theme ───────────────────────────────────────────────────

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("dark-blue")


# ─── Gateway with Event Queue ───────────────────────────────

class PacketDedup:
    def __init__(self):
        self._seen: dict[str, float] = {}

    def is_duplicate(self, hex_data: str) -> bool:
        now = time.time()
        if len(self._seen) > 256:
            self._seen = {k: v for k, v in self._seen.items() if now - v < 30}
        if hex_data in self._seen and now - self._seen[hex_data] < 30:
            return True
        self._seen[hex_data] = now
        return False


class GUIGatewayServer:
    """Gateway server that pushes events to a GUI queue."""

    def __init__(self, serial_port: str, baud: int, hub_url: str | None,
                 hub_key: str | None, name: str, eq: queue.Queue):
        self.serial_port = serial_port
        self.baud = baud
        self.hub_url = hub_url if hub_url else None
        self.hub_key = hub_key if hub_key else None
        # Use a stable gateway ID based on the name so the hub recognises reconnections
        self.name = name or "gateway"
        self.gateway_id = name or secrets.token_hex(4)
        self.lat = 0.0
        self.lon = 0.0
        self.antenna_type = 0
        self.antenna_height = 2.0
        self.eq = eq
        self.dedup = PacketDedup()
        self.serial_conn: serial.Serial | None = None
        self.hub_ws = None
        self.running = True

        self.packets_from_radio = 0
        self.packets_from_peers = 0
        self.packets_to_radio = 0
        self.packets_to_peers = 0

    def _emit(self, event_type: str, **kwargs):
        self.eq.put({"type": event_type, "time": time.time(), **kwargs})

    def open_serial(self):
        try:
            self.serial_conn = serial.Serial(self.serial_port, self.baud, timeout=0.1)
            self._emit("log", text=f"Serial opened: {self.serial_port} @ {self.baud}", color=COLORS["good"])
            time.sleep(2)
            self.serial_conn.write(b"GATEWAY ON\n")
            time.sleep(0.5)
            self.serial_conn.write(b"STATUS\n")    # Get node ID for Logic Builder
            time.sleep(0.5)
            self.serial_conn.write(b"CMD,LIST\n")  # Query pin config for Logic Builder
            time.sleep(0.5)
            self.serial_conn.write(b"POLL\n")      # Get initial sensor data immediately
            time.sleep(0.3)
            # Read all responses (STATUS multi-line + PINS)
            for _ in range(20):
                if not self.serial_conn.in_waiting:
                    time.sleep(0.1)
                    continue
                line = self.serial_conn.readline().decode("ascii", errors="replace").strip()
                if line:
                    self._emit("serial_line", text=line)
        except serial.SerialException as e:
            self._emit("log", text=f"Serial error: {e}", color=COLORS["bad"])
            self.running = False

    async def serial_reader(self):
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
                        # Format: PKT,<hex>,<rssi> or PKT,<hex>
                        pkt_parts = line_str[4:].rsplit(",", 1)
                        hex_data = pkt_parts[0]
                        pkt_rssi = int(pkt_parts[1]) if len(pkt_parts) > 1 else -100
                        if self.dedup.is_duplicate(hex_data):
                            continue
                        self.packets_from_radio += 1
                        parsed = HexPacketParser.parse_raw(hex_data)
                        if parsed:
                            parsed["rssi"] = pkt_rssi
                        self._emit("packet_from_radio", hex_data=hex_data, parsed=parsed)
                        self._emit("stats", **self._stats())
                        await self.send_to_hub(hex_data)
                    elif line_str:
                        self._emit("serial_line", text=line_str)
            except Exception as e:
                self._emit("log", text=f"Serial error: {e}", color=COLORS["bad"])
                await asyncio.sleep(1)

    def _serial_read_chunk(self) -> bytes:
        if self.serial_conn and self.serial_conn.in_waiting:
            return self.serial_conn.read(self.serial_conn.in_waiting)
        time.sleep(0.05)
        return b""

    def inject_to_radio(self, hex_data: str):
        if not self.serial_conn:
            return
        try:
            self.serial_conn.write(f"PKT,{hex_data}\n".encode("ascii"))
            self.packets_to_radio += 1
        except serial.SerialException:
            pass

    async def send_to_hub(self, hex_data: str):
        if not self.hub_ws:
            return
        try:
            msg = json.dumps({"type": "pkt", "data": hex_data, "origin": self.gateway_id})
            await self.hub_ws.send(msg)
            self.packets_to_peers += 1
        except websockets.ConnectionClosed:
            self.hub_ws = None

    async def connect_to_hub(self):
        if not self.hub_url:
            return
        while self.running:
            try:
                self._emit("log", text=f"Connecting to hub: {self.hub_url}", color=COLORS["dim"])
                async with websockets.connect(self.hub_url) as ws:
                    self.hub_ws = ws
                    auth_data = {
                        "type": "auth", "gateway_id": self.gateway_id,
                        "name": self.name, "key": self.hub_key or "",
                    }
                    if self.lat != 0.0 or self.lon != 0.0:
                        auth_data["lat"] = self.lat
                        auth_data["lon"] = self.lon
                        auth_data["height"] = self.antenna_height
                        auth_data["antenna"] = self.antenna_type
                    auth_msg = json.dumps(auth_data)
                    await ws.send(auth_msg)
                    response = await ws.recv()
                    result = json.loads(response)
                    if result.get("type") == "auth_result":
                        if result.get("success"):
                            peers = result.get("peers", 0)
                            self._emit("hub_connected", peers=peers)
                            self._emit("log", text=f"Hub connected! {peers} peer(s) online", color=COLORS["good"])
                        else:
                            self._emit("log", text=f"Hub auth failed: {result.get('error', '?')}", color=COLORS["bad"])
                            await asyncio.sleep(10)
                            continue

                    async for message in ws:
                        try:
                            msg = json.loads(message)
                        except json.JSONDecodeError:
                            continue
                        if msg.get("type") == "pkt":
                            hex_data = msg.get("data", "")
                            origin = msg.get("origin", "")
                            if origin == self.gateway_id or self.dedup.is_duplicate(hex_data):
                                continue
                            self.packets_from_peers += 1
                            parsed = HexPacketParser.parse_raw(hex_data)
                            self._emit("packet_from_hub", hex_data=hex_data, parsed=parsed)
                            self._emit("stats", **self._stats())
                            self.inject_to_radio(hex_data)
                        elif msg.get("type") == "peer_joined":
                            self._emit("log", text=f"Peer joined: {msg.get('name', '?')}", color=COLORS["good"])
                        elif msg.get("type") == "peer_left":
                            self._emit("log", text=f"Peer left: {msg.get('name', '?')}", color=COLORS["warn"])
            except (ConnectionRefusedError, OSError, websockets.ConnectionClosed) as e:
                self._emit("hub_disconnected")
                self._emit("log", text=f"Hub unavailable: {e}. Retrying...", color=COLORS["warn"])
            finally:
                self.hub_ws = None
            await asyncio.sleep(10)

    def _stats(self) -> dict:
        return {
            "radio_rx": self.packets_from_radio,
            "radio_tx": self.packets_to_radio,
            "peer_rx": self.packets_from_peers,
            "peer_tx": self.packets_to_peers,
            "hub_connected": self.hub_ws is not None,
        }

    async def run(self):
        self.open_serial()
        if not self.running:
            return
        tasks = [asyncio.create_task(self.serial_reader())]
        if self.hub_url:
            tasks.append(asyncio.create_task(self.connect_to_hub()))
        try:
            while self.running:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass

    def shutdown(self):
        self.running = False
        if self.serial_conn:
            try:
                self.serial_conn.write(b"GATEWAY OFF\n")
                time.sleep(0.2)
                self.serial_conn.close()
            except Exception:
                pass


# ─── Topology Node Physics ──────────────────────────────────

class TopoNode:
    def __init__(self, node_id: str, is_local=False):
        self.id = node_id
        self.x = 0.5 + (hash(node_id) % 100) / 500.0
        self.y = 0.5 + (hash(node_id + "y") % 100) / 500.0
        self.vx = 0.0
        self.vy = 0.0
        self.rssi = -100
        self.last_seen = 0.0
        self.hops = 0
        self.next_hop = ""
        self.is_local = is_local
        self.packets = 0


# ─── Client GUI App ─────────────────────────────────────────

class GatewayGUIApp:
    CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gateway_gui_config.json")

    def __init__(self):
        self.root = ctk.CTk()
        self.root.title("TiggyOpenMesh Gateway Client")
        self.root.geometry("1100x950")
        self.root.configure(fg_color=COLORS["bg"])
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.event_queue = queue.Queue()
        self.gateway: GUIGatewayServer | None = None
        self.async_thread: threading.Thread | None = None
        self.connected = False
        self._saved_config = self._load_config()
        self.node_names = self._saved_config.get("node_names", {})

        # Mesh state
        self.topo_nodes: dict[str, TopoNode] = {}
        self.topo_edges: dict[tuple[str, str], float] = {}  # (src,dst) -> last_seen
        self.topo_edge_rssi: dict[tuple[str, str], int] = {}  # (src,dst) -> rssi
        self.local_id = "????"
        self.packet_history: list[dict] = []
        self.selected_packet: dict | None = None
        self.particles: list = []

        # Sensor state
        self.sensor_data: dict[str, list[tuple[float, int]]] = {}  # key -> [(ts, val)]
        self.max_sensor_history = 120

        # Logic builder state
        self.logic_visible = False

        # Node pin config — defaults cover all common ESP32-S3 boards (V3, V4, XIAO)
        # CMD,LIST or STATUS can override these if they arrive
        self.node_relay_pins: list[str] = ["2","3","4","5","6","7"]
        self.node_sensor_pins: list[str] = ["15","19","20","33","34"]
        # Per-node pin configs: nodeId → (relay_pins, sensor_pins)
        self.node_pin_configs: dict[str, tuple[list[str], list[str]]] = {}
        self._queried_nodes: set[str] = set()  # Track which nodes we've already queried

        # Automation engine (before build_ui — canvas needs it)
        self.engine = AutomationEngine(self.sensor_data, self._send_serial, self.topo_nodes)

        self.build_ui()
        self._refresh_rule_list()  # Load saved rules into combo box
        # Push default pins to canvas immediately
        self.auto_canvas.relay_pins = self.node_relay_pins
        self.auto_canvas.sensor_pins = self.node_sensor_pins

        self.refresh_ports()
        self.root.after(50, self.poll_events)
        self.root.after(33, self.animate_topology)
        self.root.after(1000, self._eval_automation)

    # ─── UI Construction ────────────────────────────────────

    def build_ui(self):
        # Title bar
        title_frame = ctk.CTkFrame(self.root, fg_color=COLORS["header"], height=40, corner_radius=0)
        title_frame.pack(fill="x")
        title_frame.pack_propagate(False)
        ctk.CTkLabel(title_frame, text="  TiggyOpenMesh Gateway Client", font=("Consolas", 16, "bold"),
                      text_color=COLORS["accent"]).pack(side="left", padx=10)
        self.conn_label = ctk.CTkLabel(title_frame, text="Disconnected", font=("Consolas", 12),
                                        text_color=COLORS["bad"])
        self.conn_label.pack(side="right", padx=15)

        # Connection controls
        conn_frame = ctk.CTkFrame(self.root, fg_color=COLORS["panel"], corner_radius=8)
        conn_frame.pack(fill="x", padx=10, pady=(10, 5))

        row1 = ctk.CTkFrame(conn_frame, fg_color="transparent")
        row1.pack(fill="x", padx=10, pady=(8, 2))
        ctk.CTkLabel(row1, text="Serial:", text_color=COLORS["dim"], width=50).pack(side="left")
        self.port_combo = ctk.CTkComboBox(row1, width=200, values=["(detecting...)"])
        self.port_combo.pack(side="left", padx=5)
        self.detect_btn = ctk.CTkButton(row1, text="Detect", width=60, command=self.refresh_ports,
                                          fg_color=COLORS["faint"], text_color=COLORS["text"])
        self.detect_btn.pack(side="left", padx=5)

        ctk.CTkLabel(row1, text="Hub:", text_color=COLORS["dim"], width=35).pack(side="left", padx=(15, 0))
        self.hub_entry = ctk.CTkEntry(row1, width=250, placeholder_text="ws://192.168.1.10:9000")
        self.hub_entry.pack(side="left", padx=5)

        row2 = ctk.CTkFrame(conn_frame, fg_color="transparent")
        row2.pack(fill="x", padx=10, pady=(2, 8))
        ctk.CTkLabel(row2, text="Name:", text_color=COLORS["dim"], width=50).pack(side="left")
        self.name_entry = ctk.CTkEntry(row2, width=150, placeholder_text="My Gateway")
        self.name_entry.pack(side="left", padx=5)
        ctk.CTkLabel(row2, text="Hub Key:", text_color=COLORS["dim"]).pack(side="left", padx=(15, 0))
        self.key_entry = ctk.CTkEntry(row2, width=140, placeholder_text="(optional)")
        self.key_entry.pack(side="left", padx=5)

        ctk.CTkLabel(row2, text="Lat:", text_color=COLORS["dim"]).pack(side="left", padx=(15, 0))
        self.lat_entry = ctk.CTkEntry(row2, width=80, placeholder_text="53.123")
        self.lat_entry.pack(side="left", padx=2)
        ctk.CTkLabel(row2, text="Lon:", text_color=COLORS["dim"]).pack(side="left", padx=(5, 0))
        self.lon_entry = ctk.CTkEntry(row2, width=80, placeholder_text="-1.456")
        self.lon_entry.pack(side="left", padx=2)
        ctk.CTkLabel(row2, text="Ant H:", text_color=COLORS["dim"]).pack(side="left", padx=(5, 0))
        self.ant_height_entry = ctk.CTkEntry(row2, width=40, placeholder_text="2")
        self.ant_height_entry.pack(side="left", padx=2)

        self.connect_btn = ctk.CTkButton(row2, text="CONNECT", width=100, fg_color=COLORS["good"],
                                           text_color="#000", hover_color="#00CC00", command=self.connect)
        self.connect_btn.pack(side="left", padx=(15, 5))
        self.disconnect_btn = ctk.CTkButton(row2, text="DISCONNECT", width=100, fg_color=COLORS["bad"],
                                              text_color="#FFF", hover_color="#CC0000",
                                              command=self.disconnect, state="disabled")
        self.disconnect_btn.pack(side="left", padx=5)

        # Restore saved connection settings
        cfg = self._saved_config
        if cfg.get("hub_url"):
            self.hub_entry.insert(0, cfg["hub_url"])
        if cfg.get("gw_name"):
            self.name_entry.insert(0, cfg["gw_name"])
        if cfg.get("hub_key"):
            self.key_entry.insert(0, cfg["hub_key"])
        if cfg.get("lat"):
            self.lat_entry.insert(0, cfg["lat"])
        if cfg.get("lon"):
            self.lon_entry.insert(0, cfg["lon"])
        if cfg.get("ant_height"):
            self.ant_height_entry.insert(0, cfg["ant_height"])
        if cfg.get("serial_port"):
            self.port_combo.set(cfg["serial_port"])

        # Telegram bot config row
        tg_row = ctk.CTkFrame(self.root, fg_color="transparent")
        tg_row.pack(fill="x", padx=10, pady=(0, 2))
        ctk.CTkLabel(tg_row, text="Telegram:", text_color=COLORS["dim"],
                     font=("Consolas", 12)).pack(side="left")
        self.tg_token_entry = ctk.CTkEntry(tg_row, width=280, placeholder_text="Bot token",
                                            font=("Consolas", 12))
        self.tg_token_entry.pack(side="left", padx=2)
        ctk.CTkLabel(tg_row, text="Chat ID:", text_color=COLORS["dim"],
                     font=("Consolas", 12)).pack(side="left", padx=(5, 0))
        self.tg_chatid_entry = ctk.CTkEntry(tg_row, width=100, placeholder_text="123456789",
                                              font=("Consolas", 12))
        self.tg_chatid_entry.pack(side="left", padx=2)
        self.tg_connect_btn = ctk.CTkButton(tg_row, text="Start Bot", width=80,
                                              font=("Consolas", 12), fg_color=COLORS["accent"],
                                              text_color="#000", command=self._toggle_telegram)
        self.tg_connect_btn.pack(side="left", padx=5)
        self.tg_status_label = ctk.CTkLabel(tg_row, text="", font=("Consolas", 11))
        self.tg_status_label.pack(side="left", padx=5)
        self.telegram_bridge = None

        # Restore Telegram settings
        if cfg.get("tg_token"):
            self.tg_token_entry.insert(0, cfg["tg_token"])
        if cfg.get("tg_chatid"):
            self.tg_chatid_entry.insert(0, cfg["tg_chatid"])

        # Meshtastic bridge config row
        mesh_row = ctk.CTkFrame(self.root, fg_color="transparent")
        mesh_row.pack(fill="x", padx=10, pady=(0, 2))
        ctk.CTkLabel(mesh_row, text="Meshtastic:", text_color=COLORS["dim"],
                     font=("Consolas", 12)).pack(side="left")
        self.mesh_port_combo = ctk.CTkComboBox(mesh_row, width=200, values=["(none)"],
                                                 font=("Consolas", 12))
        self.mesh_port_combo.pack(side="left", padx=2)
        ctk.CTkButton(mesh_row, text="Detect", width=55, font=("Consolas", 12),
                       fg_color=COLORS["panel"], text_color=COLORS["text"],
                       command=self._refresh_mesh_ports).pack(side="left", padx=2)
        self.mesh_bridge_btn = ctk.CTkButton(mesh_row, text="Start Bridge", width=90,
                                               font=("Consolas", 12), fg_color=COLORS["accent"],
                                               text_color="#000", command=self._toggle_mesh_bridge)
        self.mesh_bridge_btn.pack(side="left", padx=5)
        self.mesh_status_label = ctk.CTkLabel(mesh_row, text="", font=("Consolas", 11))
        self.mesh_status_label.pack(side="left", padx=5)
        self.mesh_bridge = None
        self._mesh_port_map = {}

        # Restore Meshtastic port and populate dropdown
        self._refresh_mesh_ports()
        if cfg.get("mesh_port"):
            matched = [v for v in self._mesh_port_map if cfg["mesh_port"] in v]
            if matched:
                self.mesh_port_combo.set(matched[0])

        # Middle: Topology + Node Cards
        mid_frame = ctk.CTkFrame(self.root, fg_color="transparent")
        mid_frame.pack(fill="both", padx=10, pady=5, expand=True)
        mid_frame.grid_columnconfigure(0, weight=3)
        mid_frame.grid_columnconfigure(1, weight=2)
        mid_frame.grid_rowconfigure(0, weight=1)

        # Left: Topology canvas + stats
        left_frame = ctk.CTkFrame(mid_frame, fg_color="transparent")
        left_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 5))
        left_frame.grid_rowconfigure(0, weight=1)

        topo_frame = ctk.CTkFrame(left_frame, fg_color=COLORS["panel"], corner_radius=8)
        topo_frame.pack(fill="both", expand=True)
        ctk.CTkLabel(topo_frame, text="  Mesh Topology", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(5, 0))
        self.topo_canvas = ctk.CTkCanvas(topo_frame, bg=COLORS["bg"], highlightthickness=0, height=180)
        self.topo_canvas.pack(fill="both", expand=True, padx=10, pady=(0, 5))

        # Stats row
        stats_frame = ctk.CTkFrame(left_frame, fg_color=COLORS["panel"], corner_radius=8, height=80)
        stats_frame.pack(fill="x", pady=(5, 0))
        stats_frame.pack_propagate(False)
        stats_inner = ctk.CTkFrame(stats_frame, fg_color="transparent")
        stats_inner.pack(expand=True)

        self.stat_labels = {}
        for label, key, color in [("Radio RX", "radio_rx", COLORS["accent"]),
                                    ("Radio TX", "radio_tx", COLORS["good"]),
                                    ("Peer RX", "peer_rx", COLORS["warn"]),
                                    ("Peer TX", "peer_tx", COLORS["cursor"])]:
            f = ctk.CTkFrame(stats_inner, fg_color="transparent")
            f.pack(side="left", padx=15)
            val = ctk.CTkLabel(f, text="0", font=("Consolas", 18, "bold"), text_color=color)
            val.pack()
            ctk.CTkLabel(f, text=label, font=("Consolas", 11), text_color=COLORS["dim"]).pack()
            self.stat_labels[key] = val

        # Hub status
        f = ctk.CTkFrame(stats_inner, fg_color="transparent")
        f.pack(side="left", padx=15)
        self.hub_status_dot = ctk.CTkLabel(f, text="●", font=("Consolas", 18), text_color=COLORS["bad"])
        self.hub_status_dot.pack()
        ctk.CTkLabel(f, text="Hub", font=("Consolas", 11), text_color=COLORS["dim"]).pack()

        # Right: Node cards
        right_frame = ctk.CTkFrame(mid_frame, fg_color=COLORS["panel"], corner_radius=8)
        right_frame.grid(row=0, column=1, sticky="nsew")
        ctk.CTkLabel(right_frame, text="  Discovered Nodes", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(5, 0))
        self.nodes_frame = ctk.CTkScrollableFrame(right_frame, fg_color=COLORS["bg"], corner_radius=0)
        self.nodes_frame.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        # ─── Tabbed Content Area ─────────────────────────────
        self.tabview = ctk.CTkTabview(self.root, fg_color=COLORS["bg"],
                                       segmented_button_fg_color=COLORS["panel"],
                                       segmented_button_selected_color=COLORS["accent"],
                                       segmented_button_selected_hover_color=COLORS["accent"],
                                       segmented_button_unselected_color=COLORS["faint"])
        self.tabview.pack(fill="both", expand=True, padx=10, pady=5)

        # Tab 1: Packets (waterfall + inspector)
        packets_tab = self.tabview.add("Packets")

        # Tab 2: Sensors (full dashboard)
        sensors_tab = self.tabview.add("Sensors")
        self.sensor_dashboard = SensorDashboard(sensors_tab, self.sensor_data,
                                                  max_history=self.max_sensor_history)
        self.sensor_dashboard.pack(fill="both", expand=True)

        # Tab 3: Logic Builder
        logic_tab = self.tabview.add("Logic")
        self.logic_frame = ctk.CTkFrame(logic_tab, fg_color=COLORS["panel"], corner_radius=8)
        self.logic_frame.pack(fill="both", expand=True)

        # Logic builder toolbar
        lb_toolbar = ctk.CTkFrame(self.logic_frame, fg_color="transparent")
        lb_toolbar.pack(fill="x", padx=10, pady=(5, 0))

        ctk.CTkLabel(lb_toolbar, text="Rules:", text_color=COLORS["dim"],
                      font=("Consolas", 10)).pack(side="left", padx=(0, 4))
        self.rule_combo = ctk.CTkComboBox(lb_toolbar, width=180, values=["(none)"],
                                            font=("Consolas", 10),
                                            command=self._on_rule_select)
        self.rule_combo.pack(side="left", padx=2)
        # Allow typing to rename — save on Enter
        self.rule_combo.bind("<Return>", lambda _: self._rename_current_rule())
        ctk.CTkButton(lb_toolbar, text="+ New", width=60, font=("Consolas", 10),
                        fg_color=COLORS["accent"], text_color="#000",
                        command=self._new_rule).pack(side="left", padx=3)
        ctk.CTkButton(lb_toolbar, text="Delete", width=55, font=("Consolas", 10),
                        fg_color=COLORS["bad"], text_color="#FFF",
                        command=self._delete_rule).pack(side="left", padx=3)

        ctk.CTkLabel(lb_toolbar, text="│", text_color=COLORS["faint"]).pack(side="left", padx=5)

        self.rule_status_dot = ctk.CTkLabel(lb_toolbar, text="●", font=("Consolas", 12),
                                              text_color=COLORS["faint"])
        self.rule_status_dot.pack(side="left", padx=2)

        ctk.CTkLabel(lb_toolbar, text="│", text_color=COLORS["faint"]).pack(side="left", padx=5)

        ctk.CTkLabel(lb_toolbar, text="Check every:", text_color=COLORS["text"],
                      font=("Consolas", 11)).pack(side="left", padx=2)
        self.eval_combo = ctk.CTkComboBox(lb_toolbar, width=70, font=("Consolas", 11),
                                            values=["1s", "2s", "5s", "10s", "30s", "60s"],
                                            command=self._on_eval_change)
        self.eval_combo.set("5s")
        self.eval_combo.pack(side="left", padx=2)

        ctk.CTkLabel(lb_toolbar, text="Min action gap:", text_color=COLORS["text"],
                      font=("Consolas", 11)).pack(side="left", padx=(8, 2))
        self.gap_combo = ctk.CTkComboBox(lb_toolbar, width=70, font=("Consolas", 11),
                                          values=["5s", "10s", "30s", "60s", "300s"],
                                          command=self._on_gap_change)
        self.gap_combo.set("10s")
        self.gap_combo.pack(side="left", padx=2)

        self.rule_enabled_var = ctk.BooleanVar(value=True)
        self.rule_enabled_cb = ctk.CTkCheckBox(lb_toolbar, text="Enabled", font=("Consolas", 11),
                                                 variable=self.rule_enabled_var,
                                                 command=self._on_enabled_toggle,
                                                 text_color=COLORS["text"])
        self.rule_enabled_cb.pack(side="left", padx=10)

        ctk.CTkButton(lb_toolbar, text="Undeploy", width=65, font=("Consolas", 10),
                        fg_color="#555", text_color="#FFF",
                        command=self._undeploy_rule).pack(side="right", padx=3)
        ctk.CTkButton(lb_toolbar, text="Deploy", width=55, font=("Consolas", 10),
                        fg_color=COLORS["warn"], text_color="#000",
                        command=self._deploy_rule).pack(side="right", padx=3)
        self.deploy_status_label = ctk.CTkLabel(lb_toolbar, text="", font=("Consolas", 11))
        self.deploy_status_label.pack(side="right", padx=5)

        # Block adder toolbar
        lb_blocks = ctk.CTkFrame(self.logic_frame, fg_color="transparent")
        lb_blocks.pack(fill="x", padx=10, pady=2)

        self._block_combos: list[tuple[ctk.CTkComboBox, str]] = []
        for cat_name, block_types in CATEGORY_BLOCKS.items():
            menu_values = [BLOCK_DEFS[bt]["label"] for bt in block_types]
            combo = ctk.CTkComboBox(lb_blocks, width=130, font=("Consolas", 11),
                                     values=menu_values,
                                     command=lambda v, bts=block_types, mvs=menu_values, cn=cat_name:
                                         self._add_block_from_menu(v, bts, mvs, cn))
            combo.set(f"+ {cat_name}")
            combo.pack(side="left", padx=3)
            self._block_combos.append((combo, cat_name))

        # Canvas area
        canvas_frame = ctk.CTkFrame(self.logic_frame, fg_color=COLORS["bg"], corner_radius=4)
        canvas_frame.pack(fill="both", expand=True, padx=10, pady=5)

        self.auto_canvas = AutomationCanvas(canvas_frame, self.engine, self._on_canvas_change)

        # Event log
        self.logic_log = ctk.CTkLabel(self.logic_frame, text="Event log: (none)",
                                        font=("Consolas", 13), text_color="#B0B0B0",
                                        anchor="w")
        self.logic_log.pack(fill="x", padx=10, pady=(0, 5))

        # Packet waterfall (inside Packets tab)
        self.wf_frame = ctk.CTkFrame(packets_tab, fg_color=COLORS["panel"], corner_radius=8)
        self.wf_frame.pack(fill="both", expand=True, padx=5, pady=5)
        wf_frame = self.wf_frame
        ctk.CTkLabel(wf_frame, text="  Packet Waterfall (click to inspect)", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(5, 0))
        self.wf_text = ctk.CTkTextbox(wf_frame, font=("Consolas", 11), fg_color=COLORS["bg"],
                                       text_color=COLORS["dim"], height=100, state="disabled")
        self.wf_text.pack(fill="both", expand=True, padx=10, pady=(0, 5))
        self.wf_text.bind("<Button-1>", self.on_waterfall_click)

        # Packet inspector (inside Packets tab)
        self.insp_frame = ctk.CTkFrame(packets_tab, fg_color=COLORS["panel"], corner_radius=8, height=95)
        self.insp_frame.pack(fill="x", padx=5, pady=(0, 5))
        self.insp_frame.pack_propagate(False)
        insp_frame = self.insp_frame
        ctk.CTkLabel(insp_frame, text="  Packet Inspector", font=("Consolas", 11, "bold"),
                      text_color=COLORS["accent"], anchor="w").pack(fill="x", padx=10, pady=(5, 0))

        insp_inner = ctk.CTkFrame(insp_frame, fg_color="transparent")
        insp_inner.pack(fill="x", padx=10)
        self.insp_label = ctk.CTkLabel(insp_inner, text="Click a packet above to inspect",
                                         font=("Consolas", 11), text_color=COLORS["dim"], anchor="w")
        self.insp_label.pack(fill="x")

        decrypt_row = ctk.CTkFrame(insp_frame, fg_color="transparent")
        decrypt_row.pack(fill="x", padx=10, pady=(2, 5))
        ctk.CTkLabel(decrypt_row, text="AES Key:", text_color=COLORS["dim"], font=("Consolas", 11)).pack(side="left")
        self.aes_entry = ctk.CTkEntry(decrypt_row, width=180, placeholder_text="16-char key",
                                       font=("Consolas", 11))
        self.aes_entry.pack(side="left", padx=5)
        self.decrypt_btn = ctk.CTkButton(decrypt_row, text="Decrypt", width=70, font=("Consolas", 11),
                                           fg_color=COLORS["accent"], text_color="#000", command=self.decrypt_selected)
        self.decrypt_btn.pack(side="left", padx=5)
        self.decrypt_result = ctk.CTkLabel(decrypt_row, text="", font=("Consolas", 11),
                                            text_color=COLORS["good"])
        self.decrypt_result.pack(side="left", padx=10)

    # ─── Serial Port Detection ──────────────────────────────

    def refresh_ports(self):
        ports = detect_serial_ports()
        if ports:
            values = [desc for _, desc in ports]
            self.port_combo.configure(values=values)
            self._port_map = {desc: dev for dev, desc in ports}
            # Restore saved port if available
            saved = self._saved_config.get("serial_port", "")
            matched = [v for v in values if saved and saved in v]
            self.port_combo.set(matched[0] if matched else values[0])
        else:
            self.port_combo.configure(values=["No ports found"])
            self.port_combo.set("No ports found")
            self._port_map = {}

    def get_selected_port(self) -> str | None:
        desc = self.port_combo.get()
        if hasattr(self, "_port_map"):
            return self._port_map.get(desc)
        return None

    # ─── Connection ─────────────────────────────────────────

    def _load_config(self) -> dict:
        try:
            with open(self.CONFIG_FILE) as f:
                return json.load(f)
        except Exception:
            return {}

    def _save_config(self, port: str = None, hub: str = None, name: str = None,
                     key: str = None, lat: str = "", lon: str = "", ant_height: str = ""):
        try:
            # Read current widget values as defaults if args not provided
            cfg = {
                "serial_port": port or self.port_combo.get(),
                "hub_url": hub if hub is not None else self.hub_entry.get().strip(),
                "gw_name": name or self.name_entry.get().strip(),
                "hub_key": key if key is not None else self.key_entry.get().strip(),
                "lat": lat or self.lat_entry.get().strip(),
                "lon": lon or self.lon_entry.get().strip(),
                "ant_height": ant_height or self.ant_height_entry.get().strip(),
                "tg_token": self.tg_token_entry.get().strip(),
                "tg_chatid": self.tg_chatid_entry.get().strip(),
                "node_names": getattr(self, 'node_names', {}),
                "mesh_port": getattr(self, '_saved_config', {}).get("mesh_port", ""),
            }
            with open(self.CONFIG_FILE, "w") as f:
                json.dump(cfg, f, indent=2)
        except Exception:
            pass

    def connect(self):
        port = self.get_selected_port()
        if not port:
            return
        hub = self.hub_entry.get().strip() or None
        key = self.key_entry.get().strip() or None
        name = self.name_entry.get().strip() or "Gateway"
        lat_str = self.lat_entry.get().strip()
        lon_str = self.lon_entry.get().strip()
        ant_h_str = self.ant_height_entry.get().strip()
        # Save settings for next launch
        self._save_config(port, hub or "", name, key or "",
                          lat_str, lon_str, ant_h_str)

        self.gateway = GUIGatewayServer(port, 115200, hub, key, name, self.event_queue)
        # Pass location info for hub auth
        try:
            self.gateway.lat = float(lat_str) if lat_str else 0.0
            self.gateway.lon = float(lon_str) if lon_str else 0.0
            self.gateway.antenna_height = float(ant_h_str) if ant_h_str else 2.0
        except ValueError:
            pass
        self.gateway.antenna_type = 0  # Default: omnidirectional
        self.connected = True

        def run_async():
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            loop.run_until_complete(self.gateway.run())

        self.async_thread = threading.Thread(target=run_async, daemon=True)
        self.async_thread.start()

        self.conn_label.configure(text=f"Connected — {port}", text_color=COLORS["good"])
        self.connect_btn.configure(state="disabled")
        self.disconnect_btn.configure(state="normal")

    def disconnect(self):
        if self.gateway:
            self.gateway.shutdown()
        self.connected = False
        self.conn_label.configure(text="Disconnected", text_color=COLORS["bad"])
        self.connect_btn.configure(state="normal")
        self.disconnect_btn.configure(state="disabled")
        self.hub_status_dot.configure(text_color=COLORS["bad"])

    # ─── Event Processing ───────────────────────────────────

    def poll_events(self):
        try:
            for _ in range(50):  # batch up to 50 events per tick
                event = self.event_queue.get_nowait()
                self.handle_event(event)
        except queue.Empty:
            pass
        self.root.after(50, self.poll_events)

    def handle_event(self, event):
        etype = event.get("type", "")

        if etype in ("packet_from_radio", "packet_from_hub"):
            parsed = event.get("parsed")
            direction = "RADIO" if etype == "packet_from_radio" else "HUB"
            self.add_packet_to_waterfall(parsed, direction)
            if parsed:
                self.update_topology_from_packet(parsed)
                self.packet_history.append(parsed)
                if len(self.packet_history) > 500:
                    self.packet_history.pop(0)

                # Auto-decrypt and extract SDATA from MSG packets
                if parsed.get("type") == "MSG":
                    aes_key = self.aes_entry.get().strip()
                    encrypted = parsed.get("encrypted", "")
                    msg_id = parsed.get("msg_id", "")
                    if len(aes_key) == 16 and encrypted:
                        plain = decrypt_message(encrypted, msg_id, aes_key)
                        if plain:
                            if plain.startswith("SDATA,"):
                                self._parse_sdata(plain)
                            elif plain.startswith("PINS,"):
                                self._parse_pins(plain)
                            elif plain.startswith("CMD,RSP,"):
                                pass  # Relay state updates (future)

        elif etype == "stats":
            for key in ("radio_rx", "radio_tx", "peer_rx", "peer_tx"):
                if key in event and key in self.stat_labels:
                    self.stat_labels[key].configure(text=str(event[key]))

        elif etype == "hub_connected":
            self.hub_status_dot.configure(text_color=COLORS["good"])

        elif etype == "hub_disconnected":
            self.hub_status_dot.configure(text_color=COLORS["bad"])

        elif etype == "serial_line":
            text = event.get("text", "")
            # Extract node ID from STATUS response or standalone ID line
            # Serial format: "ID:       6CB5" (with spaces)
            # BLE format: "STATUS,ID:6CB5,BOARD:..." (no spaces)
            if text.startswith("STATUS,") or text.startswith("ID:") or text.startswith("Board:"):
                node_id = ""
                board_name = ""
                if text.startswith("STATUS,"):
                    # BLE format: STATUS,ID:6CB5,BOARD:Heltec V3,...
                    for field in text.split(","):
                        if field.startswith("ID:"):
                            node_id = field[3:].strip()
                        elif field.startswith("BOARD:"):
                            board_name = field[6:].strip()
                elif text.startswith("ID:"):
                    # Serial format: "ID:       6CB5"
                    node_id = text.split()[-1].strip()
                elif text.startswith("Board:"):
                    # Serial format: "Board:    Heltec V3"
                    board_name = text.split(":", 1)[1].strip()
                if node_id and len(node_id) == 4:
                    self.local_id = node_id
                    self.engine.local_node_id = self.local_id
                    if self.local_id not in self.topo_nodes:
                        self.topo_nodes[self.local_id] = TopoNode(self.local_id, is_local=True)
                if board_name:
                    self._set_default_pins_for_board(board_name)
            # Parse pin config: PINS,R:2,3,4|S:33,34
            elif text.startswith("PINS,"):
                self._parse_pins(text)
            # Parse beacon events
            elif text.startswith("BEACON,TRIGGERED,"):
                self._parse_beacon_event(text, True)
            elif text.startswith("BEACON,REVERTED,"):
                self._parse_beacon_event(text, False)
            # Parse direct SDATA from serial (local POLL response)
            elif text.startswith("SDATA,"):
                self._parse_sdata(text)
                self.engine._log("Engine", f"SDATA received: {text[:50]}")
            # Parse remote node responses that arrive wrapped in RX,<from>,<content>,<rssi>
            elif text.startswith("RX,"):
                # Extract content between from and trailing RSSI
                parts = text.split(",", 2)
                if len(parts) >= 3:
                    from_node = parts[1]
                    content = parts[2]
                    # Strip trailing RSSI (last field, negative int)
                    last_comma = content.rfind(",")
                    if last_comma > 0:
                        maybe_rssi = content[last_comma + 1:]
                        if maybe_rssi.lstrip("-").isdigit():
                            content = content[:last_comma]
                    if content.startswith("SDATA,"):
                        self._parse_sdata(content)
                        self.engine._log("Engine", f"RX SDATA from {from_node}: {content[:50]}")
                    elif content.startswith("PINS,"):
                        self._parse_pins(content, from_node)
            # Log unhandled serial lines (full text for scan results, truncated for others)
            elif text.startswith("BEACONSCAN,"):
                self.engine._log("Serial", text)  # Full scan result for beacon dialog
            else:
                self.engine._log("Serial", text[:60])

        elif etype == "log":
            pass  # Could add a log panel later

    # ─── Topology ───────────────────────────────────────────

    def update_topology_from_packet(self, parsed: dict):
        if not parsed:
            return
        now = time.time()
        ptype = parsed.get("type", "")
        src = parsed.get("src", "")

        pkt_rssi = parsed.get("rssi", -100)

        if ptype == "HB":
            hb_from = parsed.get("hb_from", src)
            if hb_from and hb_from != "FFFF":
                if hb_from not in self.topo_nodes:
                    self.topo_nodes[hb_from] = TopoNode(hb_from, hb_from == self.local_id)
                    # Give new node the default pins (same board type likely)
                    if hb_from not in self.node_pin_configs and self.node_relay_pins:
                        self.node_pin_configs[hb_from] = (self.node_relay_pins, self.node_sensor_pins)
                        self.auto_canvas.node_pin_configs = self.node_pin_configs
                    # Also query actual pin config via mesh (may override defaults)
                    if hb_from != self.local_id and hb_from not in self._queried_nodes:
                        self._queried_nodes.add(hb_from)
                        self._send_serial(f"MSG,{hb_from},CMD,LIST")
                node = self.topo_nodes[hb_from]
                node.last_seen = now
                node.packets += 1
                node.rssi = pkt_rssi
                node.hops = 1
                node.next_hop = hb_from
                # Direct link edge: us ↔ them
                if self.local_id:
                    self.topo_edges[(self.local_id, hb_from)] = now
                    self.topo_edge_rssi[(self.local_id, hb_from)] = pkt_rssi

        elif ptype == "MSG":
            msg_from = parsed.get("msg_from", "")
            route = parsed.get("route", "")
            if msg_from and msg_from != "FFFF":
                if msg_from not in self.topo_nodes:
                    self.topo_nodes[msg_from] = TopoNode(msg_from, msg_from == self.local_id)
                self.topo_nodes[msg_from].last_seen = now
                self.topo_nodes[msg_from].packets += 1
                # Only update RSSI from direct messages (src == msg_from means no relay)
                if src == msg_from:
                    self.topo_nodes[msg_from].rssi = pkt_rssi

            # Build edges from route
            if route:
                hops = [h.strip() for h in route.split(",") if h.strip()]
                for i in range(len(hops) - 1):
                    a, b = hops[i], hops[i + 1]
                    if a and b:
                        self.topo_edges[(a, b)] = now
                        # Last hop has the actual RSSI; others are estimated
                        if i == len(hops) - 2:
                            self.topo_edge_rssi[(a, b)] = pkt_rssi
                        for nid in (a, b):
                            if nid not in self.topo_nodes:
                                self.topo_nodes[nid] = TopoNode(nid, nid == self.local_id)
                            self.topo_nodes[nid].last_seen = now
                if len(hops) >= 1:
                    node = self.topo_nodes.get(msg_from)
                    if node:
                        node.hops = len(hops)
                        if len(hops) >= 2:
                            node.next_hop = hops[-1]

        self.update_node_cards()

    def update_node_cards(self):
        # Build a snapshot to detect changes — only rebuild if something changed
        snapshot = tuple((n.id, n.rssi, n.packets, n.is_local or n.last_seen > time.time() - 120,
                         int(n.last_seen) // 30)  # Update every 30s for age display
                         for n in sorted(self.topo_nodes.values(),
                                         key=lambda n: n.last_seen, reverse=True)[:15])
        if hasattr(self, '_node_snapshot') and self._node_snapshot == snapshot:
            return  # Nothing changed, skip rebuild
        self._node_snapshot = snapshot

        for widget in self.nodes_frame.winfo_children():
            widget.destroy()

        now = time.time()
        sorted_nodes = sorted(self.topo_nodes.values(), key=lambda n: n.last_seen, reverse=True)

        for node in sorted_nodes[:15]:  # Show top 15
            # Local node is always online (connected via USB)
            if node.is_local:
                online = True
                node.last_seen = now  # Keep it fresh
            else:
                age = now - node.last_seen if node.last_seen else 9999
                online = age < 120

            card = ctk.CTkFrame(self.nodes_frame, fg_color=COLORS["panel"], corner_radius=6)
            card.pack(fill="x", pady=2)

            row1 = ctk.CTkFrame(card, fg_color="transparent")
            row1.pack(fill="x", padx=8, pady=(5, 0))
            dot_color = COLORS["good"] if online else COLORS["faint"]
            ctk.CTkLabel(row1, text=f"● {node.id}", font=("Consolas", 13, "bold"),
                          text_color=dot_color).pack(side="left")
            if node.is_local:
                ctk.CTkLabel(row1, text="(LOCAL)", font=("Consolas", 10),
                              text_color=COLORS["accent"]).pack(side="left", padx=5)
            if node.is_local:
                status = "Online"
            else:
                age = now - node.last_seen if node.last_seen else 9999
                status = "Online" if online else f"Last: {format_uptime(age)} ago"
            ctk.CTkLabel(row1, text=status, font=("Consolas", 11),
                          text_color=COLORS["dim"]).pack(side="right")

            row2 = ctk.CTkFrame(card, fg_color="transparent")
            row2.pack(fill="x", padx=8, pady=(0, 5))

            if node.is_local:
                # Local node: show USB connection, not fake RSSI
                ctk.CTkLabel(row2, text="USB Serial", font=("Consolas", 12),
                              text_color=COLORS["accent"], width=90, anchor="w").pack(side="left", padx=(0, 8))
                ctk.CTkLabel(row2, text=f"Pkts:{node.packets}", font=("Consolas", 12),
                              text_color=COLORS["dim"], width=90, anchor="w").pack(side="left", padx=(0, 8))
                ctk.CTkLabel(row2, text="Direct", font=("Consolas", 12),
                              text_color=COLORS["dim"]).pack(side="left")
            else:
                # Remote node: show RSSI, signal bars, route
                rssi_color = COLORS["good"] if node.rssi > -80 else (COLORS["warn"] if node.rssi > -100 else COLORS["bad"])
                if not online:
                    rssi_color = COLORS["faint"]
                bars = 0 if not online else (4 if node.rssi > -60 else (3 if node.rssi > -75 else (2 if node.rssi > -90 else (1 if node.rssi > -105 else 0))))
                bar_frame = ctk.CTkFrame(row2, fg_color="transparent", width=44, height=18)
                bar_frame.pack(side="left", padx=(0, 8))
                bar_frame.pack_propagate(False)
                for b in range(4):
                    h = 5 + b * 4
                    c = rssi_color if b < bars else COLORS["faint"]
                    bar = ctk.CTkFrame(bar_frame, width=7, height=h, fg_color=c, corner_radius=1)
                    bar.place(x=b * 10, y=18 - h)
                ctk.CTkLabel(row2, text=f"{node.rssi} dBm", font=("Consolas", 12),
                              text_color=rssi_color, width=90, anchor="w").pack(side="left", padx=(0, 8))
                ctk.CTkLabel(row2, text=f"Pkts:{node.packets}", font=("Consolas", 12),
                              text_color=COLORS["dim"], width=90, anchor="w").pack(side="left", padx=(0, 8))
                if node.hops and node.hops > 1 and node.next_hop:
                    ctk.CTkLabel(row2, text=f"{node.hops}hop via {node.next_hop}", font=("Consolas", 12),
                                  text_color=COLORS["dim"]).pack(side="left")
                elif node.hops:
                    ctk.CTkLabel(row2, text="Direct", font=("Consolas", 12),
                                  text_color=COLORS["dim"]).pack(side="left")

            # Right-click context menu on node card
            node_ref = node  # Capture for lambda
            def _node_context(event, nid=node_ref.id, is_local=node_ref.is_local):
                menu = tkinter.Menu(self.root, tearoff=0, bg="#222", fg="#EEE",
                                    activebackground="#444", font=("Consolas", 10))
                menu.add_command(label=f"Query Rules on {nid}",
                                 command=lambda: self._query_node_rules(nid, is_local))
                menu.add_command(label=f"Set Name for {nid}",
                                 command=lambda: self._set_node_name(nid))
                menu.add_separator()
                menu.add_command(label=f"Clear Rules on {nid}",
                                 command=lambda: self._clear_node_rules(nid, is_local))
                menu.tk_popup(event.x_root, event.y_root)
            card.bind("<Button-3>", _node_context)
            for child in card.winfo_children():
                child.bind("<Button-3>", _node_context)
                for subchild in child.winfo_children():
                    subchild.bind("<Button-3>", _node_context)

    def _query_node_rules(self, node_id: str, is_local: bool):
        """Send BEACON,LIST and SETPOINT,LIST to query deployed rules on a node."""
        if is_local:
            self._send_serial("BEACON,LIST")
            self._send_serial("SETPOINT,LIST")
        else:
            self._send_serial(f"MSG,{node_id},CMD,BEACON,LIST")
            self._send_serial(f"MSG,{node_id},CMD,SETPOINT,LIST")
        # Show a popup that will be populated when responses arrive
        popup = ctk.CTkToplevel(self.root)
        popup.title(f"Rules on {node_id}")
        popup.geometry("400x300")
        popup.attributes("-topmost", True)
        result_text = ctk.CTkTextbox(popup, font=("Consolas", 11),
                                      fg_color=COLORS["bg"], text_color=COLORS["text"])
        result_text.pack(fill="both", expand=True, padx=10, pady=10)
        result_text.insert("end", f"Querying {node_id}...\n\nWaiting for response...\n")

        # Poll engine event log for responses
        def check_response(attempts=0):
            if attempts > 10:
                result_text.insert("end", "\nNo response — node may be offline or have no rules.")
                return
            found = False
            for _ts, name, msg in reversed(self.engine.event_log[-30:]):
                if "BEACONS" in msg or "SETPOINTS" in msg or "OK,BEACON" in msg or "OK,SETPOINT" in msg:
                    result_text.delete("1.0", "end")
                    result_text.insert("end", f"Rules on {node_id}:\n\n{msg}\n")
                    found = True
                    break
            if not found:
                popup.after(1000, lambda: check_response(attempts + 1))
        popup.after(2000, check_response)

    def _set_node_name(self, node_id: str):
        """Set a friendly name for a node."""
        if not hasattr(self, 'node_names'):
            self.node_names = {}
        dialog = ctk.CTkInputDialog(text=f"Friendly name for {node_id}:",
                                     title="Set Node Name")
        name = dialog.get_input()
        if name:
            self.node_names[node_id] = name
            self._save_config()

    def _clear_node_rules(self, node_id: str, is_local: bool):
        """Clear all deployed rules on a node."""
        if is_local:
            self._send_serial("BEACON,CLEAR")
            self._send_serial("SETPOINT,CLEAR")
        else:
            self._send_serial(f"MSG,{node_id},CMD,BEACON,CLEAR")
            self._send_serial(f"MSG,{node_id},CMD,SETPOINT,CLEAR")
        self.engine._log("Deploy", f"Cleared all rules on {node_id}")

    # ─── Topology Canvas Animation ──────────────────────────

    @staticmethod
    def _rssi_edge_color(rssi: int) -> str:
        """Map RSSI to green (strong) → orange (fair) → red (weak)."""
        if rssi > -70:
            return "#00E676"   # green — strong
        if rssi > -85:
            return "#00E5FF"   # cyan — good
        if rssi > -100:
            return "#FFA500"   # orange — fair
        return "#FF5252"       # red — weak

    def animate_topology(self):
        canvas = self.topo_canvas
        canvas.delete("all")
        w = canvas.winfo_width() or 400
        h = canvas.winfo_height() or 200
        cx, cy = w / 2, h / 2
        now = time.time()

        nodes = list(self.topo_nodes.values())
        n = len(nodes)

        if n == 0:
            canvas.create_text(cx, cy, text="Waiting for mesh traffic...",
                                fill=COLORS["dim"], font=("Consolas", 11))
            self.root.after(33, self.animate_topology)
            return

        # Signal quality summary bar at top
        online_nodes = [nd for nd in nodes if (now - nd.last_seen < 120) and nd.rssi < 0]
        if online_nodes:
            rssi_vals = [nd.rssi for nd in online_nodes]
            best, worst, avg = max(rssi_vals), min(rssi_vals), sum(rssi_vals) // len(rssi_vals)
            summary = f"{len(online_nodes)} nodes | Best: {best}dBm | Worst: {worst}dBm | Avg: {avg}dBm"
            canvas.create_text(w // 2, 10, text=summary, fill="#B0B0B0", font=("Consolas", 11))
            # Weak link warning
            weak = [nd for nd in online_nodes if nd.rssi < -100]
            if weak:
                worst_node = min(weak, key=lambda nd: nd.rssi)
                via = f" via {worst_node.next_hop}" if worst_node.next_hop and worst_node.next_hop != worst_node.id else ""
                warn = f"Weak: {worst_node.id} ({worst_node.rssi}dBm{via})"
                canvas.create_text(w // 2, 22, text=warn, fill=COLORS["warn"], font=("Consolas", 11))

        # Circular layout with padding for summary
        top_pad = 35
        radius = min(w, h - top_pad) * 0.33
        for i, node in enumerate(nodes):
            angle = i * (2 * math.pi / n) - math.pi / 2
            node.x = cx + radius * math.cos(angle)
            node.y = (cy + top_pad // 2) + radius * math.sin(angle)

        # Draw edges — colour and thickness by RSSI
        for (a, b), last in self.topo_edges.items():
            na = self.topo_nodes.get(a)
            nb = self.topo_nodes.get(b)
            if na and nb:
                age = now - last
                if age > 300:
                    continue  # Prune old edges
                rssi = self.topo_edge_rssi.get((a, b), -100)
                color = self._rssi_edge_color(rssi)
                # Fade old edges
                if age > 120:
                    color = COLORS["faint"]
                # Thickness: strong=3, weak=1
                thickness = max(1, min(3, int((rssi + 110) / 15)))
                canvas.create_line(na.x, na.y, nb.x, nb.y, fill=color, width=thickness)
                # RSSI label on edge midpoint
                if rssi > -120:
                    mx = (na.x + nb.x) / 2
                    my = (na.y + nb.y) / 2
                    canvas.create_text(mx, my - 6, text=f"{rssi}", fill=color,
                                        font=("Consolas", 11))

        # Draw nodes — size by hop count
        for node in nodes:
            age = now - node.last_seen if node.last_seen else 9999
            # Size: local=16, direct=12, multi-hop=9
            if node.is_local:
                r = 16
            elif node.hops <= 1:
                r = 12
            else:
                r = 9

            if age < 120:
                outline = COLORS["good"] if age < 30 else COLORS["warn"]
            else:
                outline = COLORS["faint"]

            fill = COLORS["header"] if not node.is_local else COLORS["bubble_in"]
            canvas.create_oval(node.x - r, node.y - r, node.x + r, node.y + r,
                                fill=fill, outline=outline, width=2)
            canvas.create_text(node.x, node.y, text=node.id, fill=COLORS["text"],
                                font=("Consolas", 8 if node.is_local else 7, "bold"))
            # RSSI below node
            if age < 120 and node.rssi < 0:
                canvas.create_text(node.x, node.y + r + 8, text=f"{node.rssi}dBm",
                                    fill=self._rssi_edge_color(node.rssi),
                                    font=("Consolas", 11))

        self.root.after(33, self.animate_topology)

    # ─── Packet Waterfall ───────────────────────────────────

    def add_packet_to_waterfall(self, parsed: dict | None, direction: str):
        if not parsed:
            return
        ts = time.strftime("%H:%M:%S")
        ptype = parsed.get("type", "?")
        src = parsed.get("src", "????")
        dest = parsed.get("dest", "????")
        ttl = parsed.get("ttl", "?")
        size = parsed.get("size", 0)
        route = parsed.get("route", "")

        arrow = "◄" if direction == "RADIO" else "►"
        line = f"{ts} {arrow}{direction:<5} {src}→{dest}  TTL:{ttl}  {size}B  {ptype}"
        if route and ptype == "MSG":
            line += f"  Route:{route}"

        self.wf_text.configure(state="normal")
        self.wf_text.insert("end", line + "\n")
        lines = int(self.wf_text.index("end-1c").split(".")[0])
        if lines > 300:
            self.wf_text.delete("1.0", f"{lines - 300}.0")
        self.wf_text.see("end")
        self.wf_text.configure(state="disabled")

    def on_waterfall_click(self, event):
        try:
            index = self.wf_text.index(f"@{event.x},{event.y}")
            line_num = int(index.split(".")[0]) - 1
            # Waterfall text trims at 300 lines but packet_history at 500.
            # When text lines are trimmed from the top, line numbers shift
            # but packet_history still has the old entries. Calculate offset.
            visible_lines = int(self.wf_text.index("end-1c").split(".")[0]) - 1
            offset = len(self.packet_history) - visible_lines
            actual_index = line_num + max(0, offset)
            if 0 <= actual_index < len(self.packet_history):
                self.selected_packet = self.packet_history[actual_index]
                self.show_inspector(self.selected_packet)
        except Exception:
            pass

    def show_inspector(self, pkt: dict):
        src = pkt.get("src", "?")
        dest = pkt.get("dest", "?")
        seq = pkt.get("seq", "?")
        ttl = pkt.get("ttl", "?")
        ptype = pkt.get("type", "?")
        route = pkt.get("route", "")
        hex_preview = pkt.get("hex", "")

        route_display = " → ".join(route.split(",")) if route else "(direct)"
        info = f"[{ptype}]  Dest:{dest}  Src:{src}  Seq:{seq}  TTL:{ttl}  Route: {route_display}  Hex: {hex_preview}..."
        self.insp_label.configure(text=info)
        self.decrypt_result.configure(text="")

    def decrypt_selected(self):
        if not self.selected_packet:
            self.decrypt_result.configure(text="No packet selected", text_color=COLORS["bad"])
            return
        key = self.aes_entry.get().strip()
        if len(key) != 16:
            self.decrypt_result.configure(text="Key must be 16 chars", text_color=COLORS["bad"])
            return
        encrypted = self.selected_packet.get("encrypted", "")
        msg_id = self.selected_packet.get("msg_id", "")
        if not encrypted or not msg_id:
            self.decrypt_result.configure(text="Not an encrypted MSG packet", text_color=COLORS["bad"])
            return
        plain = decrypt_message(encrypted, msg_id, key)
        if plain:
            self.decrypt_result.configure(text=f'Plain: "{plain}"', text_color=COLORS["good"])
        else:
            self.decrypt_result.configure(text="Decrypt failed (wrong key or auth failed)", text_color=COLORS["bad"])

    # ─── Serial Commands ───────────────────────────────────

    def _send_serial(self, cmd: str):
        """Send a command to the connected node via serial."""
        if self.gateway and self.gateway.serial_conn:
            try:
                self.gateway.serial_conn.write(f"{cmd}\n".encode("ascii"))
            except Exception:
                pass

    # ─── Logic Builder ────────────────────────────────────

    def toggle_logic_builder(self):
        """Switch to Logic tab."""
        self.tabview.set("Logic")
        self.auto_canvas.engine = self.engine
        self._refresh_rule_list()

    def _refresh_rule_list(self):
        names = [r.name for r in self.engine.rules]
        if not names:
            names = ["(none)"]
        self.rule_combo.configure(values=names)
        if self.engine.rules:
            self.rule_combo.set(self.engine.rules[0].name)
            self._load_rule(self.engine.rules[0])
        else:
            self.rule_combo.set("(none)")
            self.auto_canvas.set_rule(None)

    def _on_rule_select(self, name: str):
        for r in self.engine.rules:
            if r.name == name:
                self._load_rule(r)
                return

    def _load_rule(self, rule):
        self.auto_canvas.set_rule(rule)
        self.eval_combo.set(f"{int(rule.eval_interval)}s")
        self.gap_combo.set(f"{int(rule.action_gap)}s")
        self.rule_enabled_var.set(rule.enabled)
        self._update_deploy_status()
        status_colors = {"active": COLORS["good"], "triggered": COLORS["bad"],
                         "disabled": COLORS["faint"], "error": COLORS["bad"],
                         "idle": COLORS["dim"]}
        self.rule_status_dot.configure(
            text_color=status_colors.get(rule.status, COLORS["dim"]))

    def _new_rule(self):
        rule = self.engine.create_rule(f"Rule {len(self.engine.rules)}")
        self._refresh_rule_list()
        self.rule_combo.set(rule.name)
        self._load_rule(rule)

    def _delete_rule(self):
        rule = self.auto_canvas.current_rule
        if rule:
            self.engine.delete_rule(rule.id)
            self._refresh_rule_list()

    def _on_eval_change(self, val: str):
        rule = self.auto_canvas.current_rule
        if rule:
            try:
                rule.eval_interval = float(val.rstrip("s"))
            except ValueError:
                pass
            self.engine.save_rules()

    def _on_gap_change(self, val: str):
        rule = self.auto_canvas.current_rule
        if rule:
            try:
                rule.action_gap = float(val.rstrip("s"))
            except ValueError:
                pass
            self.engine.save_rules()

    def _on_enabled_toggle(self):
        rule = self.auto_canvas.current_rule
        if rule:
            rule.enabled = self.rule_enabled_var.get()
            self.engine.save_rules()

    def _add_block_from_menu(self, label: str, block_types: list,
                             menu_values: list, cat_name: str):
        """Add a block when selected from a category dropdown."""
        try:
            idx = menu_values.index(label)
            bt = block_types[idx]
            self.auto_canvas.add_block(bt)
        except (ValueError, IndexError):
            pass
        # Reset dropdown so same block type can be added again
        # (CTkComboBox only fires on value CHANGE, not re-selection)
        for combo, cn in self._block_combos:
            if cn == cat_name:
                combo.set(f"+ {cat_name}")
                break

    def _on_canvas_change(self):
        """Called when the canvas modifies a rule."""
        rule = self.auto_canvas.current_rule
        if rule:
            # Update the combo in case rule was renamed
            names = [r.name for r in self.engine.rules]
            self.rule_combo.configure(values=names if names else ["(none)"])

    def _deploy_rule(self):
        rule = self.auto_canvas.current_rule
        if not rule:
            return
        ok, msg = self.engine.try_deploy_as_setpoint(rule)
        color = COLORS["good"] if ok else COLORS["bad"]
        self.logic_log.configure(text=f"Deploy: {msg}", text_color=color)
        self._deploy_msg_until = time.time() + 5
        self._update_deploy_status()

    def _undeploy_rule(self):
        rule = self.auto_canvas.current_rule
        if not rule:
            return
        ok, msg = self.engine.undeploy_rule(rule)
        color = COLORS["good"] if ok else COLORS["bad"]
        self.logic_log.configure(text=f"Undeploy: {msg}", text_color=color)
        self._deploy_msg_until = time.time() + 5
        self._update_deploy_status()

    def _rename_current_rule(self):
        rule = self.auto_canvas.current_rule
        if not rule:
            return
        new_name = self.rule_combo.get().strip()
        if not new_name or new_name == rule.name:
            return
        rule.name = new_name
        self.engine.save_rules()
        # Update combo values
        names = [r.name for r in self.engine.rules]
        self.rule_combo.configure(values=names)
        self.rule_combo.set(new_name)
        self._update_deploy_status()

    def _update_deploy_status(self):
        rule = self.auto_canvas.current_rule
        if rule and rule.deployed:
            mode = getattr(rule, 'deploy_mode', 'firmware')
            label = "DEPLOYED (node)" if mode == "firmware" else "RUNNING (GUI)"
            self.deploy_status_label.configure(text=label, text_color=COLORS["good"])
        else:
            self.deploy_status_label.configure(text="", text_color=COLORS["dim"])

    def _toggle_telegram(self):
        if self.telegram_bridge and self.telegram_bridge.is_running:
            self.telegram_bridge.stop()
            self.telegram_bridge = None
            self.tg_connect_btn.configure(text="Start Bot")
            self.tg_status_label.configure(text="Stopped", text_color=COLORS["dim"])
        else:
            token = self.tg_token_entry.get().strip()
            chat_id = self.tg_chatid_entry.get().strip()
            if not token:
                self.tg_status_label.configure(text="Enter bot token", text_color=COLORS["bad"])
                return
            try:
                from telegram_bot import TelegramBridge
                self.telegram_bridge = TelegramBridge(
                    token=token, chat_id=chat_id,
                    sensor_data=self.sensor_data,
                    send_serial=self._send_serial,
                    rules=self.engine.rules,
                    engine=self.engine,
                    node_names=getattr(self, 'node_names', {}),
                )
                self.telegram_bridge.start()
                self.engine.telegram_bridge = self.telegram_bridge
                self.tg_connect_btn.configure(text="Stop Bot")
                self.tg_status_label.configure(text="Running", text_color=COLORS["good"])
                # Save to config
                self._saved_config["tg_token"] = token
                self._saved_config["tg_chatid"] = chat_id
                self._save_config()
            except Exception as e:
                self.tg_status_label.configure(text=f"Error: {e}", text_color=COLORS["bad"])

    def _refresh_mesh_ports(self):
        ports = detect_serial_ports()
        if ports:
            values = [desc for _, desc in ports]
            self.mesh_port_combo.configure(values=values)
            self._mesh_port_map = {desc: dev for dev, desc in ports}
        else:
            self.mesh_port_combo.configure(values=["No ports found"])
            self._mesh_port_map = {}

    def _toggle_mesh_bridge(self):
        if self.mesh_bridge:
            # Stop bridge
            try:
                self.mesh_bridge.mesh.stop()
                self.mesh_bridge.tom.stop()
            except Exception:
                pass
            self.mesh_bridge = None
            self.mesh_bridge_btn.configure(text="Start Bridge")
            self.mesh_status_label.configure(text="Stopped", text_color=COLORS["dim"])
            return

        desc = self.mesh_port_combo.get()
        mesh_port = self._mesh_port_map.get(desc)
        if not mesh_port:
            self.mesh_status_label.configure(text="Select a port", text_color=COLORS["bad"])
            return

        # Get TOM serial port (the one the GUI is connected to)
        tom_port = self.get_selected_port()
        if not tom_port or not self.gateway:
            self.mesh_status_label.configure(text="Connect TOM first", text_color=COLORS["bad"])
            return

        try:
            from meshtastic_bridge import Bridge
            aes_key = self.key_entry.get().strip()
            self.mesh_bridge = Bridge(
                tom_port="__gui__",  # Marker: bridge uses GUI's serial connection
                mesh_port=mesh_port,
                aes_key=aes_key,
            )
            # Override TOM interface to use GUI's send_serial instead of own serial port
            self.mesh_bridge.tom = None  # Don't open a second TOM serial
            self.mesh_bridge.mesh.connect()

            # Hook Meshtastic events into GUI
            def on_mesh_text(from_id, text, packet):
                name = self.mesh_bridge.mapper.get_mesh_name(from_id)
                tom_id = self.mesh_bridge.mapper.get_tom_id(from_id)
                self._emit("log", text=f"MESH MSG from {name}: {text}")
                # Forward to TOM mesh
                display = f"[Mesh {name}] {text}"
                self._send_serial(f"MSG,FFFF,{display}")

            def on_mesh_position(from_id, lat, lon, alt, packet):
                name = self.mesh_bridge.mapper.get_mesh_name(from_id)
                self._emit("log", text=f"MESH POS from {name}: {lat:.6f},{lon:.6f}")

            def on_mesh_nodeinfo(from_id, name, packet):
                self.mesh_bridge.mapper.set_mesh_name(from_id, name)
                tom_id = self.mesh_bridge.mapper.get_tom_id(from_id)
                self._emit("log", text=f"MESH node: {name} → {tom_id}")

            self.mesh_bridge.mesh.on_text = on_mesh_text
            self.mesh_bridge.mesh.on_position = on_mesh_position
            self.mesh_bridge.mesh.on_nodeinfo = on_mesh_nodeinfo

            self.mesh_bridge_btn.configure(text="Stop Bridge")
            self.mesh_status_label.configure(text="Running", text_color=COLORS["good"])

            # Save port
            self._saved_config["mesh_port"] = mesh_port
            self._save_config()
        except Exception as e:
            self.mesh_status_label.configure(text=f"Error: {e}", text_color=COLORS["bad"])
            self.mesh_bridge = None

    def _eval_automation(self):
        """Periodic automation rule evaluation."""
        try:
            self.engine.evaluate_all()
            # Update live values on canvas if visible
            if self.tabview.get() == "Logic":
                self.auto_canvas.update_live_values()
                rule = self.auto_canvas.current_rule
                if rule:
                    status_colors = {"active": COLORS["good"], "triggered": COLORS["bad"],
                                     "queued": COLORS["warn"], "disabled": COLORS["faint"],
                                     "error": COLORS["bad"], "idle": COLORS["dim"]}
                    self.rule_status_dot.configure(
                        text_color=status_colors.get(rule.status, COLORS["dim"]))
                # Update event log with queue status (skip if deploy message showing)
                if hasattr(self, '_deploy_msg_until') and time.time() < self._deploy_msg_until:
                    pass  # Don't overwrite deploy message
                else:
                    q = self.engine.queue_depth
                    q_txt = f"  |  Queue: {q} pending" if q > 0 else ""
                    if self.engine.event_log:
                        last = self.engine.event_log[-1]
                        ts = time.strftime("%H:%M:%S", time.localtime(last[0]))
                        self.logic_log.configure(
                            text=f"{ts} [{last[1]}] {last[2]}{q_txt}",
                            text_color="#FFE000" if q > 5 else "#B0B0B0")
                    elif q > 0:
                        self.logic_log.configure(
                            text=f"Queue: {q} pending",
                            text_color="#FFE000")
        except Exception as e:
            self.engine._log("Engine", f"Tick error: {type(e).__name__}: {e}")
        self.root.after(1000, self._eval_automation)

    # ─── Sensor Dashboard ──────────────────────────────────

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

    def _set_default_pins_for_board(self, board_name: str):
        """Set default relay/sensor pins based on board type from Pins.h defaults."""
        defaults = {
            "Heltec V3": (["2","3","4","5","6","7"], ["19","20","33"]),
            "Heltec V4": (["2","3","4","5","6","7"], ["33","34","15"]),
            "LoRa32 v1.6.1": (["2","4","12","15","17","33"], ["34","36","39"]),
            "XIAO ESP32S3": (["2","4","5","6"], ["43","44"]),
            "Heltec V2": (["2","12","13","17"], ["36","39"]),
        }
        for name, (r, s) in defaults.items():
            if name.lower() in board_name.lower():
                self.node_relay_pins = r
                self.node_sensor_pins = s
                # Store for local node
                if self.local_id and self.local_id != "????":
                    self.node_pin_configs[self.local_id] = (r, s)
                # Also set as default for all discovered nodes (same board type likely)
                for nid in self.topo_nodes:
                    if nid not in self.node_pin_configs:
                        self.node_pin_configs[nid] = (r, s)
                # Update canvas
                self.auto_canvas.relay_pins = r
                self.auto_canvas.sensor_pins = s
                self.auto_canvas.node_pin_configs = self.node_pin_configs
                break

    def _parse_beacon_event(self, text: str, triggered: bool):
        """Parse BEACON,TRIGGERED,<name>,... or BEACON,REVERTED,<name> from serial."""
        parts = text.split(",")
        if len(parts) >= 3:
            name = parts[2]
            if triggered:
                self.engine.beacon_data[name] = {
                    "timestamp": time.time(),
                    "triggered": True,
                    "type": parts[3] if len(parts) > 3 else "",
                    "arg": parts[4] if len(parts) > 4 else "",
                }
            else:
                if name in self.engine.beacon_data:
                    self.engine.beacon_data[name]["triggered"] = False
                    self.engine.beacon_data[name]["timestamp"] = time.time()

    def _parse_pins(self, text: str, from_node: str = ""):
        """Parse PINS,R:2,3,4|S:33,34 response from node."""
        data = text[5:]  # strip "PINS,"
        relay_pins = []
        sensor_pins = []
        for part in data.split("|"):
            if part.startswith("R:"):
                relay_pins = [p.strip() for p in part[2:].split(",") if p.strip()]
            elif part.startswith("S:"):
                sensor_pins = [p.strip() for p in part[2:].split(",") if p.strip()]

        # Store per-node pin config
        node_id = from_node if from_node else self.local_id
        if node_id and node_id != "????":
            self.node_pin_configs[node_id] = (relay_pins, sensor_pins)

        # Update local defaults (used when no specific node selected)
        if not from_node or from_node == self.local_id:
            self.node_relay_pins = relay_pins
            self.node_sensor_pins = sensor_pins

        # Update Logic Builder canvas with all known pin configs
        self.auto_canvas.relay_pins = self.node_relay_pins
        self.auto_canvas.sensor_pins = self.node_sensor_pins
        self.auto_canvas.node_pin_configs = self.node_pin_configs

    # ─── Lifecycle ──────────────────────────────────────────

    def on_close(self):
        self.engine.save_rules()
        self.disconnect()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


# ─── Entry Point ─────────────────────────────────────────────

if __name__ == "__main__":
    app = GatewayGUIApp()
    app.run()
