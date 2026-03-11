# LoRa Mesh Communicator

**Off-grid encrypted mesh communication — no internet, no infrastructure, no subscription.**

Send messages, trigger relays, monitor sensors and send SOS alerts across kilometres of terrain using cheap LoRa hardware. Works in forests, mountains, farms, and disaster zones.

---

## ⚡ Quick Start — Flash in 60 Seconds

**No software to install.** Just plug in your board and open Chrome:

👉 **[Click here to flash your device](https://YOUR_USERNAME.github.io/Tiggy_Lora_Mesh/flash)**

Supported boards:
| Board | Price | Best for |
|---|---|---|
| Seeed XIAO ESP32S3 + Wio-SX1262 | ~£10 | Ultra-cheap repeater |
| Heltec WiFi LoRa 32 V3 | ~£18 | Repeater with OLED status |
| Heltec WiFi LoRa 32 V4 | ~£20 | Best repeater (28dBm, solar input) |
| LilyGO T-Deck Plus | ~£55 | Full communicator with keyboard |

---

## 📱 Android App

Download the latest APK from the [Releases page](../../releases/latest) — no Play Store needed.

Connect via Bluetooth to any mesh node to send messages, view the mesh, and set up Telegram alerts.

---

## 🗺️ How It Works

```
[Device A] ──LoRa──► [Repeater] ──LoRa──► [Device B]
                          │
                     [Gateway Pi]
                          │
                     [Hub Server]
                          │
                    [Telegram Bot] ──► [Family's phones]
```

- **AES-128 encrypted** — end to end, hub never sees plaintext
- **Hybrid mesh routing** — RSSI-weighted directed routing with flood fallback
- **SOS beacon** — broadcasts GPS coordinates every 2 minutes to all nodes + Telegram
- **Telegram alerts** — group messages forwarded from your phone when you have signal
- **Solar repeaters** — Heltec V3/V4 can run indefinitely from a 1W solar panel

---

## 🔧 What You Need

**Minimum (2 nodes to talk to each other):**
- 2× any supported board (~£20 total for 2× XIAO + Wio-SX1262)
- 1× Android phone with the app

**For gateway/Telegram bridging:**
- 1× Raspberry Pi (any model) or always-on PC
- The gateway Python app (Windows .exe in Releases, or run from source)

---

## 📖 Full Documentation

**[Read the full manual →](docs/documentation.html)**

Covers: setup, serial commands, EEPROM config, mesh routing, solar mode, Telegram setup, gateway hub, hardware wiring.

---

## 🛠️ Build from Source

Requires [PlatformIO](https://platformio.org/).

```bash
git clone https://github.com/YOUR_USERNAME/Tiggy_Lora_Mesh
cd Tiggy_Lora_Mesh

# Flash a XIAO ESP32S3 repeater
pio run -e xiao-s3 --target upload

# Flash a Heltec V4 repeater
pio run -e heltec-v4 --target upload

# Flash a T-Deck Plus communicator
pio run -e tdeck-plus --target upload
```

Build the Android app in Android Studio (open `android-app/` folder).

Build the Windows gateway .exe:
```batch
cd gateway
build.bat
```

---

## 📡 Supported Hardware

### XIAO ESP32S3 + Wio-SX1262 — cheapest option
```
XIAO ESP32S3 pinout:
┌─────────────────────┐
│      [USB-C]        │
│ D0/GPIO1  ●   ● 5V  │
│ D1/GPIO2  ●   ● GND │
│ D2/GPIO3  ●   ● 3V3 │
│ D3/GPIO4  ●   ● D10/GPIO9  ← MOSI │
│ D4/GPIO5  ●   ● D9/GPIO8   ← MISO │
│ D5/GPIO6  ●   ● D8/GPIO7   ← SCK  │
│ D6/GPIO43 ●   ● D7/GPIO44        │
└─────────────────────┘

Wio-SX1262 internal connections (routed on expansion PCB):
  SCK=GPIO7  MISO=GPIO8  MOSI=GPIO9
  CS=GPIO41  RST=GPIO42  DIO1=GPIO39  BUSY=GPIO40  RXEN=GPIO38
```

The XIAO + Wio-SX1262 is a plug-together kit — no soldering required for the LoRa part.

---

## 🌍 Community

- **Issues / bugs:** [GitHub Issues](../../issues)
- **Discussions:** [GitHub Discussions](../../discussions)

---

## 📄 Licence

MIT — free for personal and commercial use. If you build something cool with it, let us know!
