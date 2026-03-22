"""
TiggyOpenMesh — Automation Canvas
═════════════════════════════════
Visual block editor for PLC-style automation rules.
Drag blocks, wire ports, configure parameters, see live values.
"""

import tkinter as tk
from tkinter import simpledialog
from typing import Optional, Any

import customtkinter as ctk

from gui_common import COLORS
from automation_engine import (
    AutomationEngine, Rule, Block, Wire, BlockType, BLOCK_DEFS,
)


# ─── Constants ────────────────────────────────────────────────

BLOCK_W = 160
BLOCK_H_BASE = 64
PORT_R = 6
PORT_SPACING = 90  # Enough vertical gap that two single-port blocks can sit beside each input
GRID = 10

CATEGORY_COLORS = {
    "input":     {"fill": "#0a2838", "border": "#00E5FF"},
    "transform": {"fill": "#0a2818", "border": "#00E676"},
    "condition":  {"fill": "#282808", "border": "#FFE000"},
    "action":    {"fill": "#280a0a", "border": "#FF5252"},
}

PORT_COLORS = {"number": "#00E5FF", "bool": "#FFE000", "string": "#FFFFFF"}

CATEGORY_BLOCKS = {
    "Input": [BlockType.SENSOR_READ, BlockType.BEACON_DETECT, BlockType.CONSTANT],
    "Transform": [BlockType.SCALE, BlockType.MOVING_AVG, BlockType.DELTA_RATE],
    "Condition": [BlockType.COMPARE, BlockType.AND_GATE, BlockType.OR_GATE,
                  BlockType.NOT_GATE, BlockType.DEBOUNCE, BlockType.LATCH],
    "Action": [BlockType.SET_RELAY, BlockType.PULSE_RELAY,
               BlockType.SEND_BROADCAST, BlockType.SEND_DIRECT],
}

STATUS_COLORS = {
    "active": "#00FF00", "triggered": "#FF5252", "stale": "#FFA500",
    "error": "#FF0000", "idle": "#444444", "disabled": "#333333",
    "queued": "#FFE000",
}


# ─── Config Dialogs ───────────────────────────────────────────

def show_block_config(parent, block: Block, discovered_nodes: dict,
                      relay_pins: list[str] = None, sensor_pins: list[str] = None):
    """Show a configuration dialog for a block. Returns True if changed."""
    bt = block.block_type
    cfg = block.config
    dialog = ctk.CTkToplevel(parent)
    dialog.title(f"Configure: {BLOCK_DEFS[bt]['label']}")
    dialog.geometry("340x300")
    dialog.configure(fg_color=COLORS["panel"])
    dialog.transient(parent)
    dialog.after(50, lambda: dialog.grab_set())  # Delay grab to avoid focus race
    dialog.after(100, dialog.lift)                # Ensure dialog is on top
    dialog.focus_force()

    entries = {}
    row = 0

    def add_field(label: str, key: str, default="", width=160):
        nonlocal row
        ctk.CTkLabel(dialog, text=label, text_color=COLORS["dim"],
                     font=("Consolas", 11)).grid(row=row, column=0, padx=10, pady=4, sticky="e")
        e = ctk.CTkEntry(dialog, width=width, font=("Consolas", 11))
        e.insert(0, str(cfg.get(key, default)))
        e.grid(row=row, column=1, padx=10, pady=4, sticky="w")
        entries[key] = e
        row += 1

    def add_combo(label: str, key: str, values: list, default=""):
        nonlocal row
        ctk.CTkLabel(dialog, text=label, text_color=COLORS["dim"],
                     font=("Consolas", 11)).grid(row=row, column=0, padx=10, pady=4, sticky="e")
        c = ctk.CTkComboBox(dialog, values=values, width=160, font=("Consolas", 11))
        c.set(str(cfg.get(key, default)))
        c.grid(row=row, column=1, padx=10, pady=4, sticky="w")
        entries[key] = c
        row += 1

    def add_node_combo(label: str, key: str):
        nonlocal row
        nodes = sorted(discovered_nodes.keys()) if discovered_nodes else []
        current = cfg.get(key, "")
        if current and current not in nodes:
            nodes.insert(0, current)
        if not nodes:
            nodes = [""]
        ctk.CTkLabel(dialog, text=label, text_color=COLORS["dim"],
                     font=("Consolas", 11)).grid(row=row, column=0, padx=10, pady=4, sticky="e")
        c = ctk.CTkComboBox(dialog, values=nodes, width=160, font=("Consolas", 11))
        c.set(current)
        c.grid(row=row, column=1, padx=10, pady=4, sticky="w")
        entries[key] = c
        row += 1

    # Build fields based on block type
    if bt == BlockType.SENSOR_READ:
        add_node_combo("Node ID:", "node_id")
        pins = sensor_pins if sensor_pins else []
        if pins:
            add_combo("Pin:", "pin", pins, str(cfg.get("pin", pins[0] if pins else "0")))
        else:
            add_field("Pin:", "pin", "0")
        add_field("Label:", "label", "")

    elif bt == BlockType.BEACON_DETECT:
        add_field("Beacon ID:", "beacon_id", "")
        add_field("Name:", "name", "")
        add_field("RSSI Thresh:", "rssi_thresh", "-70")

        # Scan button — sends BEACON,SCAN and shows results
        scan_frame = ctk.CTkFrame(dialog, fg_color="transparent")
        scan_frame.grid(row=row, column=0, columnspan=2, pady=4, padx=10, sticky="ew")
        scan_result = ctk.CTkTextbox(scan_frame, height=80, font=("Consolas", 9),
                                      fg_color=COLORS["bg"], text_color=COLORS["text"])
        scan_result.pack(fill="x", pady=(0, 4))

        def do_scan():
            scan_result.delete("1.0", "end")
            scan_result.insert("end", "Scanning... (2 seconds)\n")
            # Get the gateway app reference to send serial command
            try:
                top = parent
                while top and not hasattr(top, 'gateway'):
                    top = top.master
                if top and hasattr(top, 'gateway') and top.gateway and top.gateway.serial_conn:
                    top.gateway.serial_conn.write(b"BEACON,SCAN\n")
                    import time as _time
                    _time.sleep(2.5)
                    response = b""
                    while top.gateway.serial_conn.in_waiting:
                        response += top.gateway.serial_conn.read(top.gateway.serial_conn.in_waiting)
                    lines = response.decode("ascii", errors="replace").strip().split("\n")
                    scan_result.delete("1.0", "end")
                    found = False
                    for line in lines:
                        if line.startswith("BEACONSCAN,"):
                            parts = line.split(",")
                            count = int(parts[1]) if len(parts) > 1 else 0
                            if count == 0:
                                scan_result.insert("end", "No beacons found\n")
                            for entry in parts[2:]:
                                scan_result.insert("end", entry + "\n")
                            found = True
                    if not found:
                        scan_result.insert("end", "No scan response received\n")
                else:
                    scan_result.insert("end", "Not connected to node\n")
            except Exception as e:
                scan_result.insert("end", f"Error: {e}\n")

        ctk.CTkButton(scan_frame, text="Scan for Beacons", width=140,
                       font=("Consolas", 9), fg_color=COLORS["accent"],
                       text_color="#000", command=do_scan).pack()
        scan_result.insert("end", "Click Scan to find nearby beacons.\nCopy MAC or UUID into Beacon ID field.")
        row += 1

    elif bt == BlockType.CONSTANT:
        add_field("Value:", "value", "0")

    elif bt == BlockType.SCALE:
        add_field("Factor:", "factor", "1.0")
        add_field("Offset:", "offset", "0.0")
        add_field("Unit:", "unit", "")

    elif bt == BlockType.MOVING_AVG:
        add_field("Window:", "window", "5")

    elif bt == BlockType.COMPARE:
        _op_labels = [">   Greater than", "<   Less than", "=   Equal to",
                      "!=  Not equal", ">=  Greater or equal", "<=  Less or equal"]
        _code_to_label = {"GT": ">   Greater than", "LT": "<   Less than",
                          "EQ": "=   Equal to", "NE": "!=  Not equal",
                          "GE": ">=  Greater or equal", "LE": "<=  Less or equal"}
        # Show the display label in the combo; on_ok converts back to code
        cfg["operator"] = _code_to_label.get(cfg.get("operator", "GT"), ">   Greater than")
        add_combo("Operator:", "operator", _op_labels, ">   Greater than")

    elif bt == BlockType.DEBOUNCE:
        add_field("Hold (sec):", "hold_seconds", "5.0")

    elif bt == BlockType.SET_RELAY:
        add_node_combo("Node ID:", "node_id")
        pins = relay_pins if relay_pins else []
        if pins:
            add_combo("Pin:", "pin", pins, str(cfg.get("pin", pins[0] if pins else "0")))
        else:
            add_field("Pin:", "pin", "0")
        add_combo("Action:", "action", ["1", "0"], "1")

    elif bt == BlockType.PULSE_RELAY:
        add_node_combo("Node ID:", "node_id")
        pins = relay_pins if relay_pins else []
        if pins:
            add_combo("Pin:", "pin", pins, str(cfg.get("pin", pins[0] if pins else "0")))
        else:
            add_field("Pin:", "pin", "0")
        add_field("Duration (ms):", "duration_ms", "500")

    elif bt == BlockType.SEND_BROADCAST:
        add_field("Msg (true):", "message_true", "")
        add_field("Msg (false):", "message_false", "")

    elif bt == BlockType.SEND_DIRECT:
        add_node_combo("Node ID:", "node_id")
        add_field("Msg (true):", "message_true", "")
        add_field("Msg (false):", "message_false", "")

    # OK / Cancel buttons
    changed = [False]

    def on_ok():
        for key, widget in entries.items():
            val = widget.get() if hasattr(widget, "get") else ""
            # Type conversion
            if key in ("pin", "duration_ms", "window", "rssi_thresh"):
                try:
                    val = int(val)
                except ValueError:
                    val = 0
            elif key in ("factor", "offset", "hold_seconds", "value"):
                try:
                    val = float(val)
                except ValueError:
                    val = 0.0
            elif key == "action":
                try:
                    val = int(val)
                except ValueError:
                    val = 1
            elif key == "operator":
                # Convert display label back to internal code
                op_map = {">": "GT", "<": "LT", "=": "EQ",
                          "!=": "NE", ">=": "GE", "<=": "LE"}
                code = val.split()[0] if val else "GT"
                val = op_map.get(code, "GT")
            cfg[key] = val
        changed[0] = True
        dialog.destroy()

    btn_frame = ctk.CTkFrame(dialog, fg_color="transparent")
    btn_frame.grid(row=row, column=0, columnspan=2, pady=15)
    ctk.CTkButton(btn_frame, text="OK", width=80, fg_color=COLORS["accent"],
                  text_color="#000", command=on_ok).pack(side="left", padx=10)
    ctk.CTkButton(btn_frame, text="Cancel", width=80, fg_color=COLORS["faint"],
                  text_color=COLORS["text"],
                  command=dialog.destroy).pack(side="left", padx=10)

    dialog.wait_window()
    return changed[0]


# ─── Canvas Widget ────────────────────────────────────────────

class AutomationCanvas:
    """Visual block editor drawn on a tkinter Canvas."""

    def __init__(self, parent_frame: ctk.CTkFrame, engine: AutomationEngine,
                 on_change: Optional[callable] = None):
        self.engine = engine
        self.on_change = on_change
        self.parent = parent_frame
        self.current_rule: Optional[Rule] = None
        # Pin lists from connected node (set by gateway GUI)
        self.relay_pins: list[str] = []
        self.sensor_pins: list[str] = []
        # Per-node pin configs: nodeId → (relay_pins, sensor_pins)
        self.node_pin_configs: dict[str, tuple[list[str], list[str]]] = {}

        # Canvas with scrolling
        self._canvas_frame = tk.Frame(parent_frame, bg=COLORS["bg"])
        self._canvas_frame.pack(fill="both", expand=True)

        self.canvas = tk.Canvas(self._canvas_frame, bg=COLORS["bg"],
                                highlightthickness=0, cursor="crosshair",
                                scrollregion=(0, 0, 2000, 2000))
        self._vscroll = tk.Scrollbar(self._canvas_frame, orient="vertical",
                                      command=self.canvas.yview)
        self._hscroll = tk.Scrollbar(self._canvas_frame, orient="horizontal",
                                      command=self.canvas.xview)
        self.canvas.configure(xscrollcommand=self._hscroll.set,
                              yscrollcommand=self._vscroll.set)
        self._vscroll.pack(side="right", fill="y")
        self._hscroll.pack(side="bottom", fill="x")
        self.canvas.pack(fill="both", expand=True)

        # Interaction state
        self._selected: Optional[str] = None  # block id
        self._dragging = False
        self._drag_off = (0, 0)
        self._drawing_wire: Optional[dict] = None  # {from_block, from_port, type}
        self._wire_line_id = None
        self._hover_block: Optional[str] = None

        # Block canvas items: block_id -> list of canvas item ids
        self._block_items: dict[str, list[int]] = {}
        # Port positions: (block_id, port_name, "in"|"out") -> (cx, cy)
        self._port_pos: dict[tuple[str, str, str], tuple[float, float]] = {}
        # Wire canvas items: wire_id -> canvas item id
        self._wire_items: dict[str, int] = {}
        # Value labels: (block_id, port_name) -> canvas item id
        self._value_labels: dict[tuple[str, str], int] = {}

        # Context menu
        self._ctx_menu = tk.Menu(self.canvas, tearoff=0,
                                 bg=COLORS["panel"], fg=COLORS["text"],
                                 activebackground=COLORS["accent"],
                                 activeforeground="#000",
                                 font=("Consolas", 10))

        # Bindings
        self.canvas.bind("<Button-1>", self._on_click)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self.canvas.bind("<Double-Button-1>", self._on_double_click)
        self.canvas.bind("<Button-3>", self._on_right_click)
        self.canvas.bind("<Delete>", self._on_delete_key)
        self.canvas.bind("<BackSpace>", self._on_delete_key)
        self.canvas.bind("<MouseWheel>", self._on_mousewheel)
        self.canvas.bind("<Shift-MouseWheel>", self._on_shift_mousewheel)
        self.canvas.focus_set()

    # ─── Public API ───────────────────────────────────────────

    def set_rule(self, rule: Optional[Rule]):
        self.current_rule = rule
        self._selected = None
        self._drawing_wire = None
        self.redraw()

    def redraw(self):
        """Full redraw of all blocks and wires."""
        self.canvas.delete("all")
        self._block_items.clear()
        self._port_pos.clear()
        self._wire_items.clear()
        self._value_labels.clear()

        if not self.current_rule:
            self.canvas.create_text(
                self.canvas.winfo_width() // 2 or 300,
                self.canvas.winfo_height() // 2 or 100,
                text="Select or create a rule to begin",
                fill="#B0B0B0", font=("Consolas", 13))
            return

        # Draw blocks first (populates _port_pos needed by wires)
        for block in self.current_rule.blocks:
            self._draw_block(block)

        # Draw wires (need port positions from blocks above)
        for wire in self.current_rule.wires:
            self._draw_wire(wire)
        # Push wires behind blocks visually
        self.canvas.tag_lower("wire")

        # Update scroll region to fit all blocks
        self._update_scrollregion()

    def _update_scrollregion(self):
        """Expand scroll region to encompass all blocks with padding."""
        if not self.current_rule or not self.current_rule.blocks:
            self.canvas.configure(scrollregion=(0, 0, 2000, 2000))
            return
        max_x = max(b.x + BLOCK_W + 40 for b in self.current_rule.blocks)
        max_y = max(b.y + 200 for b in self.current_rule.blocks)
        cw = self.canvas.winfo_width() or 800
        ch = self.canvas.winfo_height() or 600
        self.canvas.configure(scrollregion=(0, 0, max(cw, max_x), max(ch, max_y)))

    def update_live_values(self):
        """Update live value labels on wires (called periodically)."""
        if not self.current_rule:
            return
        values = self.engine.get_wire_values(self.current_rule)
        for wire in self.current_rule.wires:
            key = f"{wire.from_block}:{wire.from_port}"
            val = values.get(key)
            label_key = (wire.id, "val")
            if label_key in self._value_labels:
                self.canvas.delete(self._value_labels[label_key])
            if val is not None:
                # Position at midpoint of wire
                p1 = self._port_pos.get((wire.from_block, wire.from_port, "out"))
                p2 = self._port_pos.get((wire.to_block, wire.to_port, "in"))
                if p1 and p2:
                    mx = (p1[0] + p2[0]) / 2
                    my = (p1[1] + p2[1]) / 2 - 10
                    txt = f"{val}" if not isinstance(val, float) else f"{val:.1f}"
                    if isinstance(val, bool):
                        txt = str(val)
                    item = self.canvas.create_text(
                        mx, my, text=txt, fill=COLORS["accent"],
                        font=("Consolas", 10))
                    self._value_labels[label_key] = item

        # Update block status dots
        for block in self.current_rule.blocks:
            dot_key = f"{block.id}_status_dot"
            items = self.canvas.find_withtag(dot_key)
            color = STATUS_COLORS.get(block.status, STATUS_COLORS["idle"])
            for item in items:
                self.canvas.itemconfig(item, fill=color, outline=color)

    # ─── Drawing ──────────────────────────────────────────────

    def _draw_block(self, block: Block):
        bdef = BLOCK_DEFS.get(block.block_type, {})
        cat = bdef.get("category", "input")
        colors = CATEGORY_COLORS.get(cat, CATEGORY_COLORS["input"])
        ins = bdef.get("inputs", [])
        outs = bdef.get("outputs", [])
        n_ports = max(len(ins), len(outs), 1)
        h = BLOCK_H_BASE + max(0, n_ports - 1) * PORT_SPACING

        x, y = block.x, block.y
        items = []
        border = colors["border"]
        if self._selected == block.id:
            border = "#FFFFFF"

        # Body
        r = self.canvas.create_rectangle(
            x, y, x + BLOCK_W, y + h,
            fill=colors["fill"], outline=border, width=2,
            tags=(f"block_{block.id}", "block"))
        items.append(r)

        # Title
        label = bdef.get("label", block.block_type.value)
        t = self.canvas.create_text(
            x + 8, y + 8, text=label, anchor="nw",
            fill=COLORS["text"], font=("Consolas", 11, "bold"),
            tags=(f"block_{block.id}",))
        items.append(t)

        # Status dot
        dot = self.canvas.create_oval(
            x + BLOCK_W - 14, y + 5, x + BLOCK_W - 6, y + 13,
            fill=STATUS_COLORS.get(block.status, "#444"),
            outline=STATUS_COLORS.get(block.status, "#444"),
            tags=(f"block_{block.id}", f"{block.id}_status_dot"))
        items.append(dot)

        # Config summary line
        summary = self._block_summary(block)
        if summary:
            s = self.canvas.create_text(
                x + 8, y + 26, text=summary, anchor="nw",
                fill="#B0B0B0", font=("Consolas", 10),
                tags=(f"block_{block.id}",))
            items.append(s)

        # Input ports (left side)
        for i, (pname, ptype) in enumerate(ins):
            py = y + 44 + i * PORT_SPACING
            px = x
            color = PORT_COLORS.get(ptype, "#FFF")
            # Check if connected
            connected = any(w.to_block == block.id and w.to_port == pname
                           for w in (self.current_rule.wires if self.current_rule else []))
            p = self.canvas.create_oval(
                px - PORT_R, py - PORT_R, px + PORT_R, py + PORT_R,
                fill=color if connected else colors["fill"],
                outline=color, width=2,
                tags=(f"port_{block.id}_{pname}_in", "port"))
            items.append(p)
            lbl = self.canvas.create_text(
                px + PORT_R + 4, py, text=pname, anchor="w",
                fill="#B0B0B0", font=("Consolas", 9),
                tags=(f"block_{block.id}",))
            items.append(lbl)
            self._port_pos[(block.id, pname, "in")] = (px, py)

        # Output ports (right side)
        for i, (pname, ptype) in enumerate(outs):
            py = y + 44 + i * PORT_SPACING
            px = x + BLOCK_W
            color = PORT_COLORS.get(ptype, "#FFF")
            connected = any(w.from_block == block.id and w.from_port == pname
                           for w in (self.current_rule.wires if self.current_rule else []))
            p = self.canvas.create_oval(
                px - PORT_R, py - PORT_R, px + PORT_R, py + PORT_R,
                fill=color if connected else colors["fill"],
                outline=color, width=2,
                tags=(f"port_{block.id}_{pname}_out", "port"))
            items.append(p)
            lbl = self.canvas.create_text(
                px - PORT_R - 4, py, text=pname, anchor="e",
                fill="#B0B0B0", font=("Consolas", 9),
                tags=(f"block_{block.id}",))
            items.append(lbl)
            self._port_pos[(block.id, pname, "out")] = (px, py)

        # Error text
        if block.error:
            err = self.canvas.create_text(
                x + BLOCK_W // 2, y + h + 12, text=block.error,
                fill="#FF5252", font=("Consolas", 9),
                tags=(f"block_{block.id}",))
            items.append(err)

        self._block_items[block.id] = items

    def _draw_wire(self, wire: Wire):
        p1 = self._port_pos.get((wire.from_block, wire.from_port, "out"))
        p2 = self._port_pos.get((wire.to_block, wire.to_port, "in"))
        if not p1 or not p2:
            # Ports not drawn yet — will be redrawn after blocks
            return
        # Bezier-like smooth line
        mx = (p1[0] + p2[0]) / 2
        line = self.canvas.create_line(
            p1[0], p1[1], mx, p1[1], mx, p2[1], p2[0], p2[1],
            fill=COLORS["dim"], width=2, smooth=True,
            tags=(f"wire_{wire.id}", "wire"))
        self._wire_items[wire.id] = line

    def _block_summary(self, block: Block) -> str:
        cfg = block.config
        bt = block.block_type
        if bt == BlockType.SENSOR_READ:
            nid = cfg.get("node_id", "?")
            pin = cfg.get("pin", "?")
            lbl = cfg.get("label", "")
            return f"{nid}:pin{pin}" + (f' "{lbl}"' if lbl else "")
        if bt == BlockType.BEACON_DETECT:
            bid = cfg.get("beacon_id", "?")
            name = cfg.get("name", "")
            thresh = cfg.get("rssi_thresh", -70)
            label = name if name else (bid[:12] + "..." if len(bid) > 12 else bid)
            return f"{label} {thresh}dBm"
        if bt == BlockType.CONSTANT:
            return str(cfg.get("value", 0))
        if bt == BlockType.SCALE:
            f = cfg.get("factor", 1)
            o = cfg.get("offset", 0)
            u = cfg.get("unit", "")
            return f"x{f}" + (f"+{o}" if o else "") + (f" {u}" if u else "")
        if bt == BlockType.MOVING_AVG:
            return f"window={cfg.get('window', 5)}"
        if bt == BlockType.COMPARE:
            ops = {"GT": ">", "LT": "<", "EQ": "==", "NE": "!=", "GE": ">=", "LE": "<="}
            return ops.get(cfg.get("operator", "GT"), "?")
        if bt == BlockType.DEBOUNCE:
            return f"{cfg.get('hold_seconds', 5)}s"
        if bt == BlockType.SET_RELAY:
            return f"{cfg.get('node_id', '?')}:pin{cfg.get('pin', '?')} {'HIGH' if cfg.get('action', 1) else 'LOW'}"
        if bt == BlockType.PULSE_RELAY:
            return f"{cfg.get('node_id', '?')}:pin{cfg.get('pin', '?')} {cfg.get('duration_ms', 500)}ms"
        if bt == BlockType.SEND_BROADCAST:
            return cfg.get("message_true", "")[:20]
        if bt == BlockType.SEND_DIRECT:
            return f"{cfg.get('node_id', '?')}: {cfg.get('message_true', '')[:14]}"
        return ""

    # ─── Interaction ──────────────────────────────────────────

    def _find_block_at(self, x: float, y: float) -> Optional[str]:
        if not self.current_rule:
            return None
        for block in reversed(self.current_rule.blocks):
            bdef = BLOCK_DEFS.get(block.block_type, {})
            n_ports = max(len(bdef.get("inputs", [])), len(bdef.get("outputs", [])), 1)
            h = BLOCK_H_BASE + max(0, n_ports - 1) * PORT_SPACING
            if block.x <= x <= block.x + BLOCK_W and block.y <= y <= block.y + h:
                return block.id
        return None

    def _find_port_at(self, x: float, y: float) -> Optional[tuple[str, str, str]]:
        """Returns (block_id, port_name, "in"|"out") or None.
        When drawing a wire (from output), prioritise input ports so overlapping
        blocks don't prevent connections."""
        # If drawing a wire, check input ports first (we need to land on an input)
        if self._drawing_wire:
            for (bid, pname, direction), (px, py) in self._port_pos.items():
                if direction == "in" and abs(x - px) <= PORT_R + 4 and abs(y - py) <= PORT_R + 4:
                    return (bid, pname, direction)
        # Then check all ports
        for (bid, pname, direction), (px, py) in self._port_pos.items():
            if abs(x - px) <= PORT_R + 3 and abs(y - py) <= PORT_R + 3:
                return (bid, pname, direction)
        return None

    def _find_wire_at(self, x: float, y: float) -> Optional[str]:
        items = self.canvas.find_closest(x, y, halo=5)
        if items:
            tags = self.canvas.gettags(items[0])
            for tag in tags:
                if tag.startswith("wire_"):
                    return tag[5:]
        return None

    def _on_click(self, event):
        self.canvas.focus_set()
        x, y = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)

        # Check if clicking a port
        port = self._find_port_at(x, y)
        if port:
            bid, pname, direction = port
            if self._drawing_wire:
                # Complete wire connection
                self._finish_wire(bid, pname, direction)
                return
            elif direction == "out":
                # Start drawing a wire
                bdef = BLOCK_DEFS.get(
                    self.current_rule.get_block(bid).block_type if self.current_rule else None, {})
                outs = dict(bdef.get("outputs", []))
                ptype = outs.get(pname, "number")
                self._drawing_wire = {
                    "from_block": bid, "from_port": pname, "type": ptype
                }
                return
            elif direction == "in":
                # Can also start from input (reverse direction)
                bdef = BLOCK_DEFS.get(
                    self.current_rule.get_block(bid).block_type if self.current_rule else None, {})
                ins = dict(bdef.get("inputs", []))
                ptype = ins.get(pname, "number")
                # Look for an existing wire to this port and disconnect it
                if self.current_rule:
                    for w in self.current_rule.wires:
                        if w.to_block == bid and w.to_port == pname:
                            self.current_rule.remove_wire(w.id)
                            self._notify_change()
                            self.redraw()
                            return
                return

        # Cancel wire drawing on click elsewhere
        if self._drawing_wire:
            self._drawing_wire = None
            if self._wire_line_id:
                self.canvas.delete(self._wire_line_id)
                self._wire_line_id = None
            return

        # Check if clicking a block
        bid = self._find_block_at(x, y)
        if bid:
            self._selected = bid
            block = self.current_rule.get_block(bid) if self.current_rule else None
            if block:
                self._dragging = True
                self._drag_off = (x - block.x, y - block.y)
            self.redraw()
            return

        # Click on empty canvas — deselect
        self._selected = None
        self.redraw()

    def _on_drag(self, event):
        x, y = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
        if self._dragging and self._selected and self.current_rule:
            block = self.current_rule.get_block(self._selected)
            if block:
                # Snap to grid
                new_x = round((x - self._drag_off[0]) / GRID) * GRID
                new_y = round((y - self._drag_off[1]) / GRID) * GRID
                block.x = max(0, new_x)
                block.y = max(0, new_y)
                self.redraw()

        # Drawing wire preview
        if self._drawing_wire:
            src = self._port_pos.get(
                (self._drawing_wire["from_block"],
                 self._drawing_wire["from_port"], "out"))
            if src:
                if self._wire_line_id:
                    self.canvas.delete(self._wire_line_id)
                mx = (src[0] + x) / 2
                self._wire_line_id = self.canvas.create_line(
                    src[0], src[1], mx, src[1], mx, y, x, y,
                    fill=PORT_COLORS.get(self._drawing_wire["type"], "#FFF"),
                    width=2, smooth=True, dash=(4, 4))

    def _on_release(self, event):
        if self._dragging:
            self._dragging = False
            self._notify_change()

    def _on_mousewheel(self, event):
        self.canvas.yview_scroll(-1 * (event.delta // 120), "units")

    def _on_shift_mousewheel(self, event):
        self.canvas.xview_scroll(-1 * (event.delta // 120), "units")

    def _finish_wire(self, to_block: str, to_port: str, direction: str):
        """Complete a wire connection."""
        if self._wire_line_id:
            self.canvas.delete(self._wire_line_id)
            self._wire_line_id = None

        if not self._drawing_wire or not self.current_rule:
            self._drawing_wire = None
            return

        from_block = self._drawing_wire["from_block"]
        from_port = self._drawing_wire["from_port"]
        from_type = self._drawing_wire["type"]

        # Must connect output to input
        if direction != "in":
            self._drawing_wire = None
            return

        # Can't connect to self
        if from_block == to_block:
            self._drawing_wire = None
            return

        # Type check — allow number→bool (non-zero = true) and bool→number (true=1, false=0)
        to_block_obj = self.current_rule.get_block(to_block)
        if to_block_obj:
            bdef = BLOCK_DEFS.get(to_block_obj.block_type, {})
            ins = dict(bdef.get("inputs", []))
            to_type = ins.get(to_port, "number")
            compatible = (from_type == to_type or
                          {from_type, to_type} == {"number", "bool"})
            if not compatible:
                self._drawing_wire = None
                return

        # Create wire
        wire = self.current_rule.add_wire(from_block, from_port, to_block, to_port)
        self._drawing_wire = None
        if wire:
            self._notify_change()
            self.redraw()

    def _on_double_click(self, event):
        """Open config dialog for the block under cursor."""
        bid = self._find_block_at(self.canvas.canvasx(event.x), self.canvas.canvasy(event.y))
        if bid and self.current_rule:
            block = self.current_rule.get_block(bid)
            if block:
                changed = show_block_config(
                    self.parent.winfo_toplevel(), block,
                    self.engine.discovered_nodes,
                    relay_pins=list(self.relay_pins),
                    sensor_pins=list(self.sensor_pins))
                if changed:
                    self._notify_change()
                    self.redraw()

    def _on_right_click(self, event):
        """Context menu for delete."""
        self._ctx_menu.delete(0, "end")

        # Check port first (to disconnect)
        cx, cy = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
        port = self._find_port_at(cx, cy)
        if port and self.current_rule:
            bid, pname, direction = port
            if direction == "in":
                for w in self.current_rule.wires:
                    if w.to_block == bid and w.to_port == pname:
                        self._ctx_menu.add_command(
                            label=f"Disconnect {pname}",
                            command=lambda wid=w.id: self._delete_wire(wid))
                        self._ctx_menu.tk_popup(event.x_root, event.y_root)
                        return

        # Check block
        bid = self._find_block_at(cx, cy)
        if bid and self.current_rule:
            block = self.current_rule.get_block(bid)
            if block:
                label = BLOCK_DEFS.get(block.block_type, {}).get("label", "Block")
                self._ctx_menu.add_command(
                    label=f"Configure {label}...",
                    command=lambda: self._configure_block(bid))
                self._ctx_menu.add_separator()
                self._ctx_menu.add_command(
                    label=f"Delete {label}",
                    command=lambda: self._delete_block(bid))
                self._ctx_menu.tk_popup(event.x_root, event.y_root)
                return

        # Check wire
        wid = self._find_wire_at(event.x, event.y)
        if wid:
            self._ctx_menu.add_command(
                label="Delete wire",
                command=lambda: self._delete_wire(wid))
            self._ctx_menu.tk_popup(event.x_root, event.y_root)

    def _on_delete_key(self, event):
        if self._selected and self.current_rule:
            self._delete_block(self._selected)

    def _configure_block(self, block_id: str):
        if self.current_rule:
            block = self.current_rule.get_block(block_id)
            if block:
                # Look up pins for the node selected in this block's config
                r_pins = list(self.relay_pins)  # Copy to avoid reference issues
                s_pins = list(self.sensor_pins)
                node_id = block.config.get("node_id", "")
                if node_id and node_id in self.node_pin_configs:
                    r_pins, s_pins = self.node_pin_configs[node_id]
                    r_pins = list(r_pins)
                    s_pins = list(s_pins)
                changed = show_block_config(
                    self.parent.winfo_toplevel(), block,
                    self.engine.discovered_nodes,
                    relay_pins=r_pins,
                    sensor_pins=s_pins)
                if changed:
                    self._notify_change()
                    self.redraw()

    def _delete_block(self, block_id: str):
        if self.current_rule:
            self.current_rule.remove_block(block_id)
            if self._selected == block_id:
                self._selected = None
            self._notify_change()
            self.redraw()

    def _delete_wire(self, wire_id: str):
        if self.current_rule:
            self.current_rule.remove_wire(wire_id)
            self._notify_change()
            self.redraw()

    def _notify_change(self):
        self.engine.save_rules()
        if self.on_change:
            self.on_change()

    def add_block(self, block_type: BlockType):
        """Add a new block to the current rule at a sensible position."""
        if not self.current_rule:
            return
        # Place near center, offset by existing block count
        # Place blocks in a grid with enough spacing to prevent overlap
        # Block size is ~160x86, so 200x120 grid gives clear gaps
        n = len(self.current_rule.blocks)
        cx = 20 + (n % 3) * 220
        cy = 20 + (n // 3) * 180
        block = self.current_rule.add_block(block_type, cx, cy)
        self._notify_change()
        self.redraw()
        # Open config immediately for blocks that need it
        if block_type in (BlockType.SENSOR_READ, BlockType.SET_RELAY,
                          BlockType.PULSE_RELAY, BlockType.SEND_BROADCAST,
                          BlockType.SEND_DIRECT, BlockType.BEACON_DETECT):
            show_block_config(
                self.parent.winfo_toplevel(), block,
                self.engine.discovered_nodes,
                relay_pins=list(self.relay_pins),
                sensor_pins=list(self.sensor_pins))
            self._notify_change()
            self.redraw()
