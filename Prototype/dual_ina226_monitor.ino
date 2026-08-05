/*
 * Dual INA226 Monitor - ESP32
 * -----------------------------------------------------
 * INA226 #1: address 0x44, Wire  bus -> SDA 21, SCL 22
 * INA226 #2: address 0x44, Wire1 bus -> SDA 16, SCL 17 (buck converter output)
 *
 * ASSUMPTIONS - verify before trusting data:
 *   - Both units at address 0x44 (confirm with scanner if unsure)
 *   - Both use 0.1 ohm (R100) onboard shunt
 *   - Adjust MAX_EXPECTED_CURRENT per channel below if loads differ
 *
 * Serial Plotter: open Tools > Serial Plotter in Arduino IDE,
 * set baud to 115200. Output uses "label:value" format so both
 * channels plot as separate labeled traces.
 */

#include <Wire.h>

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

const float SHUNT_RESISTANCE = 0.1; // ohms, both channels

// Adjust these independently if the two loads differ significantly
const float MAX_CURRENT_CH1 = 1.3;  // Amps, matches earlier 9V/10ohm test
const float MAX_CURRENT_CH2 = 3.0;  // Amps, guess for buck converter output - adjust to your actual load

struct INA226Channel {
  TwoWire* bus;
  uint8_t addr;
  float currentLSB;
  float powerLSB;
  uint16_t calValue;
  const char* label;
  bool healthy;
};

INA226Channel ch1 = { &Wire,  0x44, 0, 0, 0, "CH1", false };
INA226Channel ch2 = { &Wire1, 0x44, 0, 0, 0, "CH2", false };

uint16_t readRegister16(INA226Channel &ch, uint8_t reg) {
  ch.bus->beginTransmission(ch.addr);
  ch.bus->write(reg);
  if (ch.bus->endTransmission(false) != 0) return 0xFFFF;
  ch.bus->requestFrom(ch.addr, (uint8_t)2);
  if (ch.bus->available() < 2) return 0xFFFF;
  uint16_t value = ch.bus->read() << 8;
  value |= ch.bus->read();
  return value;
}

bool writeRegister16(INA226Channel &ch, uint8_t reg, uint16_t value) {
  ch.bus->beginTransmission(ch.addr);
  ch.bus->write(reg);
  ch.bus->write((uint8_t)(value >> 8));
  ch.bus->write((uint8_t)(value & 0xFF));
  return (ch.bus->endTransmission() == 0);
}

bool initChannel(INA226Channel &ch, float maxCurrent) {
  ch.currentLSB = maxCurrent / 32768.0;
  ch.calValue = (uint16_t)(0.00512 / (ch.currentLSB * SHUNT_RESISTANCE));
  ch.powerLSB = 25 * ch.currentLSB;

  ch.bus->beginTransmission(ch.addr);
  if (ch.bus->endTransmission() != 0) {
    Serial.print(ch.label);
    Serial.println(": NO ACK. Check wiring/address.");
    ch.healthy = false;
    return false;
  }

  uint16_t manufID = readRegister16(ch, REG_MANUF_ID);
  uint16_t dieID = readRegister16(ch, REG_DIE_ID);

  Serial.print(ch.label);
  Serial.print(" - Manuf ID: 0x");
  Serial.print(manufID, HEX);
  Serial.print(" Die ID: 0x");
  Serial.print(dieID, HEX);

  if (manufID != EXPECTED_MANUF_ID || dieID != EXPECTED_DIE_ID) {
    Serial.println("  -> ID MISMATCH, flagging unhealthy");
    ch.healthy = false;
    return false;
  }
  Serial.println("  -> OK");

  writeRegister16(ch, REG_CONFIG, 0x4127);
  writeRegister16(ch, REG_CALIBRATION, ch.calValue);

  uint16_t confirmCal = readRegister16(ch, REG_CALIBRATION);
  ch.healthy = (confirmCal == ch.calValue);

  Serial.print(ch.label);
  Serial.print(" Calibration: ");
  Serial.println(ch.healthy ? "OK" : "FAILED");

  return ch.healthy;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  delay(500);

  Wire.begin(21, 22);
  Wire.setClock(100000);

  Wire1.begin(16, 17);
  Wire1.setClock(100000);

  Serial.println("=== Dual INA226 Init ===");
  initChannel(ch1, MAX_CURRENT_CH1);
  initChannel(ch2, MAX_CURRENT_CH2);
  Serial.println("=== Init complete, starting plot stream ===\n");
  delay(1000);
}

void loop() {
  float bus1 = 0, current1 = 0, power1 = 0;
  float bus2 = 0, current2 = 0, power2 = 0;

  if (ch1.healthy) {
    bus1 = readRegister16(ch1, REG_BUS_VOLTAGE) * 0.00125;
    current1 = ((int16_t)readRegister16(ch1, REG_CURRENT)) * ch1.currentLSB;
    power1 = readRegister16(ch1, REG_POWER) * ch1.powerLSB;
  }

  if (ch2.healthy) {
    bus2 = readRegister16(ch2, REG_BUS_VOLTAGE) * 0.00125;
    current2 = ((int16_t)readRegister16(ch2, REG_CURRENT)) * ch2.currentLSB;
    power2 = readRegister16(ch2, REG_POWER) * ch2.powerLSB;
  }

  // Serial Plotter format: label:value, space-separated, one line per sample
  Serial.print("CH1_Bus:");
  Serial.print(bus1, 3);
  Serial.print(" CH1_Current:");
  Serial.print(current1, 4);
  Serial.print(" CH2_Bus:");
  Serial.print(bus2, 3);
  Serial.print(" CH2_Current:");
  Serial.println(current2, 4);

  delay(100); // 10Hz update rate, adjust for smoother/faster plotting
}
