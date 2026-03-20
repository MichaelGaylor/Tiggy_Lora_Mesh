"""
TiggyOpenMesh — Sensor Dashboard
=================================
Full sensor visualization: circular gauges + matplotlib line chart.
Embeds in the gateway GUI as a replaceable panel.
"""

import csv
import math
import os
import time
import tkinter as tk
from datetime import datetime
from typing import Optional

import customtkinter as ctk
import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.dates import DateFormatter

from gui_common import COLORS

# ─── Gauge Widget ────────────────────────────────────────────

class GaugeWidget(tk.Canvas):
    """Circular arc gauge for a single sensor value."""

    def __init__(self, parent, label: str = "", min_val: float = 0, max_val: float = 4095,
                 size: int = 120, **kwargs):
        super().__init__(parent, width=size, height=size + 24,
                         bg=COLORS["panel"], highlightthickness=0, **kwargs)
        self.label = label
        self.min_val = min_val
        self.max_val = max_val
        self.size = size
        self.value: Optional[float] = None
        self.min_seen: Optional[float] = None
        self.max_seen: Optional[float] = None
        self._draw()

    def set_value(self, value: float):
        self.value = value
        if self.min_seen is None or value < self.min_seen:
            self.min_seen = value
        if self.max_seen is None or value > self.max_seen:
            self.max_seen = value
        self._draw()

    def _draw(self):
        self.delete("all")
        s = self.size
        cx, cy = s // 2, s // 2
        r = s // 2 - 8
        arc_start = 225  # degrees (bottom-left)
        arc_span = 270   # degrees (270 = 3/4 circle)

        # Background arc
        self.create_arc(cx - r, cy - r, cx + r, cy + r,
                        start=arc_start - arc_span, extent=arc_span,
                        style="arc", outline=COLORS["faint"], width=8)

        if self.value is not None:
            # Value arc (colored portion)
            frac = max(0, min(1, (self.value - self.min_val) / max(self.max_val - self.min_val, 1)))
            val_span = frac * arc_span
            color = self._frac_to_color(frac)
            if val_span > 1:
                self.create_arc(cx - r, cy - r, cx + r, cy + r,
                                start=arc_start - val_span, extent=val_span,
                                style="arc", outline=color, width=8)

            # Needle
            angle_deg = arc_start - frac * arc_span
            angle_rad = math.radians(angle_deg)
            nx = cx + (r - 16) * math.cos(angle_rad)
            ny = cy - (r - 16) * math.sin(angle_rad)
            self.create_line(cx, cy, nx, ny, fill="#FFFFFF", width=2)
            self.create_oval(cx - 3, cy - 3, cx + 3, cy + 3, fill="#FFFFFF", outline="")

            # Current value (large, center)
            self.create_text(cx, cy + 16, text=str(int(self.value)),
                             fill=color, font=("Consolas", 14, "bold"))

            # Min / Max
            if self.min_seen is not None:
                self.create_text(cx - r + 4, cy + r - 2, text=str(int(self.min_seen)),
                                 fill=COLORS["dim"], font=("Consolas", 7), anchor="sw")
            if self.max_seen is not None:
                self.create_text(cx + r - 4, cy + r - 2, text=str(int(self.max_seen)),
                                 fill=COLORS["dim"], font=("Consolas", 7), anchor="se")

        # Label
        self.create_text(cx, s + 8, text=self.label,
                         fill=COLORS["text"], font=("Consolas", 8))

    @staticmethod
    def _frac_to_color(frac: float) -> str:
        """Map 0..1 fraction to green → yellow → red."""
        if frac < 0.5:
            r = int(255 * frac * 2)
            g = 255
        else:
            r = 255
            g = int(255 * (1 - frac) * 2)
        return f"#{r:02X}{g:02X}00"


# ─── Sensor Dashboard ───────────────────────────────────────

class SensorDashboard(ctk.CTkFrame):
    """Full sensor dashboard with gauges + line chart + toolbar."""

    PALETTE = ["#00E5FF", "#00E676", "#FF9100", "#448AFF", "#FF5252",
               "#FFE000", "#E040FB", "#76FF03", "#FF6E40", "#40C4FF"]

    def __init__(self, parent, sensor_data: dict, max_history: int = 120, **kwargs):
        super().__init__(parent, fg_color=COLORS["bg"], **kwargs)
        self.sensor_data = sensor_data
        self.max_history = max_history
        self.gauges: dict[str, GaugeWidget] = {}
        self.paused = False
        self.hidden_keys: set[str] = set()

        # ── Toolbar ──────────────────────────────────────────
        toolbar = ctk.CTkFrame(self, fg_color="transparent", height=30)
        toolbar.pack(fill="x", padx=8, pady=(4, 0))

        ctk.CTkLabel(toolbar, text="Sensor Dashboard", font=("Consolas", 11, "bold"),
                     text_color=COLORS["accent"]).pack(side="left")

        self.pause_btn = ctk.CTkButton(toolbar, text="Pause", width=60,
                                        font=("Consolas", 9), fg_color=COLORS["faint"],
                                        command=self._toggle_pause)
        self.pause_btn.pack(side="right", padx=4)

        ctk.CTkButton(toolbar, text="Export CSV", width=80, font=("Consolas", 9),
                       fg_color=COLORS["accent"], text_color="#000",
                       command=self._export_csv).pack(side="right", padx=4)

        ctk.CTkButton(toolbar, text="Clear", width=60, font=("Consolas", 9),
                       fg_color=COLORS["faint"],
                       command=self._clear_data).pack(side="right", padx=4)

        # History length selector
        self.history_var = ctk.StringVar(value="120")
        ctk.CTkOptionMenu(toolbar, values=["60", "120", "300", "600"],
                           variable=self.history_var, width=70,
                           font=("Consolas", 9),
                           command=self._on_history_change).pack(side="right", padx=4)
        ctk.CTkLabel(toolbar, text="History:", font=("Consolas", 9),
                     text_color=COLORS["dim"]).pack(side="right")

        # ── Gauge row ────────────────────────────────────────
        self.gauge_frame = ctk.CTkScrollableFrame(self, fg_color=COLORS["panel"],
                                                    height=160, orientation="horizontal")
        self.gauge_frame.pack(fill="x", padx=8, pady=4)

        # ── Matplotlib chart ─────────────────────────────────
        chart_frame = ctk.CTkFrame(self, fg_color=COLORS["panel"])
        chart_frame.pack(fill="both", expand=True, padx=8, pady=(0, 8))

        self.fig, self.ax = plt.subplots(figsize=(8, 3), dpi=100)
        self.fig.patch.set_facecolor(COLORS["panel"])
        self.ax.set_facecolor(COLORS["bg"])
        self.ax.tick_params(colors=COLORS["dim"], labelsize=7)
        self.ax.spines["top"].set_visible(False)
        self.ax.spines["right"].set_visible(False)
        self.ax.spines["bottom"].set_color(COLORS["faint"])
        self.ax.spines["left"].set_color(COLORS["faint"])
        self.ax.xaxis.set_major_formatter(DateFormatter("%H:%M:%S"))
        self.ax.set_ylabel("Value", color=COLORS["dim"], fontsize=8)
        self.fig.tight_layout(pad=1.5)

        self.canvas_widget = FigureCanvasTkAgg(self.fig, master=chart_frame)
        self.canvas_widget.get_tk_widget().pack(fill="both", expand=True)

        # ── Legend click area ────────────────────────────────
        self.legend_frame = ctk.CTkFrame(chart_frame, fg_color="transparent", height=20)
        self.legend_frame.pack(fill="x", padx=4, pady=(0, 2))

        # Start refresh loop
        self._refresh()

    def _refresh(self):
        """Update gauges and chart every 2 seconds."""
        if not self.paused:
            self._update_gauges()
            self._update_chart()
        self.after(2000, self._refresh)

    def _update_gauges(self):
        """Create/update gauge widgets for each sensor."""
        keys = sorted(self.sensor_data.keys())
        for key in keys:
            readings = self.sensor_data.get(key, [])
            if not readings:
                continue
            current_val = readings[-1][1]

            if key not in self.gauges:
                gauge = GaugeWidget(self.gauge_frame, label=key, size=120)
                gauge.pack(side="left", padx=6, pady=4)
                self.gauges[key] = gauge

            self.gauges[key].set_value(current_val)

        # Remove gauges for sensors no longer present
        for key in list(self.gauges.keys()):
            if key not in self.sensor_data:
                self.gauges[key].destroy()
                del self.gauges[key]

    def _update_chart(self):
        """Redraw the matplotlib line chart."""
        self.ax.clear()
        self.ax.set_facecolor(COLORS["bg"])
        self.ax.tick_params(colors=COLORS["dim"], labelsize=7)
        self.ax.spines["top"].set_visible(False)
        self.ax.spines["right"].set_visible(False)
        self.ax.spines["bottom"].set_color(COLORS["faint"])
        self.ax.spines["left"].set_color(COLORS["faint"])
        self.ax.xaxis.set_major_formatter(DateFormatter("%H:%M:%S"))

        keys = sorted(self.sensor_data.keys())
        has_data = False

        for idx, key in enumerate(keys):
            if key in self.hidden_keys:
                continue
            readings = self.sensor_data.get(key, [])
            if len(readings) < 2:
                continue
            has_data = True
            times = [datetime.fromtimestamp(t) for t, _ in readings]
            values = [v for _, v in readings]
            color = self.PALETTE[idx % len(self.PALETTE)]
            self.ax.plot(times, values, color=color, linewidth=1.5,
                         label=key, alpha=0.9)

        if has_data:
            self.ax.legend(loc="upper left", fontsize=7, framealpha=0.3,
                           labelcolor=COLORS["text"], facecolor=COLORS["panel"],
                           edgecolor=COLORS["faint"])
        else:
            self.ax.text(0.5, 0.5, "Waiting for sensor data...",
                         transform=self.ax.transAxes, ha="center", va="center",
                         color=COLORS["dim"], fontsize=10)

        self.fig.tight_layout(pad=1.5)
        self.canvas_widget.draw_idle()

        # Update legend toggles
        self._update_legend(keys)

    def _update_legend(self, keys: list[str]):
        """Update clickable legend labels below the chart."""
        for widget in self.legend_frame.winfo_children():
            widget.destroy()

        for idx, key in enumerate(keys):
            color = self.PALETTE[idx % len(self.PALETTE)]
            hidden = key in self.hidden_keys
            btn = ctk.CTkButton(
                self.legend_frame, text=key, width=80, height=18,
                font=("Consolas", 8),
                fg_color=COLORS["faint"] if hidden else color,
                text_color=COLORS["dim"] if hidden else "#000",
                command=lambda k=key: self._toggle_sensor(k)
            )
            btn.pack(side="left", padx=2)

    def _toggle_sensor(self, key: str):
        if key in self.hidden_keys:
            self.hidden_keys.discard(key)
        else:
            self.hidden_keys.add(key)

    def _toggle_pause(self):
        self.paused = not self.paused
        self.pause_btn.configure(text="Resume" if self.paused else "Pause",
                                  fg_color=COLORS["warn"] if self.paused else COLORS["faint"])

    def _clear_data(self):
        self.sensor_data.clear()
        for gauge in self.gauges.values():
            gauge.destroy()
        self.gauges.clear()

    def _on_history_change(self, val: str):
        self.max_history = int(val)

    def _export_csv(self):
        """Export all sensor history to a timestamped CSV file."""
        export_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "exports")
        os.makedirs(export_dir, exist_ok=True)
        filename = f"sensors_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        filepath = os.path.join(export_dir, filename)

        count = 0
        with open(filepath, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["timestamp", "datetime", "node_id", "pin", "value"])
            for key, readings in sorted(self.sensor_data.items()):
                parts = key.split(":")
                node_id = parts[0] if len(parts) > 1 else "?"
                pin = parts[1] if len(parts) > 1 else key
                for ts, val in readings:
                    writer.writerow([
                        f"{ts:.3f}",
                        datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S"),
                        node_id, pin, val
                    ])
                    count += 1

        # Show feedback
        self.pause_btn.configure(text=f"Exported {count} rows", fg_color=COLORS["good"])
        self.after(3000, lambda: self.pause_btn.configure(
            text="Resume" if self.paused else "Pause",
            fg_color=COLORS["warn"] if self.paused else COLORS["faint"]))
