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

// No GPS (but has GNSS SH1.25 connector for external module)
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

// Battery (must set ADC_CTRL HIGH before reading)
#define BOARD_BAT_ADC       1

// LED
#define BOARD_LED           35

// Button (PRG button)
#define BOARD_BUTTON        0

// ─── User GPIO for relays & sensors ─────────────────────────
// Free pins on Heltec V4 (40-pin header, more GPIOs than V3):
// 2, 3, 4, 5, 6, 7, 15, 16, 19, 20, 33, 34, 38, 39, 40, 41, 42, 43, 44, 45, 46
#define USER_GPIO_COUNT     6
#define DEFAULT_RELAY_PINS  { 2, 3, 4, 5, 6, 7 }
#define DEFAULT_SENSOR_PINS { 19, 20, 33 }
#define SENSOR_PIN_COUNT    3

// ═══════════════════════════════════════════════════════════════
#elif defined(BOARD_XIAO_S3)
// ═══════════════════════════════════════════════════════════════
// Seeed Studio XIAO ESP32S3 + Wio-SX1262 Kit
// ESP32-S3 + SX1262 + no OLED (optional I2C SSD1306 on GPIO5/6)
// Tiny form factor ~10x21mm — cheapest SX1262 option at ~£10
// Perfect as a low-cost headless repeater node
// ═══════════════════════════════════════════════════════════════
//
//  XIAO ESP32S3 pinout (top view, USB-C at top):
//
//   [USB-C]
//  D0/GPIO1  ●  ● 5V
//  D1/GPIO2  ●  ● GND
//  D2/GPIO3  ●  ● 3V3
//  D3/GPIO4  ●  ● D10/GPIO9  (MOSI)
//  D4/GPIO5  ●  ● D9/GPIO8   (MISO)
//  D5/GPIO6  ●  ● D8/GPIO7   (SCK)
//  D6/GPIO43 ●  ● D7/GPIO44
//
//  Wio-SX1262 internal connections (routed on expansion PCB):
//  SCK  = GPIO7   MISO = GPIO8   MOSI = GPIO9
//  CS   = GPIO41  RST  = GPIO42  DIO1 = GPIO39
//  BUSY = GPIO40  RXEN = GPIO38
//
// ═══════════════════════════════════════════════════════════════

#define BOARD_NAME          "XIAO ESP32S3"
#define HAS_DISPLAY         0
#define HAS_KEYBOARD        0
#define HAS_TRACKBALL       0
#define HAS_GPS             0
#define HAS_OLED            0    // set to 1 if you add an I2C SSD1306
#define RADIO_SX1262        1

// No power-on pin needed
#define BOARD_POWERON       -1

// SPI for LoRa (Wio-SX1262 internal routing)
#define BOARD_SPI_MOSI      9
#define BOARD_SPI_MISO      8
#define BOARD_SPI_SCK       7

// No TFT display
#define BOARD_TFT_CS        -1
#define BOARD_TFT_DC        -1
#define BOARD_TFT_BL        -1

// SX1262 control pins (Wio-SX1262 internal)
#define RADIO_CS            41
#define RADIO_RST           42
#define RADIO_DIO1          39
#define RADIO_BUSY          40
#define RADIO_RXEN          38    // DIO2 used as RX enable

// No GPS
#define BOARD_GPS_TX        -1
#define BOARD_GPS_RX        -1

// I2C (optional SSD1306 OLED — solder to D4/D5 header pins)
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

// LED (built-in)
#define BOARD_LED           21

// Boot button
#define BOARD_BUTTON        0

// ─── User GPIO for relays & sensors ─────────────────────────
// Free header pins: D0/GPIO1, D1/GPIO2, D2/GPIO3, D3/GPIO4
// D6/GPIO43 (TX) and D7/GPIO44 (RX) also usable if UART not needed
#define USER_GPIO_COUNT     4
#define DEFAULT_RELAY_PINS  { 1, 2, 3, 4 }
#define DEFAULT_SENSOR_PINS { 43, 44 }
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
