#include <Arduino.h>

// Bare minimum test for Heltec V4 USB serial.
// No BLE, no radio, no OLED, no EEPROM — just Serial.

int counter = 0;

void setup() {
    Serial.begin(115200);
    delay(3000);  // Give USB time to enumerate
    Serial.println("=== V4 USB TEST ===");
    Serial.println("If you see this, USB serial works.");
}

void loop() {
    Serial.print("Alive: ");
    Serial.println(counter++);
    delay(1000);
}
