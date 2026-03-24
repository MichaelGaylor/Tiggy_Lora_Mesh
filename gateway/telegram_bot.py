"""
TiggyOpenMesh — Telegram Bot Bridge
════════════════════════════════════
Bridges the automation engine to a Telegram chat.

Rules whose names start with '#' become Telegram commands:
  - Sensor rules (e.g. #getwind) → read latest value, apply Scale, reply
  - Action rules (e.g. #opendoor) → fire the relay action via send_serial

Built-in commands:
  #status  — list all nodes with online/offline, RSSI, last seen
  #help    — list available # commands
  #nodes   — list discovered nodes with friendly names

Runs in its own thread with a private asyncio event loop so it never
blocks the tkinter GUI.
"""

import asyncio
import logging
import threading
import time
from typing import Any, Callable, Optional

from telegram import Update
from telegram.ext import (
    Application,
    CommandHandler,
    MessageHandler,
    ContextTypes,
    filters,
)

from automation_engine import BlockType, Block, Rule

logger = logging.getLogger(__name__)


class TelegramBridge:
    """Async Telegram bot that exposes automation-engine rules as chat commands."""

    # ── Constructor ────────────────────────────────────────────

    def __init__(
        self,
        token: str,
        chat_id: str,
        sensor_data: dict,
        send_serial: Callable[[str], None],
        rules: list,
        node_names: dict,
        engine: Any,
    ):
        self.token = token
        self.chat_id = str(chat_id)
        self.sensor_data = sensor_data
        self.send_serial = send_serial
        self.rules = rules
        self.node_names = node_names  # {"5041": "Farm1", ...}
        self.engine = engine

        self._app: Optional[Application] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._running = False

    # ── Properties ─────────────────────────────────────────────

    @property
    def is_running(self) -> bool:
        return self._running

    # ── Lifecycle ──────────────────────────────────────────────

    def start(self):
        """Start the bot in a background daemon thread."""
        if self._running:
            logger.warning("TelegramBridge.start() called but already running")
            return
        self._thread = threading.Thread(target=self._run_loop, daemon=True,
                                        name="TelegramBot")
        self._thread.start()

    def stop(self):
        """Cleanly shut down the bot and its event loop."""
        if not self._running:
            return
        self._running = False
        if self._loop and self._app:
            # Schedule the shutdown coroutine on the bot's own loop
            future = asyncio.run_coroutine_threadsafe(
                self._shutdown(), self._loop
            )
            try:
                future.result(timeout=10)
            except Exception:
                logger.exception("Error during TelegramBridge shutdown")
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5)
        self._thread = None
        logger.info("TelegramBridge stopped")

    # ── Alert sending (called from engine, any thread) ─────────

    async def send_alert(self, message: str):
        """Send an alert message to the configured chat.

        Safe to await from any coroutine.  If called from a synchronous
        context use::

            asyncio.run_coroutine_threadsafe(bridge.send_alert(msg), bridge._loop)
        """
        if not self._app or not self._running:
            logger.warning("send_alert called but bot is not running")
            return
        try:
            await self._app.bot.send_message(
                chat_id=self.chat_id,
                text=message,
                parse_mode=None,  # plain text — safe, no escaping needed
            )
        except Exception:
            logger.exception("Failed to send Telegram alert")

    def send_alert_threadsafe(self, message: str):
        """Fire-and-forget alert from any thread (convenience wrapper)."""
        if self._loop and self._running:
            asyncio.run_coroutine_threadsafe(self.send_alert(message), self._loop)

    # ── Internal: event-loop thread ────────────────────────────

    def _run_loop(self):
        """Entry point for the background thread."""
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._start_bot())
        except Exception:
            logger.exception("TelegramBridge event loop crashed")
        finally:
            self._running = False
            try:
                self._loop.run_until_complete(self._loop.shutdown_asyncgens())
            except Exception:
                pass
            self._loop.close()
            self._loop = None

    async def _start_bot(self):
        """Build the Application, register handlers, and poll."""
        builder = Application.builder().token(self.token)
        self._app = builder.build()

        # Built-in commands
        self._app.add_handler(CommandHandler("help", self._cmd_help))
        self._app.add_handler(CommandHandler("status", self._cmd_status))
        self._app.add_handler(CommandHandler("nodes", self._cmd_nodes))

        # Catch-all for text messages (handles # commands from rules)
        self._app.add_handler(
            MessageHandler(filters.TEXT & ~filters.COMMAND, self._on_text)
        )
        # Also catch /commands that aren't built-in — user might type
        # /getwind instead of #getwind
        self._app.add_handler(
            MessageHandler(filters.COMMAND, self._on_slash_command)
        )

        self._running = True
        logger.info("TelegramBridge starting polling")

        await self._app.initialize()
        await self._app.start()
        await self._app.updater.start_polling(drop_pending_updates=True)

        # Keep running until told to stop
        while self._running:
            await asyncio.sleep(0.5)

    async def _shutdown(self):
        """Gracefully stop polling and shut down the application."""
        if self._app:
            try:
                if self._app.updater and self._app.updater.running:
                    await self._app.updater.stop()
                await self._app.stop()
                await self._app.shutdown()
            except Exception:
                logger.exception("Error in _shutdown sequence")

    # ── Security gate ──────────────────────────────────────────

    def _is_authorised(self, update: Update) -> bool:
        """Return True only if the message is from the allowed chat."""
        if update.effective_chat is None:
            return False
        return str(update.effective_chat.id) == self.chat_id

    # ── Built-in commands ──────────────────────────────────────

    async def _cmd_help(self, update: Update, context: ContextTypes.DEFAULT_TYPE):
        if not self._is_authorised(update):
            return
        lines = [
            "TiggyOpenMesh Gateway Bot",
            "─────────────────────────",
            "",
            "Built-in commands:",
            "  /help    — this message",
            "  /status  — node online/offline overview",
            "  /nodes   — discovered nodes with names",
            "",
        ]

        rule_cmds = self._get_hash_commands()
        if rule_cmds:
            lines.append("Rule commands:")
            for name, rule in rule_cmds:
                desc = self._describe_rule(rule)
                lines.append(f"  {name}  — {desc}")
        else:
            lines.append("No # commands defined in rules yet.")

        await update.message.reply_text("\n".join(lines))

    async def _cmd_status(self, update: Update, context: ContextTypes.DEFAULT_TYPE):
        if not self._is_authorised(update):
            return
        lines = ["Node Status", "───────────", ""]
        now = time.time()
        nodes = getattr(self.engine, "discovered_nodes", None)
        topo = getattr(self.engine, "topo_nodes", None) if nodes is None else None

        if nodes and isinstance(nodes, dict):
            # AutomationEngine.discovered_nodes: {node_id: {...}}
            for nid, info in sorted(nodes.items()):
                lines.append(self._format_node_status(nid, info, now))
        elif topo and isinstance(topo, dict):
            # gateway_gui TopoNode objects
            for nid, node in sorted(topo.items()):
                info = {
                    "rssi": getattr(node, "rssi", -100),
                    "last_seen": getattr(node, "last_seen", 0),
                }
                lines.append(self._format_node_status(nid, info, now))
        else:
            # Fall back to sensor_data keys to infer nodes
            seen_nodes: dict[str, float] = {}
            for key, readings in self.sensor_data.items():
                nid = key.split(":")[0]
                if readings:
                    ts = readings[-1][0]
                    seen_nodes[nid] = max(seen_nodes.get(nid, 0), ts)
            if seen_nodes:
                for nid, last_ts in sorted(seen_nodes.items()):
                    age = now - last_ts
                    status = "ONLINE" if age < 120 else "OFFLINE"
                    name = self.node_names.get(nid, nid)
                    age_str = self._format_age(age)
                    lines.append(f"  {name} ({nid})  {status}  last seen {age_str} ago")
            else:
                lines.append("  No nodes discovered yet.")

        await update.message.reply_text("\n".join(lines))

    async def _cmd_nodes(self, update: Update, context: ContextTypes.DEFAULT_TYPE):
        if not self._is_authorised(update):
            return
        lines = ["Discovered Nodes", "────────────────", ""]

        # Gather node IDs from sensor data keys and any engine structures
        node_ids: set[str] = set()
        for key in self.sensor_data:
            node_ids.add(key.split(":")[0])

        nodes = getattr(self.engine, "discovered_nodes", None)
        if nodes and isinstance(nodes, dict):
            node_ids.update(nodes.keys())

        topo = getattr(self.engine, "topo_nodes", None)
        if topo and isinstance(topo, dict):
            node_ids.update(topo.keys())

        if node_ids:
            for nid in sorted(node_ids):
                friendly = self.node_names.get(nid, "")
                label = f"{friendly} ({nid})" if friendly else nid
                lines.append(f"  {label}")
        else:
            lines.append("  No nodes discovered yet.")

        await update.message.reply_text("\n".join(lines))

    # ── Hash-command dispatch ──────────────────────────────────

    async def _on_text(self, update: Update, context: ContextTypes.DEFAULT_TYPE):
        """Handle plain text messages — look for # commands."""
        if not self._is_authorised(update):
            return
        text = (update.message.text or "").strip()
        if not text.startswith("#"):
            return
        await self._dispatch_hash_command(text, update)

    async def _on_slash_command(self, update: Update, context: ContextTypes.DEFAULT_TYPE):
        """Handle /commands that aren't built-in — map /getwind -> #getwind."""
        if not self._is_authorised(update):
            return
        text = (update.message.text or "").strip()
        # Convert /getwind to #getwind for rule lookup
        if text.startswith("/"):
            hash_text = "#" + text[1:].split("@")[0]  # strip bot username suffix
            await self._dispatch_hash_command(hash_text, update)

    async def _dispatch_hash_command(self, command: str, update: Update):
        """Find the matching rule and execute it."""
        cmd_lower = command.lower()

        # Built-in overrides
        if cmd_lower == "#help":
            await self._cmd_help(update, None)
            return
        if cmd_lower == "#status":
            await self._cmd_status(update, None)
            return
        if cmd_lower == "#nodes":
            await self._cmd_nodes(update, None)
            return

        # Search rules for a matching # command name
        rule = self._find_rule_by_command(cmd_lower)
        if rule is None:
            await update.message.reply_text(
                f"Unknown command: {command}\nType /help or #help to see available commands."
            )
            return

        # Determine what kind of rule this is and respond accordingly
        has_sensor = False
        has_action = False
        for block in rule.blocks:
            cat = self._block_category(block)
            if cat == "input" and block.block_type == BlockType.SENSOR_READ:
                has_sensor = True
            elif cat == "action":
                has_action = True

        if has_sensor and not has_action:
            # Sensor query — read and reply
            reply = self._handle_sensor_query(rule)
            await update.message.reply_text(reply)
        elif has_action:
            # Action rule — execute it
            reply = self._handle_action_command(rule)
            await update.message.reply_text(reply)
        else:
            await update.message.reply_text(
                f"Rule '{rule.name}' matched but has no sensor reads or actions to execute."
            )

    # ── Sensor query handling ──────────────────────────────────

    def _handle_sensor_query(self, rule: Rule) -> str:
        """Read the latest sensor values referenced by this rule, apply
        any Scale transform, and return a formatted string."""
        now = time.time()
        lines = [f"{rule.name}", ""]

        # Build a quick lookup of block id -> block
        block_map = {b.id: b for b in rule.blocks}

        # Build wire map: from_block -> [(to_block, from_port, to_port)]
        wire_out: dict[str, list[tuple[str, str, str]]] = {}
        for w in rule.wires:
            wire_out.setdefault(w.from_block, []).append(
                (w.to_block, w.from_port, w.to_port)
            )

        sensor_blocks = [b for b in rule.blocks
                         if b.block_type == BlockType.SENSOR_READ]

        if not sensor_blocks:
            return f"{rule.name}: no sensor blocks found in this rule."

        for sb in sensor_blocks:
            node_id = sb.config.get("node_id", "")
            pin = sb.config.get("pin", 0)
            label = sb.config.get("label", "")
            key = f"{node_id}:{pin}"
            readings = self.sensor_data.get(key, [])

            friendly = self.node_names.get(node_id, node_id)
            sensor_label = label if label else f"pin {pin}"

            if not readings:
                lines.append(f"  {friendly} {sensor_label}: no data")
                continue

            ts, raw_val = readings[-1]
            age = now - ts

            # Look for a Scale block wired to this sensor's output
            scaled_val = raw_val
            unit = ""
            for to_bid, from_port, to_port in wire_out.get(sb.id, []):
                downstream = block_map.get(to_bid)
                if downstream and downstream.block_type == BlockType.SCALE:
                    factor = downstream.config.get("factor", 1.0)
                    offset = downstream.config.get("offset", 0.0)
                    unit = downstream.config.get("unit", "")
                    try:
                        scaled_val = float(raw_val) * factor + offset
                    except (TypeError, ValueError):
                        scaled_val = raw_val
                    break  # use the first Scale block found

            # Format the value
            if isinstance(scaled_val, float):
                val_str = f"{scaled_val:.2f}"
                # Strip trailing zeroes for cleaner display
                if "." in val_str:
                    val_str = val_str.rstrip("0").rstrip(".")
            else:
                val_str = str(scaled_val)

            if unit:
                val_str = f"{val_str} {unit}"

            age_str = self._format_age(age)
            lines.append(f"  {friendly} {sensor_label}: {val_str}  ({age_str} ago)")

        return "\n".join(lines)

    # ── Action command handling ─────────────────────────────────

    def _handle_action_command(self, rule: Rule) -> str:
        """Execute the relay/message action blocks in a rule."""
        actions_fired = 0

        for block in rule.blocks:
            bt = block.block_type
            cfg = block.config

            if bt == BlockType.SET_RELAY:
                node = cfg.get("node_id", "")
                pin = cfg.get("pin", 0)
                action = cfg.get("action", 1)
                cmd = f"MSG,{node},CMD,SET,{pin},{action}"
                self.send_serial(cmd)
                actions_fired += 1
                friendly = self.node_names.get(node, node)
                state = "HIGH" if action else "LOW"
                logger.info("Telegram cmd: SET %s pin%d=%s", friendly, pin, state)

            elif bt == BlockType.PULSE_RELAY:
                node = cfg.get("node_id", "")
                pin = cfg.get("pin", 0)
                dur = cfg.get("duration_ms", 500)
                cmd = f"MSG,{node},CMD,PULSE,{pin},{dur}"
                self.send_serial(cmd)
                actions_fired += 1
                friendly = self.node_names.get(node, node)
                logger.info("Telegram cmd: PULSE %s pin%d %dms", friendly, pin, dur)

            elif bt == BlockType.SEND_BROADCAST:
                msg = cfg.get("message_true", "")
                if msg:
                    cmd = f"MSG,FFFF,{msg}"
                    self.send_serial(cmd)
                    actions_fired += 1

            elif bt == BlockType.SEND_DIRECT:
                node = cfg.get("node_id", "")
                msg = cfg.get("message_true", "")
                if msg:
                    cmd = f"MSG,{node},{msg}"
                    self.send_serial(cmd)
                    actions_fired += 1

        if actions_fired:
            return f"Done — {rule.name}: {actions_fired} action(s) sent."
        return f"Rule '{rule.name}' matched but no actions could be fired."

    # ── Helpers ─────────────────────────────────────────────────

    def _get_hash_commands(self) -> list[tuple[str, Rule]]:
        """Return a sorted list of (command_name, rule) for rules whose names
        start with '#'."""
        result = []
        for rule in self.rules:
            name = (rule.name or "").strip()
            if name.startswith("#"):
                result.append((name, rule))
        result.sort(key=lambda x: x[0].lower())
        return result

    def _find_rule_by_command(self, cmd_lower: str) -> Optional[Rule]:
        """Find a rule whose name matches the given # command (case-insensitive)."""
        for rule in self.rules:
            name = (rule.name or "").strip()
            if name.lower() == cmd_lower:
                return rule
        return None

    def _describe_rule(self, rule: Rule) -> str:
        """Return a short human description of what a rule does."""
        has_sensor = any(b.block_type == BlockType.SENSOR_READ for b in rule.blocks)
        action_types = set()
        for b in rule.blocks:
            if self._block_category(b) == "action":
                action_types.add(b.block_type.value)

        parts = []
        if has_sensor and not action_types:
            parts.append("read sensor")
        elif action_types:
            for at in sorted(action_types):
                parts.append(at.replace("_", " "))
        return ", ".join(parts) if parts else "rule"

    @staticmethod
    def _block_category(block: Block) -> str:
        """Return the category string for a block ('input', 'transform',
        'condition', 'action')."""
        from automation_engine import BLOCK_DEFS
        defn = BLOCK_DEFS.get(block.block_type, {})
        return defn.get("category", "")

    def _format_node_status(self, node_id: str, info: dict, now: float) -> str:
        """Format a single node's status line."""
        friendly = self.node_names.get(node_id, "")
        label = f"{friendly} ({node_id})" if friendly else node_id
        rssi = info.get("rssi", -100)
        last_seen = info.get("last_seen", 0)
        age = now - last_seen if last_seen else 9999
        online = "ONLINE" if age < 120 else "OFFLINE"
        age_str = self._format_age(age)
        rssi_str = f"RSSI {rssi} dBm" if rssi > -120 else "no signal"
        return f"  {label}  {online}  {rssi_str}  last {age_str} ago"

    @staticmethod
    def _format_age(seconds: float) -> str:
        """Return a human-readable age string like '2m 15s' or '1h 3m'."""
        s = int(seconds)
        if s < 0:
            return "just now"
        if s < 60:
            return f"{s}s"
        if s < 3600:
            m, sec = divmod(s, 60)
            return f"{m}m {sec}s"
        h, remainder = divmod(s, 3600)
        m = remainder // 60
        return f"{h}h {m}m"
