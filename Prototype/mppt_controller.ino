/* =====================================================================
   MPPT SOLAR CHARGE CONTROLLER  —  Asynchronous Buck Power Stage
   ESP32 (Arduino core 3.x)  •  2x INA226 on SEPARATE I2C buses  •  P&O

   RUN MODES — switch at runtime over serial, no reflashing outdoors:

       a  = Circuit A  direct connect  (PWM off, log only)
       b  = Circuit B  fixed duty      (no tracking)
       c  = Circuit C  MPPT            (perturb & observe)
       s  = duty sweep (characterise power vs duty — ground truth)
       x  = stop (duty 0)
       + / -   nudge duty by one step (mode b only)
       ?  = print status

   ---------------------------------------------------------------------
   PIN NOTES — READ BEFORE CHANGING
     GPIO34/35/36/39 are INPUT-ONLY. No output driver at all, so they
     cannot do I2C. (34/35 were the original panel-side pins.)
     GPIO12 is a strapping pin (MTDI). An I2C pull-up holds it high at
     reset, which stops the board booting or flashing.
     GPIO0, 2, 5, 15 are also strapping pins — avoid for I2C.
   --------------------------------------------------------------------- */

#include <Wire.h>

// ---------------------------------------------------------------- pinout
const int PWM_PIN = 25;          // buck control -> R1 -> Q2 base

const int PANEL_SDA = 32;        // INA226 #1, panel side   (I2C bus 0)
const int PANEL_SCL = 33;
const int LOAD_SDA  = 21;        // INA226 #2, load side    (I2C bus 1)
const int LOAD_SCL  = 22;

// Both modules were found at 0x44 (A1 -> VS, A0 -> GND). The sketch still
// scans 0x40..0x4F on each bus at boot and uses whatever it actually finds.
uint8_t ADDR_IN  = 0x44;   // panel side
uint8_t ADDR_OUT = 0x44;   // load side

TwoWire &busIn  = Wire;          // I2C peripheral 0
TwoWire &busOut = Wire1;         // I2C peripheral 1

// ---------------------------------------------------------------- config
const int PWM_FREQ = 20000;   // was 10000
const int PWM_RES  = 11;      // was 12

// DUTY CEILING. Cout is rated 16 V; on the 19 V bench 0.75 duty puts
// Vout at ~14 V. Do not raise without a higher-voltage output cap.
const int DUTY_MAX   = 1536;   // was 3072
const int DUTY_MIN   = 64;     // was 128
const int DUTY_START = 512;    // was 1024

// Shunts actually fitted to each breakout. Stock modules ship 0.1 ohm,
// which clips at 0.82 A (81.92 mV / 0.1). 0.02 ohm gives 4.1 A full scale.
const float R_SHUNT_IN  = 0.02f;
const float R_SHUNT_OUT = 0.02f;

const int PO_STEP = 8;                     // perturbation size, counts
const unsigned long PO_INTERVAL  = 100;     // ms between perturbations
const unsigned long LOG_INTERVAL = 200;     // ms between CSV rows
const unsigned long WINDOW_MS    = 120000;  // 2 minute averaging window
const unsigned long SOFTSTART_MS = 5000;    // ramp duty in over 5 s

const int SWEEP_FROM = 128;    // was 256
const int SWEEP_TO   = DUTY_MAX;
const int SWEEP_STEP = 32;
const unsigned long SWEEP_DWELL = 300;

// ------------------------------------------------------- INA226 registers
const uint8_t REG_CONFIG = 0x00;
const uint8_t REG_SHUNT  = 0x01;
const uint8_t REG_BUS    = 0x02;
const uint8_t REG_DIEID  = 0xFF;

// AVG=16, Vbus CT=1.1ms, Vshunt CT=1.1ms, continuous both -> ~35 ms
const uint16_t INA_CONFIG = 0x4527;

const float BUS_LSB   = 0.00125f;   // 1.25 mV / bit
const float SHUNT_LSB = 0.0000025f; // 2.5 uV / bit

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

// Current comes from the shunt register directly, not the INA226's own
// current register — that skips the calibration register, so a wrong CAL
// value can't silently scale every reading.
float inaBusVolts(TwoWire &bus, uint8_t addr) {
  uint16_t raw;
  if (!inaReadReg(bus, addr, REG_BUS, raw)) return NAN;
  return raw * BUS_LSB;
}

float inaAmps(TwoWire &bus, uint8_t addr, float rshunt) {
  uint16_t raw;
  if (!inaReadReg(bus, addr, REG_SHUNT, raw)) return NAN;
  int16_t s = (int16_t)raw;              // shunt register is signed
  return (s * SHUNT_LSB) / rshunt;
}

// Plain address probe — just checks whether anything ACKs. More forgiving
// than reading a register, which additionally requires repeated-start to work.
bool inaProbe(TwoWire &bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

// INA226 can sit anywhere in 0x40..0x4F depending on the A0/A1 straps.
// Scan rather than assume, so a wrong address constant can't be the fault.
uint8_t findIna(TwoWire &bus) {
  for (uint8_t a = 0x40; a <= 0x4F; a++) {
    if (inaProbe(bus, a)) return a;
  }
  return 0;
}

// ------------------------------------------------------------- run state
// Plain ints, not an enum: the Arduino IDE auto-inserts function prototypes
// above the first declaration, so a user-defined type used in a signature
// gets referenced before it exists and fails to compile.
const int MODE_STOP   = 0;
const int MODE_DIRECT = 1;
const int MODE_FIXED  = 2;
const int MODE_MPPT   = 3;
const int MODE_SWEEP  = 4;

int mode = MODE_STOP;

int   duty    = 0;
int   poDir   = PO_STEP;
float lastPin = -1.0f;

bool  haveIn = false, haveOut = false;

unsigned long tLastPO = 0, tLastLog = 0, tWindowStart = 0;
int   sweepDuty = 0;
unsigned long tSweepStep = 0;

double sumPout = 0, sumPin = 0;
long   nSamples = 0;

const char* modeName() {
  switch (mode) {
    case MODE_DIRECT: return "A-DIRECT";
    case MODE_FIXED:  return "B-FIXED";
    case MODE_MPPT:   return "C-MPPT";
    case MODE_SWEEP:  return "SWEEP";
    default:          return "STOP";
  }
}

void setDuty(int d) {
  if (d < 0) d = 0;
  if (d > DUTY_MAX) d = DUTY_MAX;      // hard ceiling, always enforced
  duty = d;
  ledcWrite(PWM_PIN, duty);
}

void softStart(int target) {
  Serial.println(F("# soft-start ramp"));
  int steps = 50;
  for (int i = 1; i <= steps; i++) {
    setDuty((int)((long)target * i / steps));
    delay(SOFTSTART_MS / steps);
  }
}

void resetWindow() {
  sumPout = 0; sumPin = 0; nSamples = 0;
  tWindowStart = millis();
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);
  delay(600);

  // PWM up first, at zero, so the FET is definitely off before anything else
  if (!ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES)) {
    Serial.println(F("!! ledcAttach FAILED — check pin/freq/resolution"));
  }
  ledcWrite(PWM_PIN, 0);

  busIn.begin(PANEL_SDA, PANEL_SCL, 100000);
  busOut.begin(LOAD_SDA, LOAD_SCL, 100000);

  uint8_t foundIn  = findIna(busIn);
  uint8_t foundOut = findIna(busOut);

  if (foundIn)  ADDR_IN  = foundIn;
  if (foundOut) ADDR_OUT = foundOut;

  haveIn  = (foundIn  != 0);
  haveOut = (foundOut != 0);

  Serial.println();
  Serial.println(F("=== MPPT Charge Controller ==="));
  Serial.print(F("# panel meter (SDA ")); Serial.print(PANEL_SDA);
  Serial.print(F(" SCL ")); Serial.print(PANEL_SCL);
  Serial.print(F(" addr 0x")); Serial.print(ADDR_IN, HEX);
  Serial.print(F("): ")); Serial.println(haveIn ? "OK" : "NOT FOUND");

  Serial.print(F("# load  meter (SDA ")); Serial.print(LOAD_SDA);
  Serial.print(F(" SCL ")); Serial.print(LOAD_SCL);
  Serial.print(F(" addr 0x")); Serial.print(ADDR_OUT, HEX);
  Serial.print(F("): ")); Serial.println(haveOut ? "OK" : "NOT FOUND");

  if (haveIn)  inaWriteReg(busIn,  ADDR_IN,  REG_CONFIG, INA_CONFIG);
  if (haveOut) inaWriteReg(busOut, ADDR_OUT, REG_CONFIG, INA_CONFIG);

  if (!haveIn) {
    Serial.println(F("!! Panel meter missing — MPPT cannot run."));
    Serial.println(F("!! Check: pull-ups present? address 0x40 vs 0x41?"));
    Serial.println(F("!! Check: SDA/SCL not on an input-only or strapping pin?"));
  }

  Serial.print(F("# duty ceiling ")); Serial.print(DUTY_MAX);
  Serial.print(F(" of ")); Serial.println((1 << PWM_RES) - 1);
  Serial.println(F("# commands: a b c s x + - ?"));
  Serial.println(F("# ms,mode,duty,Vin,Iin,Pin,Vout,Iout,Pout,eff"));
  resetWindow();
}

// ------------------------------------------------------------ mode switch
void enterMode(int m) {
  mode = m;
  lastPin = -1.0f;
  poDir = PO_STEP;

  Serial.print(F("# MODE -> ")); Serial.println(modeName());

  switch (m) {
    case MODE_STOP:
    case MODE_DIRECT:
      setDuty(0);
      break;
    case MODE_FIXED:
    case MODE_MPPT:
      setDuty(0);
      softStart(DUTY_START);
      break;
    case MODE_SWEEP:
      setDuty(0);
      sweepDuty = SWEEP_FROM;
      setDuty(sweepDuty);
      tSweepStep = millis();
      Serial.println(F("# sweep: duty,Vin,Iin,Pin,Vout,Iout,Pout"));
      break;
  }
  resetWindow();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'a': enterMode(MODE_DIRECT); break;
      case 'b': enterMode(MODE_FIXED);  break;
      case 'c': enterMode(MODE_MPPT);   break;
      case 's': enterMode(MODE_SWEEP);  break;
      case 'x': enterMode(MODE_STOP);   break;
      case '+': if (mode == MODE_FIXED) { setDuty(duty + PO_STEP);
                  Serial.print(F("# duty ")); Serial.println(duty); } break;
      case '-': if (mode == MODE_FIXED) { setDuty(duty - PO_STEP);
                  Serial.print(F("# duty ")); Serial.println(duty); } break;
      case '?':
        Serial.print(F("# ")); Serial.print(modeName());
        Serial.print(F("  duty=")); Serial.print(duty);
        Serial.print(F("  samples=")); Serial.println(nSamples);
        break;
      default: break;
    }
  }
}

// ------------------------------------------------------------------- loop
void loop() {
  handleSerial();

  unsigned long now = millis();

  float Vin  = haveIn  ? inaBusVolts(busIn,  ADDR_IN)            : NAN;
  float Iin  = haveIn  ? inaAmps(busIn,  ADDR_IN,  R_SHUNT_IN)   : NAN;
  float Vout = haveOut ? inaBusVolts(busOut, ADDR_OUT)           : NAN;
  float Iout = haveOut ? inaAmps(busOut, ADDR_OUT, R_SHUNT_OUT)  : NAN;

  float Pin  = (isnan(Vin)  || isnan(Iin))  ? NAN : Vin  * Iin;
  float Pout = (isnan(Vout) || isnan(Iout)) ? NAN : Vout * Iout;

  // ---- perturb & observe -------------------------------------------------
  if (mode == MODE_MPPT && haveIn && now - tLastPO >= PO_INTERVAL) {
    tLastPO = now;

    if (!isnan(Pin)) {
      // Lost power on the last move? Turn around. Gained? Keep going.
      if (lastPin >= 0 && Pin < lastPin) poDir = -poDir;
      lastPin = Pin;

      int next = duty + poDir;
      if (next >= DUTY_MAX) { next = DUTY_MAX; poDir = -PO_STEP; }
      if (next <= DUTY_MIN) { next = DUTY_MIN; poDir =  PO_STEP; }
      setDuty(next);
    }
  }

  // ---- duty sweep --------------------------------------------------------
  if (mode == MODE_SWEEP && now - tSweepStep >= SWEEP_DWELL) {
    tSweepStep = now;

    Serial.print(F("SWEEP,"));
    Serial.print(sweepDuty); Serial.print(',');
    Serial.print(Vin, 3);    Serial.print(',');
    Serial.print(Iin, 4);    Serial.print(',');
    Serial.print(Pin, 4);    Serial.print(',');
    Serial.print(Vout, 3);   Serial.print(',');
    Serial.print(Iout, 4);   Serial.print(',');
    Serial.println(Pout, 4);

    sweepDuty += SWEEP_STEP;
    if (sweepDuty > SWEEP_TO) {
      Serial.println(F("# sweep complete — the peak Pin row is the true MPP"));
      enterMode(MODE_STOP);
    } else {
      setDuty(sweepDuty);
    }
  }

  // ---- CSV logging + window averaging ------------------------------------
  if (mode != MODE_SWEEP && now - tLastLog >= LOG_INTERVAL) {
    tLastLog = now;

    float eff = (!isnan(Pin) && !isnan(Pout) && Pin > 0.05f) ? (Pout / Pin) : NAN;

    Serial.print(now);        Serial.print(',');
    Serial.print(modeName()); Serial.print(',');
    Serial.print(duty);       Serial.print(',');
    Serial.print(Vin, 3);     Serial.print(',');
    Serial.print(Iin, 4);     Serial.print(',');
    Serial.print(Pin, 4);     Serial.print(',');
    Serial.print(Vout, 3);    Serial.print(',');
    Serial.print(Iout, 4);    Serial.print(',');
    Serial.print(Pout, 4);    Serial.print(',');
    if (isnan(eff)) Serial.println(); else Serial.println(eff, 4);

    if (mode != MODE_STOP) {
      if (!isnan(Pout)) sumPout += Pout;
      if (!isnan(Pin))  sumPin  += Pin;
      nSamples++;
    }
  }

  // ---- 5 minute summary --------------------------------------------------
  if (mode != MODE_STOP && mode != MODE_SWEEP &&
      now - tWindowStart >= WINDOW_MS && nSamples > 0) {

    double avgPout = sumPout / nSamples;
    double avgPin  = sumPin  / nSamples;

    Serial.println();
    Serial.print(F("SUMMARY,"));
    Serial.print(modeName());    Serial.print(',');
    Serial.print(F("avgPout=")); Serial.print(avgPout, 4); Serial.print(F(" W,"));
    Serial.print(F("avgPin="));  Serial.print(avgPin, 4);  Serial.print(F(" W,"));
    Serial.print(F("eff="));
    if (avgPin > 0.05) Serial.print(avgPout / avgPin, 4); else Serial.print(F("n/a"));
    Serial.print(F(",duty="));   Serial.print(duty);
    Serial.print(F(",n="));      Serial.println(nSamples);
    Serial.println();

    resetWindow();
  }

  delay(5);
}
