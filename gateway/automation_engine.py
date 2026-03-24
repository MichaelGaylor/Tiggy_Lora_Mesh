"""
TiggyOpenMesh — Automation Engine
══════════════════════════════════
PLC-style rule evaluation engine for the gateway GUI.
Rules are built visually with draggable blocks and evaluated periodically.

Data model, JSON persistence, topological-sort evaluator, anti-flood protection.
"""

import json
import os
import time
import uuid
from collections import defaultdict
from enum import Enum
from typing import Any, Callable, Optional


# ─── Block Types ──────────────────────────────────────────────

class BlockType(str, Enum):
    # Inputs
    SENSOR_READ = "sensor_read"
    BEACON_DETECT = "beacon_detect"
    CONSTANT = "constant"
    # Transforms
    SCALE = "scale"
    MOVING_AVG = "moving_avg"
    DELTA_RATE = "delta_rate"
    # Conditions
    COMPARE = "compare"
    AND_GATE = "and_gate"
    OR_GATE = "or_gate"
    NOT_GATE = "not_gate"
    DEBOUNCE = "debounce"
    LATCH = "latch"
    # Actions
    SET_RELAY = "set_relay"
    PULSE_RELAY = "pulse_relay"
    SEND_BROADCAST = "send_broadcast"
    TELEGRAM_OUTPUT = "telegram_output"
    SEND_DIRECT = "send_direct"


# Block metadata: category, ports, default config
BLOCK_DEFS = {
    BlockType.SENSOR_READ: {
        "category": "input",
        "label": "Sensor Read",
        "inputs": [],
        "outputs": [("value", "number")],
        "defaults": {"node_id": "", "pin": 0, "label": ""},
    },
    BlockType.BEACON_DETECT: {
        "category": "input",
        "label": "Beacon Detect",
        "inputs": [],
        "outputs": [("detected", "bool"), ("rssi", "number")],
        "defaults": {"beacon_id": "", "name": "", "rssi_thresh": -70},
    },
    BlockType.CONSTANT: {
        "category": "input",
        "label": "Constant",
        "inputs": [],
        "outputs": [("value", "number")],
        "defaults": {"value": 0},
    },
    BlockType.SCALE: {
        "category": "transform",
        "label": "Scale",
        "inputs": [("in", "number")],
        "outputs": [("out", "number")],
        "defaults": {"factor": 1.0, "offset": 0.0, "unit": ""},
    },
    BlockType.MOVING_AVG: {
        "category": "transform",
        "label": "Moving Avg",
        "inputs": [("in", "number")],
        "outputs": [("out", "number")],
        "defaults": {"window": 5},
    },
    BlockType.DELTA_RATE: {
        "category": "transform",
        "label": "Delta/Rate",
        "inputs": [("in", "number")],
        "outputs": [("out", "number")],
        "defaults": {},
    },
    BlockType.COMPARE: {
        "category": "condition",
        "label": "Compare",
        "inputs": [("a", "number"), ("b", "number")],
        "outputs": [("result", "bool")],
        "defaults": {"operator": "GT"},
    },
    BlockType.AND_GATE: {
        "category": "condition",
        "label": "AND",
        "inputs": [("in1", "bool"), ("in2", "bool")],
        "outputs": [("result", "bool")],
        "defaults": {},
    },
    BlockType.OR_GATE: {
        "category": "condition",
        "label": "OR",
        "inputs": [("in1", "bool"), ("in2", "bool")],
        "outputs": [("result", "bool")],
        "defaults": {},
    },
    BlockType.NOT_GATE: {
        "category": "condition",
        "label": "NOT",
        "inputs": [("in", "bool")],
        "outputs": [("result", "bool")],
        "defaults": {},
    },
    BlockType.DEBOUNCE: {
        "category": "condition",
        "label": "Debounce",
        "inputs": [("in", "bool")],
        "outputs": [("result", "bool")],
        "defaults": {"hold_seconds": 5.0},
    },
    BlockType.LATCH: {
        "category": "condition",
        "label": "Latch (SR)",
        "inputs": [("set", "bool"), ("reset", "bool")],
        "outputs": [("q", "bool")],
        "defaults": {},
    },
    BlockType.SET_RELAY: {
        "category": "action",
        "label": "Set Relay",
        "inputs": [("trigger", "bool")],
        "outputs": [],
        "defaults": {"node_id": "", "pin": 0, "action": 1},
    },
    BlockType.PULSE_RELAY: {
        "category": "action",
        "label": "Pulse Relay",
        "inputs": [("trigger", "bool")],
        "outputs": [],
        "defaults": {"node_id": "", "pin": 0, "duration_ms": 500},
    },
    BlockType.SEND_BROADCAST: {
        "category": "action",
        "label": "Broadcast Msg",
        "inputs": [("trigger", "bool")],
        "outputs": [],
        "defaults": {"message_true": "", "message_false": ""},
    },
    BlockType.TELEGRAM_OUTPUT: {
        "category": "action",
        "label": "Telegram Output",
        "inputs": [("value", "number")],
        "outputs": [],
        "defaults": {"label": "", "unit": "", "format": "{label}: {value} {unit}"},
    },
    BlockType.SEND_DIRECT: {
        "category": "action",
        "label": "Direct Msg",
        "inputs": [("trigger", "bool")],
        "outputs": [],
        "defaults": {"node_id": "", "message_true": "", "message_false": ""},
    },
}


# ─── Data Classes ─────────────────────────────────────────────

class Block:
    __slots__ = ("id", "block_type", "x", "y", "config",
                 "last_value", "error", "status")

    def __init__(self, block_type: BlockType, x: float = 100, y: float = 100,
                 config: Optional[dict] = None, block_id: Optional[str] = None):
        self.id = block_id or uuid.uuid4().hex[:8]
        self.block_type = block_type
        self.x = x
        self.y = y
        self.config = config or dict(BLOCK_DEFS[block_type]["defaults"])
        self.last_value: Any = None
        self.error = ""
        self.status = "idle"

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "type": self.block_type.value,
            "x": self.x,
            "y": self.y,
            "config": dict(self.config),
        }

    @staticmethod
    def from_dict(d: dict) -> "Block":
        bt = BlockType(d["type"])
        return Block(bt, d.get("x", 100), d.get("y", 100),
                     d.get("config", {}), d.get("id"))


class Wire:
    __slots__ = ("id", "from_block", "from_port", "to_block", "to_port")

    def __init__(self, from_block: str, from_port: str,
                 to_block: str, to_port: str, wire_id: Optional[str] = None):
        self.id = wire_id or uuid.uuid4().hex[:8]
        self.from_block = from_block
        self.from_port = from_port
        self.to_block = to_block
        self.to_port = to_port

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "from_block": self.from_block,
            "from_port": self.from_port,
            "to_block": self.to_block,
            "to_port": self.to_port,
        }

    @staticmethod
    def from_dict(d: dict) -> "Wire":
        return Wire(d["from_block"], d["from_port"],
                    d["to_block"], d["to_port"], d.get("id"))


class Rule:
    def __init__(self, name: str = "New Rule", rule_id: Optional[str] = None):
        self.id = rule_id or uuid.uuid4().hex[:8]
        self.name = name
        self.enabled = True
        self.deployed = False  # Whether rule has been deployed to firmware
        self.blocks: list[Block] = []
        self.wires: list[Wire] = []
        self.eval_interval = 5.0
        self.action_gap = 10.0
        self.last_eval = 0.0
        self.status = "idle"
        self.error = ""

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "name": self.name,
            "enabled": self.enabled,
            "deployed": self.deployed,
            "eval_interval": self.eval_interval,
            "action_gap": self.action_gap,
            "blocks": [b.to_dict() for b in self.blocks],
            "wires": [w.to_dict() for w in self.wires],
        }

    @staticmethod
    def from_dict(d: dict) -> "Rule":
        r = Rule(d.get("name", "Rule"), d.get("id"))
        r.enabled = d.get("enabled", True)
        r.deployed = d.get("deployed", False)
        r.eval_interval = d.get("eval_interval", 5.0)
        r.action_gap = d.get("action_gap", d.get("cooldown", 10.0))
        r.blocks = [Block.from_dict(b) for b in d.get("blocks", [])]
        r.wires = [Wire.from_dict(w) for w in d.get("wires", [])]
        return r

    def add_block(self, block_type: BlockType, x: float = 100, y: float = 100) -> Block:
        b = Block(block_type, x, y)
        self.blocks.append(b)
        return b

    def add_wire(self, from_block: str, from_port: str,
                 to_block: str, to_port: str) -> Optional[Wire]:
        # Prevent duplicate wires to same input port
        for w in self.wires:
            if w.to_block == to_block and w.to_port == to_port:
                return None
        w = Wire(from_block, from_port, to_block, to_port)
        self.wires.append(w)
        return w

    def remove_block(self, block_id: str):
        self.blocks = [b for b in self.blocks if b.id != block_id]
        self.wires = [w for w in self.wires
                      if w.from_block != block_id and w.to_block != block_id]

    def remove_wire(self, wire_id: str):
        self.wires = [w for w in self.wires if w.id != wire_id]

    def get_block(self, block_id: str) -> Optional[Block]:
        for b in self.blocks:
            if b.id == block_id:
                return b
        return None


# ─── Automation Engine ────────────────────────────────────────

class AutomationEngine:
    """Evaluates automation rules using sensor data and fires actions via serial."""

    CMD_SPACING = 3.0         # Minimum seconds between successive LoRa commands
    CMD_QUEUE_MAX = 20        # Max pending commands — oldest dropped if full
    MAX_CMDS_PER_MINUTE = 12  # Hard safety cap per minute
    STALE_THRESHOLD = 300.0   # 5 minutes

    def __init__(self, sensor_data: dict, send_serial: Callable[[str], None],
                 discovered_nodes: dict):
        self.sensor_data = sensor_data
        self.send_serial = send_serial
        self.discovered_nodes = discovered_nodes
        self.local_node_id = ""  # Set by gateway GUI after connecting
        self.beacon_data: dict[str, dict] = {}  # beacon_name → {timestamp, rssi, triggered}
        self.deployed_beacons: set[str] = set()  # beacon IDs that have been deployed
        self.rules: list[Rule] = []
        self.rules_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rules.json")

        # Runtime state per block (keyed by block id)
        self._block_state: dict[str, dict] = {}
        # Event log: (timestamp, rule_name, message)
        self.event_log: list[tuple[float, str, str]] = []
        # Command queue: (serial_cmd, rule_name, log_msg, block)
        self._cmd_queue: list[tuple[str, str, str, Block]] = []
        self._last_cmd_time = 0.0
        # Per-minute rate tracking
        self._cmd_timestamps: list[float] = []
        # Auto-poll tracking
        self._last_poll_time: dict[str, float] = {}

        self.load_rules()

    # ─── Persistence ──────────────────────────────────────────

    def save_rules(self):
        data = {
            "version": 1,
            "rules": [r.to_dict() for r in self.rules],
        }
        try:
            with open(self.rules_path, "w") as f:
                json.dump(data, f, indent=2)
        except Exception:
            pass

    def load_rules(self):
        if not os.path.exists(self.rules_path):
            return
        try:
            with open(self.rules_path) as f:
                data = json.load(f)
            self.rules = [Rule.from_dict(r) for r in data.get("rules", [])]
        except Exception:
            self.rules = []

    # ─── Rule Management ──────────────────────────────────────

    def create_rule(self, name: str = "New Rule") -> Rule:
        r = Rule(name)
        self.rules.append(r)
        self.save_rules()
        return r

    def delete_rule(self, rule_id: str):
        self.rules = [r for r in self.rules if r.id != rule_id]
        # Clean up runtime state
        self._block_state = {k: v for k, v in self._block_state.items()
                             if not any(k.startswith(b.id)
                                       for r in self.rules for b in r.blocks
                                       if b.id == k)}
        self.save_rules()

    def get_rule(self, rule_id: str) -> Optional[Rule]:
        for r in self.rules:
            if r.id == rule_id:
                return r
        return None

    # ─── Evaluation ───────────────────────────────────────────

    def evaluate_all(self):
        """Called by GUI timer every 1 second."""
        now = time.time()
        # Prune old command timestamps
        self._cmd_timestamps = [t for t in self._cmd_timestamps if now - t < 60]

        for rule in self.rules:
            if not rule.enabled:
                rule.status = "disabled"
                continue
            if now - rule.last_eval < rule.eval_interval:
                continue
            rule.last_eval = now
            try:
                self._evaluate_rule(rule, now)
            except Exception as e:
                rule.status = "error"
                rule.error = str(e)

        # Drain one command from the queue if spacing allows
        self._drain_queue(now)

        self._ensure_data_freshness(now)

    def _evaluate_rule(self, rule: Rule, now: float):
        block_map = {b.id: b for b in rule.blocks}
        # Build incoming wire map: to_block -> [(from_block, from_port, to_port)]
        wire_map: dict[str, list[tuple[str, str, str]]] = defaultdict(list)
        for w in rule.wires:
            wire_map[w.to_block].append((w.from_block, w.from_port, w.to_port))

        sorted_ids = self._topo_sort(rule)
        values: dict[tuple[str, str], Any] = {}  # (block_id, port_name) -> value

        any_triggered = False
        for bid in sorted_ids:
            block = block_map.get(bid)
            if not block:
                continue
            # Gather inputs from wires
            inputs = {}
            for (src_id, src_port, dst_port) in wire_map.get(bid, []):
                inputs[dst_port] = values.get((src_id, src_port))
            # Evaluate
            outputs = self._eval_block(block, inputs, now, rule)
            for port_name, val in outputs.items():
                values[(bid, port_name)] = val
            block.last_value = outputs.get(
                list(outputs.keys())[0]) if outputs else None

            bdef = BLOCK_DEFS.get(block.block_type, {})
            if bdef.get("category") == "action" and inputs.get("trigger"):
                any_triggered = True

        rule.status = "triggered" if any_triggered else "active"
        rule.error = ""

    def _topo_sort(self, rule: Rule) -> list[str]:
        """Topological sort of blocks by wire dependencies."""
        in_degree: dict[str, int] = {b.id: 0 for b in rule.blocks}
        adj: dict[str, list[str]] = {b.id: [] for b in rule.blocks}
        for w in rule.wires:
            if w.from_block in adj and w.to_block in in_degree:
                adj[w.from_block].append(w.to_block)
                in_degree[w.to_block] += 1
        # Kahn's algorithm
        queue = [bid for bid, deg in in_degree.items() if deg == 0]
        result = []
        while queue:
            bid = queue.pop(0)
            result.append(bid)
            for nxt in adj.get(bid, []):
                in_degree[nxt] -= 1
                if in_degree[nxt] == 0:
                    queue.append(nxt)
        # Append any remaining (cycles) — they won't evaluate cleanly but won't crash
        for b in rule.blocks:
            if b.id not in result:
                result.append(b.id)
        return result

    def _eval_block(self, block: Block, inputs: dict, now: float,
                    rule: Rule) -> dict[str, Any]:
        bt = block.block_type
        cfg = block.config
        sk = block.id  # state key

        # ── Inputs ────────────────────────────────────────────
        if bt == BlockType.SENSOR_READ:
            node_id = cfg.get("node_id", "")
            pin = cfg.get("pin", 0)
            key = f"{node_id}:{pin}"
            readings = self.sensor_data.get(key, [])
            if readings:
                val = readings[-1][1]
                age = now - readings[-1][0]
                if age > self.STALE_THRESHOLD:
                    block.error = f"Stale ({int(age)}s)"
                    block.status = "stale"
                else:
                    block.error = ""
                    block.status = "active"
                return {"value": val}
            block.error = "No data"
            block.status = "stale"
            return {"value": None}

        if bt == BlockType.BEACON_DETECT:
            beacon_id = cfg.get("beacon_id", "")
            beacon_name = cfg.get("name", "").strip()
            rssi_thresh = cfg.get("rssi_thresh", -70)
            node_id = cfg.get("node_id", "")
            if not beacon_id:
                block.error = "No beacon configured"
                return {"detected": False, "rssi": 0}
            # beacon_data is keyed by NAME (firmware sends BEACON,TRIGGERED,<name>,...)
            entry = self.beacon_data.get(beacon_name, {})
            if not entry:
                if beacon_name in self.deployed_beacons:
                    block.error = "Listening..."
                    block.status = "active"
                else:
                    block.error = "Not deployed — click Deploy"
                    block.status = "idle"
                return {"detected": False, "rssi": 0}
            ts = entry.get("timestamp", 0)
            triggered = entry.get("triggered", False)
            source_node = entry.get("source_node", "")
            age = now - ts if ts else 999
            # If node specified, only match events from that node
            if node_id and source_node and source_node != node_id:
                block.error = f"Waiting for {node_id}"
                block.status = "active"
                return {"detected": False, "rssi": 0}
            if triggered and age < 15:
                block.error = ""
                block.status = "triggered"
                return {"detected": True, "rssi": entry.get("rssi", 0)}
            block.error = "Listening..." if age < 60 else "Not seen"
            block.status = "active"
            return {"detected": False, "rssi": 0}

        if bt == BlockType.CONSTANT:
            block.status = "active"
            block.error = ""
            return {"value": cfg.get("value", 0)}

        # ── Transforms ────────────────────────────────────────
        if bt == BlockType.SCALE:
            v = inputs.get("in")
            if v is None:
                return {"out": None}
            block.status = "active"
            return {"out": v * cfg.get("factor", 1.0) + cfg.get("offset", 0.0)}

        if bt == BlockType.MOVING_AVG:
            v = inputs.get("in")
            if v is None:
                return {"out": None}
            buf = self._block_state.setdefault(sk, {"buf": []})["buf"]
            buf.append(v)
            window = max(1, int(cfg.get("window", 5)))
            if len(buf) > window:
                buf[:] = buf[-window:]
            block.status = "active"
            return {"out": sum(buf) / len(buf)}

        if bt == BlockType.DELTA_RATE:
            v = inputs.get("in")
            prev = self._block_state.get(sk, {})
            if v is not None and prev.get("value") is not None:
                dt = now - prev.get("time", now)
                if dt > 0.1:
                    rate = (v - prev["value"]) / dt
                    self._block_state[sk] = {"value": v, "time": now}
                    block.status = "active"
                    return {"out": round(rate, 4)}
            self._block_state[sk] = {"value": v, "time": now}
            return {"out": None}

        # ── Conditions ────────────────────────────────────────
        if bt == BlockType.COMPARE:
            a, b = inputs.get("a"), inputs.get("b")
            if a is None or b is None:
                block.status = "idle"
                block.error = f"Missing: {'a' if a is None else ''} {'b' if b is None else ''}"
                return {"result": None}
            op = cfg.get("operator", "GT")
            ops = {
                "GT": a > b, "LT": a < b, "EQ": a == b,
                "NE": a != b, "GE": a >= b, "LE": a <= b,
            }
            result = ops.get(op, False)
            block.status = "triggered" if result else "active"
            block.error = ""
            return {"result": result}

        if bt == BlockType.AND_GATE:
            vals = [inputs.get(k) for k in ("in1", "in2", "in3", "in4") if k in inputs]
            if not vals or any(v is None for v in vals):
                return {"result": None}
            block.status = "active"
            return {"result": all(vals)}

        if bt == BlockType.OR_GATE:
            vals = [inputs.get(k) for k in ("in1", "in2", "in3", "in4") if k in inputs]
            if not vals or any(v is None for v in vals):
                return {"result": None}
            block.status = "active"
            return {"result": any(vals)}

        if bt == BlockType.NOT_GATE:
            v = inputs.get("in")
            if v is None:
                return {"result": None}
            block.status = "active"
            return {"result": not v}

        if bt == BlockType.DEBOUNCE:
            v = inputs.get("in")
            hold = cfg.get("hold_seconds", 5.0)
            st = self._block_state.setdefault(sk, {"since": 0, "output": False})
            if v:
                if st["since"] == 0:
                    st["since"] = now
                if now - st["since"] >= hold:
                    st["output"] = True
            else:
                st["since"] = 0
                st["output"] = False
            block.status = "active"
            return {"result": st["output"]}

        if bt == BlockType.LATCH:
            s = inputs.get("set", False)
            r = inputs.get("reset", False)
            st = self._block_state.setdefault(sk, {"q": False})
            if r:
                st["q"] = False
            if s:
                st["q"] = True
            block.status = "active"
            return {"q": st["q"]}

        # ── Actions ───────────────────────────────────────────
        if bt == BlockType.SET_RELAY:
            return self._eval_action_relay(block, inputs, now, rule, pulse=False)

        if bt == BlockType.PULSE_RELAY:
            return self._eval_action_relay(block, inputs, now, rule, pulse=True)

        if bt == BlockType.SEND_BROADCAST:
            return self._eval_action_message(block, inputs, now, rule, broadcast=True)

        if bt == BlockType.SEND_DIRECT:
            return self._eval_action_message(block, inputs, now, rule, broadcast=False)

        if bt == BlockType.TELEGRAM_OUTPUT:
            return self._eval_action_telegram(block, inputs, now, rule)

        return {}

    def _eval_action_relay(self, block: Block, inputs: dict, now: float,
                           rule: Rule, pulse: bool) -> dict:
        trigger = inputs.get("trigger")
        if trigger is None:
            block.status = "idle"
            return {}
        st = self._block_state.setdefault(block.id, {"prev": None, "last_fire": 0.0})
        # Rising-edge + action gap
        if trigger and not st["prev"] and (now - st["last_fire"] > rule.action_gap):
            cfg = block.config
            node = cfg.get("node_id", "")
            pin = cfg.get("pin", 0)
            if pulse:
                dur = cfg.get("duration_ms", 500)
                cmd = f"MSG,{node},CMD,PULSE,{pin},{dur}"
                log_msg = f"PULSE {node}:pin{pin} {dur}ms"
            else:
                action = cfg.get("action", 1)
                cmd = f"MSG,{node},CMD,SET,{pin},{action}"
                log_msg = f"SET {node}:pin{pin}={'HIGH' if action else 'LOW'}"
            if self._enqueue_cmd(cmd, rule.name, log_msg, block):
                st["last_fire"] = now
                block.status = "queued"
            else:
                block.error = "Queue full"
        st["prev"] = bool(trigger)
        if not trigger:
            block.status = "active"
        return {}

    def _eval_action_message(self, block: Block, inputs: dict, now: float,
                             rule: Rule, broadcast: bool) -> dict:
        trigger = inputs.get("trigger")
        if trigger is None:
            block.status = "idle"
            return {}
        st = self._block_state.setdefault(block.id, {"prev": None, "last_fire": 0.0})
        cfg = block.config
        # Fire on state change (both edges)
        if trigger != st["prev"] and (now - st["last_fire"] > rule.action_gap):
            msg = cfg.get("message_true", "") if trigger else cfg.get("message_false", "")
            if msg:
                if broadcast:
                    cmd = f"MSG,FFFF,{msg}"
                    log_msg = f"BROADCAST: {msg}"
                else:
                    node = cfg.get("node_id", "")
                    cmd = f"MSG,{node},{msg}"
                    log_msg = f"MSG to {node}: {msg}"
                if self._enqueue_cmd(cmd, rule.name, log_msg, block):
                    st["last_fire"] = now
                    block.status = "queued"
                else:
                    block.error = "Queue full"
        st["prev"] = bool(trigger)
        if not trigger:
            block.status = "active"
        return {}

    def _eval_action_telegram(self, block: Block, inputs: dict, now: float,
                              rule: Rule) -> dict:
        """Telegram Output block — stores the incoming value for bot queries.
        The value is whatever is wired to the 'value' input port.
        When a user sends the rule's #name in Telegram, the bot reads
        block.last_value and formats it with label/unit from config."""
        val = inputs.get("value")
        cfg = block.config
        if val is not None:
            label = cfg.get("label", "")
            unit = cfg.get("unit", "")
            fmt = cfg.get("format", "{label}: {value} {unit}").strip()
            try:
                if isinstance(val, float):
                    formatted = fmt.format(label=label, value=f"{val:.1f}", unit=unit).strip()
                else:
                    formatted = fmt.format(label=label, value=val, unit=unit).strip()
            except (KeyError, ValueError):
                formatted = f"{label}: {val} {unit}".strip()
            block.last_value = {"value": val, "formatted": formatted}
            block.status = "active"
            block.error = ""
        else:
            block.status = "idle"
            block.error = "No input"
        return {}

    # ─── Command Queue ────────────────────────────────────────

    def _enqueue_cmd(self, cmd: str, rule_name: str, log_msg: str,
                     block: Block) -> bool:
        """Add a command to the send queue. Returns False if queue is full."""
        if len(self._cmd_queue) >= self.CMD_QUEUE_MAX:
            self._log(rule_name, f"DROPPED (queue full): {log_msg}")
            return False
        self._cmd_queue.append((cmd, rule_name, log_msg, block))
        pending = len(self._cmd_queue)
        if pending > 1:
            self._log(rule_name, f"QUEUED ({pending} pending): {log_msg}")
        return True

    def _drain_queue(self, now: float):
        """Send the next queued command if enough time has elapsed."""
        if not self._cmd_queue:
            return
        # Enforce spacing between commands
        if now - self._last_cmd_time < self.CMD_SPACING:
            return
        # Enforce per-minute hard cap
        recent = [t for t in self._cmd_timestamps if now - t < 60]
        if len(recent) >= self.MAX_CMDS_PER_MINUTE:
            return

        cmd, rule_name, log_msg, block = self._cmd_queue.pop(0)
        self.send_serial(cmd)
        self._last_cmd_time = now
        self._cmd_timestamps.append(now)
        block.status = "triggered"
        self._log(rule_name, log_msg)

    @property
    def queue_depth(self) -> int:
        """Number of commands waiting to be sent."""
        return len(self._cmd_queue)

    def _ensure_data_freshness(self, now: float):
        """Auto-poll nodes referenced by active rules at the rule's eval_interval."""
        # Find the fastest eval_interval for each node across all enabled rules
        needed: dict[str, float] = {}  # node_id -> shortest eval_interval
        for rule in self.rules:
            if not rule.enabled:
                continue
            for block in rule.blocks:
                if block.block_type == BlockType.SENSOR_READ:
                    nid = block.config.get("node_id", "")
                    if nid:
                        current = needed.get(nid, 999.0)
                        needed[nid] = min(current, rule.eval_interval)

        for node_id, interval in needed.items():
            last_poll = self._last_poll_time.get(node_id, 0)
            if now - last_poll > interval:
                if node_id == self.local_node_id:
                    self._log("Engine", f"POLL local {node_id}")
                    self.send_serial("POLL")
                else:
                    self._log("Engine", f"POLL remote {node_id}")
                    self.send_serial(f"POLL,{node_id}")
                self._last_poll_time[node_id] = now

        if not needed:
            pass

    def _log(self, rule_name: str, message: str):
        self.event_log.append((time.time(), rule_name, message))
        if len(self.event_log) > 200:
            self.event_log = self.event_log[-200:]

    # ─── Deploy to Node (firmware setpoint) ───────────────────

    def try_deploy_as_setpoint(self, rule: Rule) -> tuple[bool, str]:
        """Try to convert a rule to a firmware setpoint or beacon command.
        Supports:
          sensor [> scale] > compare > [debounce >] relay/broadcast
          beacon_detect > relay/broadcast
        """
        # Check for beacon rules first
        beacons = [b for b in rule.blocks if b.block_type == BlockType.BEACON_DETECT]
        if beacons:
            return self._deploy_beacon_rule(rule, beacons[0])

        sensors = [b for b in rule.blocks if b.block_type == BlockType.SENSOR_READ]
        compares = [b for b in rule.blocks if b.block_type == BlockType.COMPARE]
        relays = [b for b in rule.blocks if b.block_type == BlockType.SET_RELAY]
        broadcasts = [b for b in rule.blocks if b.block_type == BlockType.SEND_BROADCAST]
        constants = [b for b in rule.blocks if b.block_type == BlockType.CONSTANT]
        scales = [b for b in rule.blocks if b.block_type == BlockType.SCALE]
        debounces = [b for b in rule.blocks if b.block_type == BlockType.DEBOUNCE]

        if len(sensors) != 1 or len(compares) != 1:
            return False, "Need exactly 1 sensor and 1 compare block"
        if len(relays) + len(broadcasts) != 1:
            return False, "Need exactly 1 action (relay or broadcast)"
        if len(scales) > 1:
            return False, "Max 1 Scale block for firmware deploy"
        if len(debounces) > 1:
            return False, "Max 1 Debounce block for firmware deploy"

        # Check for unsupported block types
        allowed = {BlockType.SENSOR_READ, BlockType.COMPARE, BlockType.CONSTANT,
                   BlockType.SET_RELAY, BlockType.SEND_BROADCAST,
                   BlockType.SCALE, BlockType.DEBOUNCE}
        for b in rule.blocks:
            if b.block_type not in allowed:
                return False, f"Block type '{BLOCK_DEFS[b.block_type]['label']}' cannot deploy to node"

        op = compares[0].config.get("operator", "")
        valid_ops = {"GT", "LT", "EQ", "GE", "LE", "NE"}
        if op not in valid_ops:
            return False, f"Unknown operator: {op}"

        sensor_pin = sensors[0].config.get("pin", 0)
        threshold = constants[0].config.get("value", 0) if constants else 0

        # Build action part
        if relays:
            target_node = relays[0].config.get("node_id", "")
            relay_pin = relays[0].config.get("pin", 0)
            action = relays[0].config.get("action", 1)
            action_part = f"{target_node},{relay_pin},{action}"
        else:
            msg_true = broadcasts[0].config.get("message_true", "")
            msg_false = broadcasts[0].config.get("message_false", "")
            if not msg_true:
                return False, "Broadcast block needs a message"
            action_part = f"MSG,{msg_true}"
            if msg_false:
                action_part += f",{msg_false}"

        cmd = f"SETPOINT,{sensor_pin},{op},{threshold},{action_part}"

        # Append SCALE suffix if present
        if scales:
            factor = scales[0].config.get("factor", 1.0)
            offset = scales[0].config.get("offset", 0.0)
            if factor != 1.0 or offset != 0.0:
                cmd += f",SCALE,{factor},{offset}"

        # Append DEBOUNCE suffix if present
        if debounces:
            hold_s = debounces[0].config.get("hold_seconds", 0)
            if hold_s > 0:
                cmd += f",DEBOUNCE,{int(hold_s * 1000)}"

        self.send_serial(cmd)
        rule.deployed = True
        self._save_rules()
        return True, f"Deployed to node: {cmd}"

    def _deploy_beacon_rule(self, rule: Rule, beacon_block: 'Block') -> tuple[bool, str]:
        """Deploy a beacon detection rule to the target node."""
        cfg = beacon_block.config
        beacon_id = cfg.get("beacon_id", "").strip()
        name = cfg.get("name", "beacon").strip() or "beacon"
        rssi_thresh = cfg.get("rssi_thresh", -70)
        node_id = cfg.get("node_id", "").strip()

        if not beacon_id:
            return False, "Beacon ID (MAC) is required"

        # Determine action from connected blocks
        relays = [b for b in rule.blocks if b.block_type == BlockType.SET_RELAY]
        broadcasts = [b for b in rule.blocks if b.block_type == BlockType.SEND_BROADCAST]

        if relays:
            relay_pin = relays[0].config.get("pin", 2)
            action = relays[0].config.get("action", 1)
            action_part = f"RELAY,{relay_pin},{action}"
        elif broadcasts:
            msg = broadcasts[0].config.get("message_true", "BEACON_NEAR")
            action_part = f"MSG,{msg}"
        else:
            return False, "Need a relay or broadcast action block"

        cmd = f"BEACON,ADD,{beacon_id},{name},{rssi_thresh},{action_part}"

        # Send to target node (local or remote)
        is_local = not node_id or node_id == self.local_node_id
        if is_local:
            self.send_serial(cmd)
        else:
            self.send_serial(f"MSG,{node_id},{cmd}")

        self.deployed_beacons.add(name)
        rule.deployed = True
        self._save_rules()
        self._log("Deploy", f"BEACON,ADD sent: {name} ({beacon_id})")
        target = "local" if is_local else node_id
        return True, f"Beacon rule deployed to {target}: {name}"

    def undeploy_rule(self, rule: Rule) -> tuple[bool, str]:
        """Remove a deployed rule from the firmware."""
        if not rule.deployed:
            return False, "Rule is not deployed"

        # Check for beacon rules — send BEACON,CLEAR to remove all
        beacons = [b for b in rule.blocks if b.block_type == BlockType.BEACON_DETECT]
        if beacons:
            name = beacons[0].config.get("name", "")
            self.send_serial("BEACON,CLEAR")
            self.deployed_beacons.discard(name)
            self._log("Undeploy", f"BEACON,CLEAR sent (removed: {name})")
        else:
            # For setpoint rules, send SETPOINT,CLEAR
            self.send_serial("SETPOINT,CLEAR")
            self._log("Undeploy", f"SETPOINT,CLEAR sent")

        rule.deployed = False
        self._save_rules()
        return True, "Rule undeployed from node"

    # ─── Wire Value Access (for canvas live display) ──────────

    def get_wire_values(self, rule: Rule) -> dict[str, Any]:
        """Return current output values for all blocks: {block_id:port -> value}"""
        result = {}
        for block in rule.blocks:
            if block.last_value is not None:
                bdef = BLOCK_DEFS.get(block.block_type, {})
                outs = bdef.get("outputs", [])
                if outs:
                    result[f"{block.id}:{outs[0][0]}"] = block.last_value
        return result
