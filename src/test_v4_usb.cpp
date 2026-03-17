#include <Arduino.h>
#include "HWCDC.h"

// V4 USB test — try every possible serial output method.
// One of these MUST produce output on the USB JTAG/serial port.

// When CDC_ON_BOOT=0 and USB_MODE=1:
//   Serial = HardwareSerial(0) = UART0 (goes nowhere on V4)
//   USBSerial = HWCDC (the USB JTAG serial)
//
// When CDC_ON_BOOT=1 and USB_MODE=1:
//   Serial = HWCDC
//   Serial0 = HardwareSerial(0) = UART0

extern HWCDC USBSerial;  // Always exists when USB_MODE=1

int counter = 0;

void setup() {
    // Try HWCDC directly
    USBSerial.begin(115200);
    delay(3000);

    // Also try Serial (may be HWCDC or UART0 depending on CDC_ON_BOOT)
    Serial.begin(115200);
    delay(1000);

    // Print on both
    USBSerial.println("=== V4 USB TEST (USBSerial) ===");
    Serial.println("=== V4 USB TEST (Serial) ===");
    USBSerial.println("If you see this on USBSerial, HWCDC works directly.");
    Serial.println("If you see this on Serial, Serial mapping works.");
}

void loop() {
    USBSerial.print("[USBSerial] Alive: ");
    USBSerial.println(counter);
    Serial.print("[Serial] Alive: ");
    Serial.println(counter);
    counter++;
    delay(1000);
}
