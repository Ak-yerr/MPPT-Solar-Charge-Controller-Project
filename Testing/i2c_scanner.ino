/*
 * I2C Scanner for ESP32
 * -----------------------------------------------------
 * Scans all valid 7-bit I2C addresses (0x08-0x77) and
 * reports which devices ACK. Use this to find your
 * INA226's actual address before running any diagnostic
 * or measurement code.
 *
 * Wiring:
 *   SDA -> GPIO21
 *   SCL -> GPIO22
 */

#include <Wire.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  delay(500);

  Wire.begin(21, 22); // SDA, SCL
  Wire.setClock(100000);

  Serial.println("=== I2C Scanner ===");
}

void loop() {
  int devicesFound = 0;

  Serial.println("Scanning...");

  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
      Serial.print("Device found at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      devicesFound++;
    }
  }

  if (devicesFound == 0) {
    Serial.println("No I2C devices found. Check wiring, pull-ups, and power.");
  } else {
    Serial.print(devicesFound);
    Serial.println(" device(s) found.");
  }

  Serial.println("Done. Rescanning in 5 seconds.\n");
  delay(5000);
}
