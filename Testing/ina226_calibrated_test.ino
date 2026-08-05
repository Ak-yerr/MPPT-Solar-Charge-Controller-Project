/*
 * INA226 Calibrated Test - ESP32
 * -----------------------------------------------------
 * Address: 0x44
 * Shunt: 0.1 ohm (onboard, R100)
 * Calibrated for max expected current ~1.3A
 *
 * Wiring:
 *   VCC -> 3.3V
 *   GND -> GND (common with power source - and load -)
 *   SDA -> GPIO21
 *   SCL -> GPIO22
 *   IN+ -> power source +
 *   IN-, VBS -> load resistor (10 ohm, 50W)
 *   ALE -> unconnected
 */

#include <Wire.h>

#define INA226_ADDR       0x44
#define REG_CONFIG        0x00
#define REG_SHUNT_VOLTAGE 0x01
#define REG_BUS_VOLTAGE   0x02
#define REG_POWER         0x03
#define REG_CURRENT       0x04
#define REG_CALIBRATION   0x05
#define REG_MANUF_ID      0xFE
#define REG_DIE_ID        0xFF

#define EXPECTED_MANUF_ID 0x5449
#define EXPECTED_DIE_ID   0x2260

// --- Calibration math for 0.1 ohm shunt, ~1.3A max expected current ---
// Current_LSB = Max_Expected_Current / 2^15
const float CURRENT_LSB = 0.00004;      // 40uA per bit
const float SHUNT_RESISTANCE = 0.1;     // ohms
const uint16_t CAL_VALUE = (uint16_t)(0.00512 / (CURRENT_LSB * SHUNT_RESISTANCE)); // = 1280 (0x500)
const float POWER_LSB = 25 * CURRENT_LSB; // per datasheet: Power_LSB = 25 x Current_LSB

uint16_t readRegister16(uint8_t reg) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return 0xFFFF;
  }
  Wire.requestFrom(INA226_ADDR, (uint8_t)2);
  if (Wire.available() < 2) return 0xFFFF;
  uint16_t value = Wire.read() << 8;
  value |= Wire.read();
  return value;
}

bool writeRegister16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA226_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return (Wire.endTransmission() == 0);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  delay(500);

  Wire.begin(21, 22);
  Wire.setClock(100000);

  Serial.println("=== INA226 Calibrated Diagnostic ===");

  Wire.beginTransmission(INA226_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("No ACK at 0x44. Check wiring/power.");
    while (1) delay(1000);
  }
  Serial.println("Device ACK confirmed at 0x44.");

  uint16_t manufID = readRegister16(REG_MANUF_ID);
  Serial.print("Manufacturer ID: 0x");
  Serial.print(manufID, HEX);
  Serial.println(manufID == EXPECTED_MANUF_ID ? "  -> OK" : "  -> MISMATCH, check chip");

  uint16_t dieID = readRegister16(REG_DIE_ID);
  Serial.print("Die ID: 0x");
  Serial.print(dieID, HEX);
  Serial.println(dieID == EXPECTED_DIE_ID ? "  -> OK" : "  -> MISMATCH, check chip");

  // Config: avg=1, 1.1ms conversion, continuous shunt+bus mode
  writeRegister16(REG_CONFIG, 0x4127);

  // Write calibration register - this is what actually fixes the range problem
  if (!writeRegister16(REG_CALIBRATION, CAL_VALUE)) {
    Serial.println("Calibration write FAILED.");
  } else {
    uint16_t confirmCal = readRegister16(REG_CALIBRATION);
    Serial.print("Calibration register: 0x");
    Serial.print(confirmCal, HEX);
    Serial.println(confirmCal == CAL_VALUE ? "  -> OK" : "  -> MISMATCH");
  }

  Serial.println("=== Entering read loop ===\n");
}

void loop() {
  int16_t shuntRaw = (int16_t)readRegister16(REG_SHUNT_VOLTAGE);
  uint16_t busRaw = readRegister16(REG_BUS_VOLTAGE);
  int16_t currentRaw = (int16_t)readRegister16(REG_CURRENT);
  uint16_t powerRaw = readRegister16(REG_POWER);

  float shunt_mV = shuntRaw * 0.0025;
  float bus_V = busRaw * 0.00125;
  float current_A = currentRaw * CURRENT_LSB;
  float power_W = powerRaw * POWER_LSB;

  Serial.print("Shunt: ");
  Serial.print(shunt_mV, 3);
  Serial.print(" mV\t");

  Serial.print("Bus: ");
  Serial.print(bus_V, 3);
  Serial.print(" V\t");

  Serial.print("Current: ");
  Serial.print(current_A, 4);
  Serial.print(" A\t");

  Serial.print("Power: ");
  Serial.print(power_W, 4);
  Serial.println(" W");

  delay(1000);
}
