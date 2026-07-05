// ═══════════════════════════════════════════════════════════════
// Board Pin Definitions
// ═══════════════════════════════════════════════════════════════
//
// Select your board by uncommenting ONE of these defines
// in platformio.ini build_flags, e.g.:
//   -DBOARD_TDECK_PLUS
//   -DBOARD_LORA32
//
// Or define your own custom board at the bottom.
// ═══════════════════════════════════════════════════════════════
#pragma once

// ─── Board capabilities (set per board below) ───────────────
// HAS_DISPLAY          - has a graphical TFT display
// HAS_KEYBOARD         - has I2C keyboard
// HAS_TRACKBALL        - has trackball input
// HAS_GPS              - has built-in GPS module
// HAS_OLED             - has small OLED (SSD1306)
// RADIO_SX1262         - uses SX1262 (RadioLib)
// RADIO_SX1276         - uses SX1276 (RadioLib)
// RADIO_DIO2_RF_SWITCH - SX1262 DIO2 controls the RF antenna switch (Heltec, T-Deck, etc.)
// RADIO_RXEN           - External RF switch pin (XIAO Wio-SX1262, set to GPIO number)
// RADIO_POWER          - TX power in dBm (default 20, max 22 for SX1262, 20 for SX1276)
// RADIO_CURRENT_LIMIT  - PA over-current protection in mA (default 140 for SX1262)
// RADIO_FEM_EN         - Front-end module enable pin (GC1109 on Heltec V4, HIGH = on)
// RADIO_FEM_TXEN       - Front-end TX enable pin (HIGH = TX/PA, LOW = RX/LNA)
// ADC_CTRL             - GPIO that gates the battery voltage divider (optional)
// ADC_CTRL_ACTIVE      - HIGH or LOW: which level enables the divider.
//                        N-channel low-side switch (V3/V4) → HIGH (default)
//                        P-channel high-side switch       → LOW
// BATT_HIBERNATE_DEFAULT  1 (default): firmware sleeps the chip when
//                          VBAT drops below BAT_LOW_MV, wakes when it
//                          recovers above BAT_RECOVER_MV.
//                        0: low-voltage auto-shutdown disabled. The
//                          node stays running until the LDO browns out.
//                          Useful for bench/lab boards, solar nodes
//                          where the operator prefers noisy low readings
//                          over an hour-long sleep, and during field
//                          commissioning. Override at runtime with the
//                          BATT_HIBERNATE ON|OFF serial command.
//                        Set per-board with `#define BATT_HIBERNATE_DEFAULT 0`
//                        inside that board's block below, or globally
//                        via a -DBATT_HIBERNATE_DEFAULT=0 build flag.

// ═══════════════════════════════════════════════════════════════
#if defined(BOARD_TDECK_PLUS)
// ═══════════════════════════════════════════════════════════════
// LilyGO T-Deck Plus
// ESP32-S3 + SX1262 + GPS + Keyboard + 2.8" TFT
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "T-Deck Plus"
#define HAS_DISPLAY         1
#define HAS_KEYBOARD        1
#define HAS_TRACKBALL       1
#define HAS_GPS             1
#define HAS_OLED            0
#define RADIO_SX1262        1
#define RADIO_DIO2_RF_SWITCH 1  // DIO2 controls antenna RF switch

// Power
#define BOARD_POWERON       10

// Shared SPI (Display + LoRa + SD)
#define BOARD_SPI_MOSI      41
#define BOARD_SPI_MISO      38
#define BOARD_SPI_SCK       40

// ST7789 TFT Display (320x240)
#define BOARD_TFT_CS        12
#define BOARD_TFT_DC        11
#define BOARD_TFT_BL        42

// SX1262 LoRa
#define RADIO_CS            9
#define RADIO_RST           17
#define RADIO_DIO1          45
#define RADIO_BUSY          13
#define RADIO_TCXO_VOLTAGE  1.8f  // SX1262 TCXO on DIO3 (same as Heltec)

// GPS (UART)
#define BOARD_GPS_TX        43
#define BOARD_GPS_RX        44

// I2C (keyboard + sensors)
#define BOARD_I2C_SDA       18
#define BOARD_I2C_SCL       8

// Keyboard
#define KB_I2C_ADDR         0x55
#define BOARD_KB_INT        46

// Trackball
#define BOARD_TBALL_UP      3
#define BOARD_TBALL_DOWN    15
#define BOARD_TBALL_LEFT    1
#define BOARD_TBALL_RIGHT   2
#define BOARD_TBALL_CLICK   0

// Touch Screen (GT911 on shared I2C bus)
#define BOARD_TOUCH_INT     16
#define TOUCH_I2C_ADDR      0x5D

// Speaker (I2S DOUT pin — used for simple tone() beeps)
#define BOARD_SPEAKER_PIN   6

// SD Card
#define BOARD_SDCARD_CS     39

// Battery
#define BOARD_BAT_ADC       4

// LED (none on T-Deck Plus)
#define BOARD_LED           -1

// DRV8871 actuator (none on T-Deck Plus)
#define ACTUATOR_IN1        -1
#define ACTUATOR_IN2        -1

// User GPIO pins available for relays/sensors
// (T-Deck Plus has limited free pins since most are used)
// Use the Grove connector or solder to test pads
#define USER_GPIO_COUNT     0

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_LORA32)
// ═══════════════════════════════════════════════════════════════
// LilyGO LoRa32 T3 V1.6.1
// ESP32 + SX1276 + 0.96" OLED - cheap repeater/relay node
// ~£15 - perfect for unmanned relay stations
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "LoRa32 v1.6.1"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            1
#define RADIO_SX1276        1

// Power (no power gate on this board)
#define BOARD_POWERON       -1

// SPI for LoRa
#define BOARD_SPI_MOSI      27
#define BOARD_SPI_MISO      19
#define BOARD_SPI_SCK       5

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1276 LoRa
#define RADIO_CS            18
#define RADIO_RST           23
#define RADIO_DIO0          26
// SX1276 doesn't have DIO1/BUSY in same way, use DIO0 for RX interrupt
#define RADIO_DIO1          RADIO_DIO0
#define RADIO_BUSY          -1

// No GPS
#define BOARD_GPS_TX        -1
#define BOARD_GPS_RX        -1

// I2C (for OLED display)
#define BOARD_I2C_SDA       21
#define BOARD_I2C_SCL       22
#define OLED_RST            16

// No keyboard/trackball
#define KB_I2C_ADDR         0
#define BOARD_KB_INT        -1
#define BOARD_TBALL_UP      -1
#define BOARD_TBALL_DOWN    -1
#define BOARD_TBALL_LEFT    -1
#define BOARD_TBALL_RIGHT   -1
#define BOARD_TBALL_CLICK   -1

// SD Card (optional, shares SPI)
#define BOARD_SDCARD_CS     13

// Battery
#define BOARD_BAT_ADC       35

// LED
#define BOARD_LED           25

// Button
#define BOARD_BUTTON        0

// DRV8871 actuator (none wired by default — set to GPIOs to enable)
#define ACTUATOR_IN1        -1
#define ACTUATOR_IN2        -1

// ─── User GPIO for relays & sensors ─────────────────────────
// These pins are FREE on the LoRa32 for relay/sensor use.
// Wire relays to these, configure via mesh commands.
//
// Available: 2, 4, 12, 13, 14, 15, 17, 33, 34, 36, 39
// (34, 36, 39 are INPUT ONLY - good for sensors)
// (13 conflicts with SD card CS - use one or the other)
//
// Default relay/sensor pin assignments (override via EEPROM config):
#define USER_GPIO_COUNT     8
#define DEFAULT_RELAY_PINS  { 2, 4, 12 }
#define DEFAULT_SENSOR_PINS { 15, 33, 34, 36, 39 }

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_HELTEC_V3)
// ═══════════════════════════════════════════════════════════════
// Heltec WiFi LoRa 32 V3
// ESP32-S3 + SX1262 + 0.96" OLED
// ~£20 - good mid-range repeater/relay node
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "Heltec V3"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            1
#define RADIO_SX1262        1
#define RADIO_DIO2_RF_SWITCH 1  // DIO2 controls antenna RF switch
#define RADIO_TCXO_VOLTAGE  1.8f  // SX1262 TCXO on DIO3 at 1.8V — required or radio hangs

// Power
#define BOARD_POWERON       -1
#define VEXT_CTRL           36    // Vext power for OLED (LOW = on)

// SPI for LoRa
#define BOARD_SPI_MOSI      10
#define BOARD_SPI_MISO      11
#define BOARD_SPI_SCK       9

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1262 LoRa
#define RADIO_CS            8
#define RADIO_RST           12
#define RADIO_DIO1          14
#define RADIO_BUSY          13

// No GPS
#define BOARD_GPS_TX        -1
#define BOARD_GPS_RX        -1

// I2C (for OLED display)
#define BOARD_I2C_SDA       17
#define BOARD_I2C_SCL       18
#define OLED_RST            21

// No keyboard/trackball
#define KB_I2C_ADDR         0
#define BOARD_KB_INT        -1
#define BOARD_TBALL_UP      -1
#define BOARD_TBALL_DOWN    -1
#define BOARD_TBALL_LEFT    -1
#define BOARD_TBALL_RIGHT   -1
#define BOARD_TBALL_CLICK   -1

// No SD Card
#define BOARD_SDCARD_CS     -1

// Battery (390k/100k divider on VBAT, gated by ADC_CTRL active-HIGH).
// Heltec V3 uses an N-channel low-side switch on the divider, so HIGH
// turns it on. (Some V3 docs claim active-low — verified empirically
// via BATT_DEBUG that this batch is active-high.)
// If you have a V3 batch with a P-channel high-side switch, set
//   #define ADC_CTRL_ACTIVE  LOW
// here to flip the polarity.
#define BOARD_BAT_ADC       1
#define ADC_CTRL            37    // gate pin
#define ADC_CTRL_ACTIVE     HIGH  // HIGH = enable divider on this board
#define BAT_DIVIDER         4.9f  // 100k/(390k+100k) = 0.2041 → mul by 4.9
#define BAT_LOW_MV          3300  // Cutoff: shut down to protect LiPo
#define BAT_RECOVER_MV      3700  // Hysteresis: must reach this to wake up

// LED
#define BOARD_LED           35

// Button (PRG button)
#define BOARD_BUTTON        0

// DRV8871 actuator (none wired by default — set to GPIOs to enable)
#define ACTUATOR_IN1        -1
#define ACTUATOR_IN2        -1

// ─── User GPIO for relays & sensors ─────────────────────────
// Free pins on Heltec V3:
// 2, 3, 4, 5, 6, 7, 33, 34, 40, 41, 42, 45, 46, 47, 48
#define USER_GPIO_COUNT     6
#define DEFAULT_RELAY_PINS  { 2, 3, 4 }
#define DEFAULT_SENSOR_PINS { 5, 6, 7, 33, 34 }
#define SENSOR_PIN_COUNT    2

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_HELTEC_V4)
// ═══════════════════════════════════════════════════════════════
// Heltec WiFi LoRa 32 V4
// ESP32-S3R2 + SX1262 + GC1109 PA (28dBm) + 0.96" OLED
// 16MB flash, native USB-C, solar input, GNSS connector
// ~£25 - best repeater option with highest TX power
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "Heltec V4"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            1
#define RADIO_SX1262        1
#define RADIO_DIO2_RF_SWITCH 1  // DIO2 controls SX1262 RF switch → GC1109 CTX
#define RADIO_FEM_EN        2     // GC1109 front-end enable (HIGH = on, permanently)
#define RADIO_FEM_TXEN      46    // GC1109 PA TX enable (HIGH = on, permanently)
#define RADIO_FEM_POWER     7     // GC1109 PA power pin (ANALOG mode)
#define RADIO_TCXO_VOLTAGE  1.8f  // SX1262 TCXO on DIO3 at 1.8V

// Power
#define BOARD_POWERON       -1
#define VEXT_CTRL           36    // Vext power for OLED (LOW = on)
#define ADC_CTRL            37    // gate pin for battery divider
#define ADC_CTRL_ACTIVE     HIGH  // HIGH = enable divider (same as V3)

// SPI for LoRa (same bus as V3)
#define BOARD_SPI_MOSI      10
#define BOARD_SPI_MISO      11
#define BOARD_SPI_SCK       9

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1262 LoRa
#define RADIO_CS            8
#define RADIO_RST           12
#define RADIO_DIO1          14
#define RADIO_BUSY          13

// GPS via GNSS SH1.25 connector (ribbon cable to external module)
// Pins from factory test: Serial1.begin(9600, SERIAL_8N1, 39, 38)
#define BOARD_GPS_TX        38
#define BOARD_GPS_RX        39

// I2C (for OLED display)
#define BOARD_I2C_SDA       17
#define BOARD_I2C_SCL       18
#define OLED_RST            21

// No keyboard/trackball
#define KB_I2C_ADDR         0
#define BOARD_KB_INT        -1
#define BOARD_TBALL_UP      -1
#define BOARD_TBALL_DOWN    -1
#define BOARD_TBALL_LEFT    -1
#define BOARD_TBALL_RIGHT   -1
#define BOARD_TBALL_CLICK   -1

// No SD Card
#define BOARD_SDCARD_CS     -1

// Battery (390k/100k divider on VBAT, gated by ADC_CTRL active-low)
#define BOARD_BAT_ADC       1
#define BAT_DIVIDER         4.9f  // 100k/(390k+100k) = 0.2041 → mul by 4.9
#define BAT_LOW_MV          3300  // Cutoff: shut down to protect LiPo
#define BAT_RECOVER_MV      3700  // Hysteresis: must reach this to wake up

// LED
#define BOARD_LED           35

// Button (PRG button)
#define BOARD_BUTTON        0

// DRV8871 actuator (none wired by default — set to GPIOs to enable)
#define ACTUATOR_IN1        -1
#define ACTUATOR_IN2        -1

// ─── User GPIO for relays & sensors ─────────────────────────
// Free pins on Heltec V4 (verified from V4 pinmap):
// 3, 4, 5, 6, 40, 41, 42, 47, 48
// RESERVED — DO NOT USE:
//   GPIO 2     = FEM_EN (front-end module)
//   GPIO 7     = FEM_POWER (PA power)
//   GPIO 19/20 = USB D-/D+ (kills USB serial)
//   GPIO 33/34 = FSPIHD/FSPICS0 (flash SPI — will crash!)
//   GPIO 38/39 = GPS header connector (hardwired to GNSS SH1.25)
//   GPIO 46    = FEM_TXEN (PA TX enable)
#define USER_GPIO_COUNT     6
#define DEFAULT_RELAY_PINS  { 3, 4 }          // NOT 2(FEM_EN), NOT 7(FEM_POWER)
#define DEFAULT_SENSOR_PINS { 5, 6, 40, 41 }  // GPIO33/34 are FSPI on V4, use 40/41 instead
#define SENSOR_PIN_COUNT    2

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_XIAO_S3)
// ═══════════════════════════════════════════════════════════════
// Seeed Studio XIAO ESP32S3 + Wio-SX1262 Kit
// ESP32-S3 + SX1262 — tiny 10x21mm, plug-together, no soldering
// ~£10 from The Pi Hut — cheapest SX1262 mesh repeater possible
//
// The Wio-SX1262 baseboard connects to the XIAO via the underside
// B2B (board-to-board / SMD edge) connector, NOT the through-hole
// header. The radio control pins are routed to GPIOs that are not
// exposed on the header at all (38–42).
//
// Pin map verified against:
//   - Seeed wiki: wiki.seeedstudio.com/wio_sx1262_with_xiao_esp32s3_kit
//   - RadioLib discussion #1361 (B2B connector pinout)
//
//  XIAO ESP32S3 through-hole header (top view, USB-C at top):
//                    [USB-C]
//          D0/GPIO1   ●  ● 5V
//          D1/GPIO2   ●  ● GND
//          D2/GPIO3   ●  ● 3V3
//          D3/GPIO4   ●  ● D10/GPIO9
//  (SDA)   D4/GPIO5   ●  ● D9 /GPIO8
//  (SCL)   D5/GPIO6   ●  ● D8 /GPIO7
//   (TX)   D6/GPIO43  ●  ● D7 /GPIO44  (RX)
//
//  Wio-SX1262 connections (via B2B underside connector — NOT on header):
//    SCK=GPIO7    MISO=GPIO8   MOSI=GPIO9
//    CS=GPIO41    RST=GPIO42   DIO1=GPIO39   BUSY=GPIO40
//    ANT_SW=GPIO38  (HIGH=RX enable, LOW=TX/idle)
//    TCXO=1.8V on DIO3 (internal to module)
//    DIO2 & DIO3 are internal to module — not host-controlled
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "XIAO ESP32S3"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            0    // set to 1 if you add an I2C SSD1306
#define RADIO_SX1262        1
#define RADIO_TCXO_VOLTAGE  1.8f  // Wio-SX1262 TCXO at 1.8V

// No power-on pin needed
#define BOARD_POWERON       -1

// SPI for LoRa (B2B connector — XIAO ESP32-S3 default SPI)
#define BOARD_SPI_SCK       7
#define BOARD_SPI_MISO      8
#define BOARD_SPI_MOSI      9

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1262 control pins (B2B underside connector — not on header)
#define RADIO_CS            41   // LORA_SPI_NSS
#define RADIO_RST           42   // LORA_RST (active low)
#define RADIO_DIO1          39   // LORA_DIO1
#define RADIO_BUSY          40   // LORA_BUSY (active high)
#define RADIO_RXEN          38   // LORA_ANT_SW: HIGH=RX, LOW=TX/idle

// No GPS
#define BOARD_GPS_TX        -1
#define BOARD_GPS_RX        -1

// I2C (optional SSD1306 OLED — connect to D4/GPIO5 SDA, D5/GPIO6 SCL)
#define BOARD_I2C_SDA       5
#define BOARD_I2C_SCL       6
#define OLED_RST            -1

// No keyboard/trackball
#define KB_I2C_ADDR         0
#define BOARD_KB_INT        -1
#define BOARD_TBALL_UP      -1
#define BOARD_TBALL_DOWN    -1
#define BOARD_TBALL_LEFT    -1
#define BOARD_TBALL_RIGHT   -1
#define BOARD_TBALL_CLICK   -1

// No SD card
#define BOARD_SDCARD_CS     -1

// No battery ADC on base board
#define BOARD_BAT_ADC       -1

// Built-in LED
#define BOARD_LED           21

// Boot button
#define BOARD_BUTTON        0

// DRV8871 actuator (none wired by default — set to GPIOs to enable)
#define ACTUATOR_IN1        -1
#define ACTUATOR_IN2        -1

// ─── User GPIO for relays & sensors ─────────────────────────
// Free header pins (B2B baseboard reserves only the underside pads):
//   D0/GPIO1, D1/GPIO2, D2/GPIO3, D3/GPIO4, D4/GPIO5, D5/GPIO6,
//   D6/GPIO43 (TX), D7/GPIO44 (RX)
// Reserved by Wio-SX1262 (B2B underside): 7, 8, 9, 38, 39, 40, 41, 42
#define USER_GPIO_COUNT     8
#define DEFAULT_RELAY_PINS  { 2, 4 }
#define DEFAULT_SENSOR_PINS { 5, 6 }         // 50/50 split, no overlap
#define SENSOR_PIN_COUNT    4

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_HELTEC_V2)
// ═══════════════════════════════════════════════════════════════
// Heltec WiFi LoRa 32 V2
// ESP32 (original, not S3) + SX1276 + 0.96" OLED
// ~£15 — classic Heltec board, widely available
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "Heltec V2"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            1
#define RADIO_SX1276        1

// Power (VEXT pin powers OLED — LOW = on)
#define BOARD_POWERON       -1
#define VEXT_CTRL           21    // Vext power for OLED (LOW = on)

// SPI for LoRa
#define BOARD_SPI_MOSI      27
#define BOARD_SPI_MISO      19
#define BOARD_SPI_SCK       5

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1276 LoRa (DIO0 = RX-done interrupt on SX1276)
#define RADIO_CS            18
#define RADIO_RST           14
#define RADIO_DIO0          26
// SX1276 doesn't have DIO1/BUSY in the same way; alias DIO0
#define RADIO_DIO1          RADIO_DIO0
#define RADIO_BUSY          -1

// No GPS
#define BOARD_GPS_TX        -1
#define BOARD_GPS_RX        -1

// I2C (for OLED display)
#define BOARD_I2C_SDA       4
#define BOARD_I2C_SCL       15
#define OLED_RST            16

// No keyboard/trackball
#define KB_I2C_ADDR         0
#define BOARD_KB_INT        -1
#define BOARD_TBALL_UP      -1
#define BOARD_TBALL_DOWN    -1
#define BOARD_TBALL_LEFT    -1
#define BOARD_TBALL_RIGHT   -1
#define BOARD_TBALL_CLICK   -1

// No SD Card
#define BOARD_SDCARD_CS     -1

// Battery ADC (pin 37 is input-only on Heltec V2)
#define BOARD_BAT_ADC       37

// LED
#define BOARD_LED           25

// Button (PRG/BOOT button)
#define BOARD_BUTTON        0

// DRV8871 actuator (none wired by default — set to GPIOs to enable)
#define ACTUATOR_IN1        -1
#define ACTUATOR_IN2        -1

// ─── User GPIO for relays & sensors ─────────────────────────
// Free pins on Heltec V2:
// 2, 12, 13, 17, 36, 39 — others are used by radio/OLED/SPI
// (36, 39 are INPUT ONLY — good for sensors/ADC)
#define USER_GPIO_COUNT     6
#define DEFAULT_RELAY_PINS  { 2, 4 }
#define DEFAULT_SENSOR_PINS { 12, 15, 34, 36 }  // 50/50 split

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_TIGGYOPENMESH_V1)
// ═══════════════════════════════════════════════════════════════
// TiggyOpenMesh V1 / V2 — custom 3S LiPo farm/repeater board
// ESP32-S3-WROOM-1-N8R2 + Wio-SX1262 (header) + DRV8871 actuator
// 12V 3S LiPo input → AP63203 buck → 3.3V. Native USB-C.
// 4 high-side MOSFET relays, 2 analog + 3 digital/opto sensor inputs,
// optional GPS and OLED via headers (J5 / GPS port).
//
// One board define (BOARD_TIGGYOPENMESH_V1) covers both PCB revs —
// V2 is electrically identical to V1+rework from the firmware's
// perspective, so no separate build env is needed. The "V1" string
// is the heartbeat board code that nodes already in the field report,
// so renaming would break recognition on existing deployments.
//
// V1 → V2 PCB changes (informational; firmware semantics unchanged):
//   • Battery divider: V1 routed Vbat→ADC to GPIO 37 (which has no
//     ADC peripheral), requiring R28 to be cut and fly-wired to GPIO 1
//     before the board would work. V2 routes Vbat→ADC natively to
//     GPIO 1 (ADC1_CH0).
//   • Relay 4: V2 adds an on-board high-side MOSFET driver on GPIO 37
//     (the pin freed by the battery re-route). The pin is HARDWIRED
//     to that driver — it can ONLY be used as a relay output, never
//     as a sensor input or anything else, regardless of what the
//     firmware would otherwise allow.
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "TiggyOpenMesh V1"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             1     // optional, header J?
#define HAS_OLED            1     // optional, header J5 (I2C SSD1306)
#define RADIO_SX1262        1
#define RADIO_TCXO_VOLTAGE  1.8f  // Wio-SX1262 TCXO at 1.8V
#define BATT_HIBERNATE_DEFAULT 0 // 0 for disabled 1 for enabled 

// Power
#define BOARD_POWERON       -1

// SPI for LoRa (Wio-SX1262 via 15.24mm header pins J14/J15)
#define BOARD_SPI_SCK       9
#define BOARD_SPI_MISO      11
#define BOARD_SPI_MOSI      10

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1262 control pins
#define RADIO_CS            8     // NSS
#define RADIO_RST           12    // NRST (active low)
#define RADIO_DIO1          14
#define RADIO_BUSY          13
#define RADIO_RXEN          47    // RF_SW (antenna switch): HIGH=RX, LOW=TX/idle

// GPS (UART, optional via external header)
#define BOARD_GPS_TX        38    // ESP TX → GPS RX
#define BOARD_GPS_RX        39    // ESP RX ← GPS TX

// I2C (OLED via header J5 — SSD1306)
#define BOARD_I2C_SDA       17
#define BOARD_I2C_SCL       18
#define OLED_RST            21

// No keyboard/trackball
#define KB_I2C_ADDR         0
#define BOARD_KB_INT        -1
#define BOARD_TBALL_UP      -1
#define BOARD_TBALL_DOWN    -1
#define BOARD_TBALL_LEFT    -1
#define BOARD_TBALL_RIGHT   -1
#define BOARD_TBALL_CLICK   -1

// No SD card
#define BOARD_SDCARD_CS     -1

// Battery — 3S LiPo via 1MΩ + 220kΩ divider (always-on, no ADC_CTRL gate)
//   V1 ships with the divider on GPIO 37 (no ADC peripheral) — needs
//   manual rework: cut R28→GPIO37, fly-wire R28 to the GPIO 1 pad.
//   V2 routes the divider directly to GPIO 1 on the PCB — no rework.
//   Divider math: (1M + 220k) / 220k = 5.545 (multiplier ADC→VBAT)
//   ADC sees max 12.6V × 0.180 = 2.27V (within ESP32-S3 2.4V headroom)
#define BOARD_BAT_ADC       1     // ADC1_CH0 GPIO1 (V1+rework / V2 native)
#define BAT_DIVIDER         5.545f
#define BAT_LOW_MV          9300    // 3 × 3.1V LiPo cutoff
#define BAT_RECOVER_MV      11100   // 3 × 3.7V LiPo recovery
//   Tuneable in field via:  BATT_CFG <divider> <low_mv> <recover_mv>

// LEDs (2 indicator LEDs)
#define BOARD_LED           35
#define BOARD_LED2          36

// Boot button only (no user button — just BOOT/EN)
#define BOARD_BUTTON        0

// Linear actuator (DRV8871 motor driver — exposed for app, not used by mesh)
#define ACTUATOR_IN1        41    // RAM_IN
#define ACTUATOR_IN2        42    // RAM_OUT

// ─── User GPIO for relays & sensors ─────────────────────────
// Reserved by hardware on this board:
//   8–14    Wio-SX1262 control + SPI
//   17–18   I2C OLED
//   19–20   USB
//   21      OLED RST  — DUAL ROLE (see sensor list below)
//   35–36   LEDs
//   38–39   GPS UART
//   41–42   DRV8871 actuator
//   46      strap pin
//   47      RF_SW
//   1       Battery ADC (V1+rework / V2 native)
//   37      Relay 4 driver (V2 only; V1 had Vbat divider here)
// Relay outputs (high-side MOSFET drivers on the PCB):
//   2, 3, 4    relays 1-3 (V1 + V2)
//   37         relay 4 — V2 only. HARDWIRED to the on-board driver,
//              cannot be repurposed as a sensor / digital input /
//              anything else. Drives the relay coil directly when
//              the GPIO goes HIGH. Boot-time behaviour: floats low
//              during ROM bootloader (~50-200ms), so the relay stays
//              de-energised until pinMode()+digitalWrite(LOW) runs
//              in setup() — no boot-time twitch.
// Sensor headers:
//   5, 6, 7    opto-isolated digital inputs (active LOW on GPIO side —
//              external HIGH lights the opto LED → GPIO reads LOW). Use
//              an inverter / Compare<1 downstream if your logic expects
//              positive polarity.
//   40         J9 direct-connect digital pin (JP1 closed bypasses opto).
//              Bidirectional + fast — required for DHT22, DS18B20,
//              HC-SR04 echo. R18 5.1k pull-up to 3.3V already on board.
//   15, 16     spare digital pins
//   21         OLED RST on the J5 header — DOUBLES as a sensor pin
//              when the OLED isn't deployed. The OLED driver only
//              touches GPIO 21 if DEVICE,ADD,OLED has been issued;
//              with no OLED in the device table the pin is
//              electrically free. Either OLED or a sensor on 21 per
//              node, not both. isPinSafe() passes it (not in the
//              forbidden list), so DEVICE,ADD against pin 21 deploys
//              cleanly when the user wires a sensor to J5 pin 3.
// Available extras for future use: 45, 48
#define USER_GPIO_COUNT     8
#define DEFAULT_RELAY_PINS  { 2, 3, 4, 37 }
#define DEFAULT_SENSOR_PINS { 5, 6, 7, 15, 16, 21, 40 }
#define SENSOR_PIN_COUNT    5

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_CUSTOM)
// ═══════════════════════════════════════════════════════════════
// Custom board - fill in your own pins
// Copy one of the above sections and modify
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "Custom Board"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            0
// Define either RADIO_SX1262 or RADIO_SX1276
#define RADIO_SX1276        1

#define BOARD_POWERON       -1
#define BOARD_SPI_MOSI      27
#define BOARD_SPI_MISO      19
#define BOARD_SPI_SCK       5
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1
#define RADIO_CS            18
#define RADIO_RST           23
#define RADIO_DIO0          26
#define RADIO_DIO1          26
#define RADIO_BUSY          -1
#define BOARD_GPS_TX        -1
#define BOARD_GPS_RX        -1
#define BOARD_I2C_SDA       21
#define BOARD_I2C_SCL       22
#define KB_I2C_ADDR         0
#define BOARD_KB_INT        -1
#define BOARD_TBALL_UP      -1
#define BOARD_TBALL_DOWN    -1
#define BOARD_TBALL_LEFT    -1
#define BOARD_TBALL_RIGHT   -1
#define BOARD_TBALL_CLICK   -1
#define BOARD_SDCARD_CS     -1
#define BOARD_LED           -1
#define BOARD_BUTTON        -1

// DRV8871 actuator (none wired by default — set to GPIOs to enable)
#define ACTUATOR_IN1        -1
#define ACTUATOR_IN2        -1

// ─── Battery monitoring (optional, V3/V4 reference impl) ────
// 1) Pick BOARD_BAT_ADC = an ADC1 GPIO (1–10 on ESP32-S3, 32–39 on ESP32).
// 2) Optional ADC_CTRL: gate pin to disconnect divider (active-low). Skip
//    if the divider is hard-wired across VBAT.
// 3) Size resistors so VBAT_max / DIVIDER < 2.4V (ESP32-S3 ADC headroom):
//    Lower=100kΩ, Upper=R_top:  DIVIDER = (R_top + 100kΩ) / 100kΩ
//
//    Battery     VBAT_max  DIVIDER   R_top
//    ────────    ────────  ───────   ─────
//    1S LiPo     4.2V       4.9      390k    (V3/V4 stock)
//    2S LiPo     8.4V       4.9      390k
//    3S LiPo     12.6V      5.7      470k
//    4S LiPo     16.8V      7.6      680k
//    12V SLA     14.4V      6.6      560k
//
// 4) Set thresholds. Per-cell cutoffs:
//    LiPo / Li-ion:  3.1V cutoff,  3.7V recovery
//    LiFePO4:        2.5V cutoff,  3.2V recovery
//    SLA (12V pack): 10.5V cutoff, 12.6V recovery
//
// All four below are runtime-overridable via the BATT_CFG serial command,
// so wrong compile-time values can be tuned in the field without re-flash.
//
// Example: 3S LiPo on GPIO 4, gated by GPIO 5 (N-channel low-side) ─
// #define BOARD_BAT_ADC    4
// #define ADC_CTRL         5
// #define ADC_CTRL_ACTIVE  HIGH    // or LOW for P-channel high-side
// #define BAT_DIVIDER      5.7f
// #define BAT_LOW_MV       9300    // 3 × 3.1V
// #define BAT_RECOVER_MV   11100   // 3 × 3.7V
#define BOARD_BAT_ADC       -1

#define USER_GPIO_COUNT     4
#define DEFAULT_RELAY_PINS  { 2, 4, 12, 15 }
#define DEFAULT_SENSOR_PINS { 34, 36 }
#define SENSOR_PIN_COUNT    2

// ═══════════════════════════════════════════════════════════════
#else
#error "No board defined! Add -DBOARD_TDECK_PLUS or -DBOARD_LORA32 to build_flags in platformio.ini"
#endif

// ─── IO Expansion Board UART2 Pins ──────────────────────────
// Secondary ESP32 connected via UART2 for additional I/O.
// Pins chosen to avoid conflict with LoRa, OLED, BLE, and sensor defaults.
// V3/V4: GPIO 48/47 are free on both boards and don't clash with any
// default relay/sensor pin (V3 relays include 2 and 3, V4 relays use 3).
#ifndef IO_EXPAND_TX
  #if defined(BOARD_HELTEC_V3) || defined(BOARD_HELTEC_V4)
    #define IO_EXPAND_TX  48
    #define IO_EXPAND_RX  47
  #elif defined(BOARD_XIAO_S3)
    // D6/D7 (silkscreen TX/RX) — avoids relay defaults on GPIO 2/4
    #define IO_EXPAND_TX  43
    #define IO_EXPAND_RX  44
  #elif defined(BOARD_LORA32)
    #define IO_EXPAND_TX  17
    #define IO_EXPAND_RX  16
  #elif defined(BOARD_HELTEC_V2)
    #define IO_EXPAND_TX  17
    #define IO_EXPAND_RX  13
  #else
    #define IO_EXPAND_TX  -1
    #define IO_EXPAND_RX  -1
  #endif
#endif

// Every board section above must define ACTUATOR_IN1 / ACTUATOR_IN2
// (use -1 if no DRV8871 wired). Same pattern as BOARD_GPS_TX, BOARD_LED
// etc — explicit per-board so adding a new board can't accidentally
// leave the actuator init pointing at random pins. The firmware checks
// at runtime: ACTUATOR_IN1 >= 0 → drive LOW at boot, else skip.

// ─── Storage Virtual Pins ───────────────────────────────────
// In-firmware state variables — pins 200..(200+STORAGE_VPIN_RESERVED-1)
// don't touch GPIO. They're a small array persisted to EEPROM that
// behaves exactly like digital pins from the perspective of CMD,SET /
// CMD,GET / Beacon-rule RELAY action / POLL / Digital Read in the GUI.
//
// VIRTUAL_PINS lists which of those slots the firmware automatically
// appends to BOTH relayPins[] and sensorPins[] on first boot (and on
// PIN_CONFIG_VERSION bump). Default is 8 vpins (200-207); a board can
// override by #define'ing VIRTUAL_PINS to a shorter or longer list
// before this point. Set to { } to disable entirely on a board.
//
// State survives reboot via EEPROM, so a flip-flop pattern built from
// two BEACON,ADD rules (one sets vpin 200, the other resets it) runs
// autonomously on the node — works without the gateway online.
#ifndef STORAGE_VPIN_BASE
  #define STORAGE_VPIN_BASE     200   // First storage vpin number
#endif
#ifndef STORAGE_VPIN_RESERVED
  #define STORAGE_VPIN_RESERVED 32    // Slots backed by EEPROM (RAM array size)
#endif
// Default to 8 vpins. POLL responses are now auto-split into multiple
// SDATA packets when the total would overflow the LoRa packet size
// limit, so the previous 4-vpin cap (chosen defensively when the
// firmware emitted everything in one packet) no longer applies.
// Bump per-board via #define before this point if you need more or
// fewer for a specific board.
#ifndef VIRTUAL_PINS
  #define VIRTUAL_PINS          { 200, 201, 202, 203, 204, 205, 206, 207 }
#endif
