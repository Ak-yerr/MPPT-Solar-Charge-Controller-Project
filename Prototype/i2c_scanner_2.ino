/* =====================================================================
   DUAL-BUS I2C SCANNER  —  find the INA226 modules
   Flash this, open Serial Monitor at 115200, press reset.
   It scans both I2C buses every 3 seconds and reports every address
   that answers, plus whether it looks like an INA226.
   ===================================================================== */

#include <Wire.h>

// Change these if you want to test different pins
const int PANEL_SDA = 32;
const int PANEL_SCL = 33;
const int LOAD_SDA  = 21;
const int LOAD_SCL  = 22;

TwoWire &busA = Wire;    // panel bus
TwoWire &busB = Wire1;   // load bus

// INA226 die-ID register returns 0x2260; manufacturer register 0x5449
bool readReg16(TwoWire &bus, uint8_t addr, uint8_t reg, uint16_t &out) {
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom((int)addr, 2) != 2) return false;
  uint8_t hi = bus.read();
  uint8_t lo = bus.read();
  out = ((uint16_t)hi << 8) | lo;
  return true;
}

void scanBus(TwoWire &bus, const char *name, int sda, int scl) {
  Serial.print("--- ");
  Serial.print(name);
  Serial.print("  SDA=");
  Serial.print(sda);
  Serial.print(" SCL=");
  Serial.print(scl);
  Serial.println(" ---");

  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      found++;
      Serial.print("   device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);

      uint16_t manu = 0, die = 0;
      bool okM = readReg16(bus, addr, 0xFE, manu);
      bool okD = readReg16(bus, addr, 0xFF, die);
      if (okM && okD && manu == 0x5449 && die == 0x2260) {
        Serial.print("   <-- INA226 confirmed");
      } else if (okD) {
        Serial.print("   (die ID 0x");
        Serial.print(die, HEX);
        Serial.print(")");
      }
      Serial.println();
    }
    delay(2);
  }

  if (found == 0) {
    Serial.println("   nothing responded");
    Serial.println("   check: VCC to 3V3? GND to ESP32 GND?");
    Serial.println("          SDA/SCL swapped? pull-up resistors present?");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== DUAL-BUS I2C SCANNER ===");

  busA.begin(PANEL_SDA, PANEL_SCL, 100000);   // slow clock, more forgiving
  busB.begin(LOAD_SDA,  LOAD_SCL,  100000);
}

void loop() {
  scanBus(busA, "BUS A (panel)", PANEL_SDA, PANEL_SCL);
  scanBus(busB, "BUS B (load)",  LOAD_SDA,  LOAD_SCL);
  Serial.println("(rescanning in 3s — move wires and watch)");
  Serial.println();
  delay(3000);
}
