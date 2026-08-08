/* =====================================================================
   CIRCUIT B LOGGER  —  LM2596 buck module
   ESP32 (Arduino core 3.x)  •  2x INA226 on separate I2C buses

   The LM2596 sets its output with an onboard trim pot, so there is no
   PWM to generate. This sketch only measures and logs, using the exact
   same CSV columns as the MPPT sketch so the two logs plot together.

   PATH:  Panel -> INA226 #1 -> LM2596 -> INA226 #2 -> load

   COMMANDS (single character, no enter needed):
       b  = start a logging window
       x  = stop
       ?  = status

   GPIO25 is held LOW here, so if the wire to your own buck board is
   still connected, that converter stays off.

   FAIRNESS NOTE: set the LM2596 trim pot ONCE, before the run, and do
   not touch it again. Retuning it per session turns it into a manual
   tracker and erases the contrast you are trying to measure.
   ===================================================================== */

#include <Wire.h>

// ---------------------------------------------------------------- pinout
const int BUCK_PWM_PIN = 25;     // held low — not used in this circuit

const int PANEL_SDA = 32;        // INA226 #1, panel side   (I2C bus 0)
const int PANEL_SCL = 33;
const int LOAD_SDA  = 21;        // INA226 #2, load side    (I2C bus 1)
const int LOAD_SCL  = 22;

uint8_t ADDR_IN  = 0x44;         // auto-detected at boot
uint8_t ADDR_OUT = 0x44;

TwoWire &busIn  = Wire;
TwoWire &busOut = Wire1;

// ---------------------------------------------------------------- config
const float R_SHUNT_IN  = 0.02f;
const float R_SHUNT_OUT = 0.02f;

const unsigned long LOG_INTERVAL = 200;     // ms between CSV rows
const unsigned long WINDOW_MS    = 120000;  // 2 minute averaging window

// ------------------------------------------------------- INA226 registers
const uint8_t REG_CONFIG = 0x00;
const uint8_t REG_SHUNT  = 0x01;
const uint8_t REG_BUS    = 0x02;

const uint16_t INA_CONFIG = 0x4527;   // AVG=16, 1.1ms CT, continuous

const float BUS_LSB   = 0.00125f;     // 1.25 mV / bit
const float SHUNT_LSB = 0.0000025f;   // 2.5 uV / bit

bool inaWriteReg(TwoWire &bus, uint8_t addr, uint8_t reg, uint16_t val) {
  bus.beginTransmission(addr);
  bus.write(reg);
  bus.write((uint8_t)(val >> 8));
  bus.write((uint8_t)(val & 0xFF));
  return bus.endTransmission() == 0;
}

bool inaReadReg(TwoWire &bus, uint8_t addr, uint8_t reg, uint16_t &out) {
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom((int)addr, 2) != 2) return false;
  uint8_t hi = bus.read();
  uint8_t lo = bus.read();
  out = ((uint16_t)hi << 8) | lo;
  return true;
}

float inaBusVolts(TwoWire &bus, uint8_t addr) {
  uint16_t raw;
  if (!inaReadReg(bus, addr, REG_BUS, raw)) return NAN;
  return raw * BUS_LSB;
}

float inaAmps(TwoWire &bus, uint8_t addr, float rshunt) {
  uint16_t raw;
  if (!inaReadReg(bus, addr, REG_SHUNT, raw)) return NAN;
  int16_t s = (int16_t)raw;
  return (s * SHUNT_LSB) / rshunt;
}

bool inaProbe(TwoWire &bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

uint8_t findIna(TwoWire &bus) {
  for (uint8_t a = 0x40; a <= 0x4F; a++) if (inaProbe(bus, a)) return a;
  return 0;
}

// ------------------------------------------------------------- run state
const int MODE_STOP = 0;
const int MODE_LOG  = 1;
int mode = MODE_STOP;

bool haveIn = false, haveOut = false;

unsigned long tLastLog = 0, tWindowStart = 0;
double sumPout = 0, sumPin = 0;
long   nSamples = 0;

void resetWindow() {
  sumPout = 0; sumPin = 0; nSamples = 0;
  tWindowStart = millis();
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);
  delay(600);

  // Hold the custom buck's control line low so that converter stays off
  // if its wire is still attached.
  pinMode(BUCK_PWM_PIN, OUTPUT);
  digitalWrite(BUCK_PWM_PIN, LOW);

  busIn.begin(PANEL_SDA, PANEL_SCL, 100000);
  busOut.begin(LOAD_SDA, LOAD_SCL, 100000);

  uint8_t foundIn  = findIna(busIn);
  uint8_t foundOut = findIna(busOut);
  if (foundIn)  ADDR_IN  = foundIn;
  if (foundOut) ADDR_OUT = foundOut;
  haveIn  = (foundIn  != 0);
  haveOut = (foundOut != 0);

  if (haveIn)  inaWriteReg(busIn,  ADDR_IN,  REG_CONFIG, INA_CONFIG);
  if (haveOut) inaWriteReg(busOut, ADDR_OUT, REG_CONFIG, INA_CONFIG);

  Serial.println();
  Serial.println(F("=== CIRCUIT B — LM2596 LOGGER ==="));
  Serial.print(F("# panel meter (SDA ")); Serial.print(PANEL_SDA);
  Serial.print(F(" SCL ")); Serial.print(PANEL_SCL);
  Serial.print(F(" addr 0x")); Serial.print(ADDR_IN, HEX);
  Serial.print(F("): ")); Serial.println(haveIn ? "OK" : "NOT FOUND");

  Serial.print(F("# load  meter (SDA ")); Serial.print(LOAD_SDA);
  Serial.print(F(" SCL ")); Serial.print(LOAD_SCL);
  Serial.print(F(" addr 0x")); Serial.print(ADDR_OUT, HEX);
  Serial.print(F("): ")); Serial.println(haveOut ? "OK" : "NOT FOUND");

  Serial.print(F("# window ")); Serial.print(WINDOW_MS / 1000);
  Serial.println(F(" s"));
  Serial.println(F("# GPIO25 held LOW — custom buck disabled"));
  Serial.println(F("# commands: b x ?"));
  Serial.println(F("# ms,mode,duty,Vin,Iin,Pin,Vout,Iout,Pout,eff"));
  resetWindow();
}

// ------------------------------------------------------------------- loop
void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'b') {
      mode = MODE_LOG;
      Serial.println(F("# MODE -> B-LM2596"));
      resetWindow();
    } else if (c == 'x') {
      mode = MODE_STOP;
      Serial.println(F("# MODE -> STOP"));
    } else if (c == '?') {
      Serial.print(F("# mode="));
      Serial.print(mode == MODE_LOG ? "B-LM2596" : "STOP");
      Serial.print(F("  samples=")); Serial.println(nSamples);
    }
  }

  unsigned long now = millis();

  float Vin  = haveIn  ? inaBusVolts(busIn,  ADDR_IN)           : NAN;
  float Iin  = haveIn  ? inaAmps(busIn,  ADDR_IN,  R_SHUNT_IN)  : NAN;
  float Vout = haveOut ? inaBusVolts(busOut, ADDR_OUT)          : NAN;
  float Iout = haveOut ? inaAmps(busOut, ADDR_OUT, R_SHUNT_OUT) : NAN;

  float Pin  = (isnan(Vin)  || isnan(Iin))  ? NAN : Vin  * Iin;
  float Pout = (isnan(Vout) || isnan(Iout)) ? NAN : Vout * Iout;

  if (now - tLastLog >= LOG_INTERVAL) {
    tLastLog = now;

    float eff = (!isnan(Pin) && !isnan(Pout) && Pin > 0.05f) ? (Pout / Pin) : NAN;

    // "duty" column is always 0 here — the LM2596 has no commanded duty.
    // Kept so this log has identical columns to the MPPT log.
    Serial.print(now);   Serial.print(',');
    Serial.print(mode == MODE_LOG ? "B-LM2596" : "STOP"); Serial.print(',');
    Serial.print(0);     Serial.print(',');
    Serial.print(Vin, 3);  Serial.print(',');
    Serial.print(Iin, 4);  Serial.print(',');
    Serial.print(Pin, 4);  Serial.print(',');
    Serial.print(Vout, 3); Serial.print(',');
    Serial.print(Iout, 4); Serial.print(',');
    Serial.print(Pout, 4); Serial.print(',');
    if (isnan(eff)) Serial.println(); else Serial.println(eff, 4);

    if (mode == MODE_LOG) {
      if (!isnan(Pout)) sumPout += Pout;
      if (!isnan(Pin))  sumPin  += Pin;
      nSamples++;
    }
  }

  if (mode == MODE_LOG && now - tWindowStart >= WINDOW_MS && nSamples > 0) {
    double avgPout = sumPout / nSamples;
    double avgPin  = sumPin  / nSamples;

    Serial.println();
    Serial.print(F("SUMMARY,B-LM2596,"));
    Serial.print(F("avgPout=")); Serial.print(avgPout, 4); Serial.print(F(" W,"));
    Serial.print(F("avgPin="));  Serial.print(avgPin, 4);  Serial.print(F(" W,"));
    Serial.print(F("eff="));
    if (avgPin > 0.05) Serial.print(avgPout / avgPin, 4); else Serial.print(F("n/a"));
    Serial.print(F(",duty=0,n=")); Serial.println(nSamples);
    Serial.println();

    resetWindow();
  }

  delay(5);
}
