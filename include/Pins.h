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

// ─── Board capabilities (set automatically) ─────────────────
// HAS_DISPLAY      - has a graphical TFT display
// HAS_KEYBOARD     - has I2C keyboard
// HAS_TRACKBALL    - has trackball input
// HAS_GPS          - has built-in GPS module
// HAS_OLED         - has small OLED (SSD1306)
// RADIO_SX1262     - uses SX1262 (RadioLib)
// RADIO_SX1276     - uses SX1276 (RadioLib)

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

// SD Card
#define BOARD_SDCARD_CS     39

// Battery
#define BOARD_BAT_ADC       4

// LED (none on T-Deck Plus)
#define BOARD_LED           -1

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

// ─── User GPIO for relays & sensors ─────────────────────────
// These pins are FREE on the LoRa32 for relay/sensor use.
// Wire relays to these, configure via mesh commands.
//
// Available: 2, 4, 12, 13, 14, 15, 17, 33, 34, 36, 39
// (34, 36, 39 are INPUT ONLY - good for sensors)
// (13 conflicts with SD card CS - use one or the other)
//
// Default relay/sensor pin assignments (override via EEPROM config):
#define USER_GPIO_COUNT     6
#define DEFAULT_RELAY_PINS  { 2, 4, 12, 15, 17, 33 }
#define DEFAULT_SENSOR_PINS { 34, 36, 39 }
#define SENSOR_PIN_COUNT    3

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

// Battery
#define BOARD_BAT_ADC       1

// LED
#define BOARD_LED           35

// Button (PRG button)
#define BOARD_BUTTON        0

// ─── User GPIO for relays & sensors ─────────────────────────
// Free pins on Heltec V3:
// 2, 3, 4, 5, 6, 7, 19, 20, 33, 34, 38, 39, 40, 41, 42, 43, 44, 45, 46
// Plenty available since ESP32-S3 has many GPIOs
#define USER_GPIO_COUNT     6
#define DEFAULT_RELAY_PINS  { 2, 3, 4, 5, 6, 7 }
#define DEFAULT_SENSOR_PINS { 19, 20, 33 }
#define SENSOR_PIN_COUNT    3

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
#define RADIO_TCXO_VOLTAGE  1.8f  // SX1262 TCXO on DIO3 at 1.8V

// Power
#define BOARD_POWERON       -1
#define VEXT_CTRL           36    // Vext power for OLED (LOW = on)
#define ADC_CTRL            37    // HIGH = enable battery voltage divider

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

// Battery (must set ADC_CTRL HIGH before reading)
#define BOARD_BAT_ADC       1

// LED
#define BOARD_LED           35

// Button (PRG button)
#define BOARD_BUTTON        0

// ─── User GPIO for relays & sensors ─────────────────────────
// Free pins on Heltec V4 (40-pin header, more GPIOs than V3):
// 2, 3, 4, 5, 6, 7, 15, 16, 33, 34, 38, 39, 40, 41, 42, 43, 44, 45, 46
// NOTE: GPIO 19/20 are USB D-/D+ — DO NOT USE as GPIO or USB serial dies!
#define USER_GPIO_COUNT     6
#define DEFAULT_RELAY_PINS  { 2, 3, 4, 5, 6, 7 }
#define DEFAULT_SENSOR_PINS { 33, 34, 38 }
#define SENSOR_PIN_COUNT    3

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_XIAO_S3)
// ═══════════════════════════════════════════════════════════════
// Seeed Studio XIAO ESP32S3 + Wio-SX1262 Kit
// ESP32-S3 + SX1262 — tiny 10x21mm, plug-together, no soldering
// ~£10 from The Pi Hut — cheapest SX1262 mesh repeater possible
//
// Pins verified from official Seeed schematic:
//   Wio-SX1262 for XIAO V1.0.kicad_sch
//
//  XIAO ESP32S3 header (top view, USB-C at top):
//
//            [USB-C]
//  RF_SW1 <- D0/GPIO1  ●  ● 5V
//            D1/GPIO2  ●  ● GND
//  RST    <- D2/GPIO3  ●  ● 3V3
//            D3/GPIO4  ●  ● D10/GPIO10 -> MOSI
//  (SDA)     D4/GPIO5  ●  ● D9 /GPIO9  <- MISO
//  (SCL)     D5/GPIO6  ●  ● D8 /GPIO8  -> SCK
//            D6/GPIO43 ●  ● D7 /GPIO44 -> CS
//
//  Wio-SX1262 connections (schematic net labels):
//  SCK=GPIO8   MISO=GPIO9   MOSI=GPIO10
//  CS=GPIO44   RST=GPIO3    DIO1=GPIO33  BUSY=GPIO34
//  RF_SW1=GPIO1  (HIGH=RX enable, LOW=TX/idle)
//  DIO2 & DIO3 are internal to module — not host-controlled
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "XIAO ESP32S3"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            0    // set to 1 if you add an I2C SSD1306
#define RADIO_SX1262        1
#define RADIO_TCXO_VOLTAGE  1.6f  // Wio-SX1262 TCXO at 1.6V

// No power-on pin needed
#define BOARD_POWERON       -1

// SPI for LoRa (from Wio-SX1262 schematic)
#define BOARD_SPI_SCK       8
#define BOARD_SPI_MISO      9
#define BOARD_SPI_MOSI      10

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1262 control pins (from Wio-SX1262 schematic net labels)
#define RADIO_CS            44   // LORA_SPI_NSS
#define RADIO_RST           3    // LORA_RST (active low)
#define RADIO_DIO1          33   // LORA_DIO1
#define RADIO_BUSY          34   // LORA_BUSY (active high)
#define RADIO_RXEN          1    // LORA_RF_SW1: HIGH=RX, LOW=TX/idle

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

// ─── User GPIO for relays & sensors ─────────────────────────
// Free header pins: D1/GPIO2, D3/GPIO4, D4/GPIO5, D5/GPIO6
// GPIO1,3,8,9,10,33,34,44 used by Wio-SX1262 — leave alone
#define USER_GPIO_COUNT     4
#define DEFAULT_RELAY_PINS  { 2, 4, 5, 6 }
#define DEFAULT_SENSOR_PINS { 43, 44 }
#define SENSOR_PIN_COUNT    2

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

// ─── User GPIO for relays & sensors ─────────────────────────
// Free pins on Heltec V2:
// 2, 12, 13, 17, 36, 39 — others are used by radio/OLED/SPI
// (36, 39 are INPUT ONLY — good for sensors/ADC)
#define USER_GPIO_COUNT     4
#define DEFAULT_RELAY_PINS  { 2, 12, 13, 17 }
#define DEFAULT_SENSOR_PINS { 36, 39 }
#define SENSOR_PIN_COUNT    2

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
#define BOARD_BAT_ADC       -1
#define BOARD_LED           -1
#define BOARD_BUTTON        -1
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
#ifndef IO_EXPAND_TX
  #if defined(BOARD_HELTEC_V3) || defined(BOARD_HELTEC_V4)
    #define IO_EXPAND_TX  2
    #define IO_EXPAND_RX  3
  #elif defined(BOARD_XIAO_S3)
    #define IO_EXPAND_TX  2
    #define IO_EXPAND_RX  4
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
