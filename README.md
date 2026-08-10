# MPPT Solar Charge Controller

**Status: Complete.**

This is a maximum power point tracking charge controller built around a custom asynchronous buck converter. An ESP32 reads panel-side and load-side voltage and current from two INA226 monitors on separate I2C buses, runs a perturb-and-observe loop on measured panel power, and adjusts the buck converter's PWM duty cycle to hold the panel at its maximum power point as irradiance changes.

The converter power stage was designed and built from discrete parts rather than using a converter module. The same firmware also runs two comparison modes (direct-to-load and fixed-duty) and a duty sweep, so all three circuits can be measured against each other on the same hardware and in the same conditions without reflashing.

---

## Table of Contents

- [Hardware](#hardware)
- [Wiring](#wiring)
- [Power Stage Design](#power-stage-design)
- [Run Modes](#run-modes)
- [Testing Results](#testing-results)
- [Control Architecture](#control-architecture)
- [Measurement](#measurement)
- [Software](#software)
- [Setup](#setup)

---

## Hardware

| Component | Purpose |
|---|---|
| ESP32 DevKitC | Main controller. Runs P&O tracking, PWM generation, and logging |
| 2x INA226 breakout (shunts replaced with 0.02 ohm) | Panel-side and load-side voltage/current measurement |
| IRF9540 P-channel MOSFET | Buck converter high-side switch |
| BC337 NPN transistor | Gate drive. Pulls the gate below the panel rail |
| 2x 3-33V zener, in series | Clamp holding Vgs inside the FET's ±20 V limit |
| SR560 Schottky diode | Asynchronous freewheel path |
| 100 µH inductor, 3A | Buck inductor |
| 1000 µF electrolytic, 16 V rated | Output filter. **The 16 V rating sets the duty ceiling** |
| 220 µF electrolytic + 100 nF ceramic | Input filter and local decoupling |
| 100 nF ceramic | Output-side decoupling |
| 1.0 kΩ resistor | PWM pin (GPIO25) to BC337 base |
| 680 Ω resistor | First zener to MOSFET Gate |
| 220 Ω resistor | MOSFET Gate to BC337 Collector |
| 10 W 12V solar panel | Source under test |
| 33 ohm load resistor | Fixed load, matched to the panel's MPP at 1,000 W/m² |
| Standard bench supply | 19 V bench supply used for indoor testing |
| Breadboard and jumper wires | Prototyping |

---

## Wiring

### Pin assignments

| Function | ESP32 GPIO |
|---|---|
| Buck PWM out (to R1, then Q2 base) | 25 |
| Panel-side INA226 I2C (SDA / SCL) | 32 / 33 |
| Load-side INA226 I2C (SDA / SCL) | 21 / 22 |

Both INA226 modules are strapped to the same address (0x44, A1 to VS and A0 to GND). This is only possible because they sit on two independent I2C peripherals rather than sharing one bus.

### Pin selection constraints

Three ESP32 constraints drove this pinout, and each one caused a real failure before it was understood:

- **GPIO34/35/36/39 are input-only.** They have no output driver at all, so they cannot do I2C. 34 and 35 were the original panel-side pins and could not work.
- **GPIO12 is a strapping pin (MTDI).** An I2C pull-up holds it high at reset, which prevents the board from booting or flashing.
- **GPIO0, 2, 5, and 15 are also strapping pins** and are avoided for I2C for the same reason.

### Power

Panel positive goes to main segment with first INA226 monitor for current and voltage reading. IN- node goes to the buck converter positive rail, while GND is shared between panel, controller, and buck. 

DC-DC converter: Positive rail powers first parallel capacitor network and MOSFET source. Negative rail connects to Schottky diode anode, first parallel capacitor network's GND node, and final OUT- output terminal.

Second INA226 recieves OUT+ and OUT- as IN+ and GND (shared), while VCC and GND (shared) is powered by the ESP32. IN- output (final converted output) is delivered to the final load, with the the non-positive side connected to the GND rail shared across the controller, sensors, and converter module.

---

## Power Stage Design

### Topology and impedance matching

The converter is an asynchronous buck: a P-channel high-side switch with a diode freewheel path rather than a synchronous low-side FET. A buck converter presents an apparent input impedance of `load / D²`, so sweeping the duty cycle sweeps the load the panel sees. That is the entire mechanism the tracker uses — duty is the single knob that moves the panel's operating point along its I-V curve, and the P&O loop just walks that knob toward peak power.

### Gate drive

A P-channel MOSFET on the high side needs its gate pulled *below* the panel rail to turn on, which a 3.3 V ESP32 pin cannot do directly. An NPN transistor pulls the gate down, level-shifting the logic-level PWM into a full-swing gate drive referenced to the panel rail.

Two zeners in series clamp gate-to-source voltage within the FET's ±20 V limit, so a high input voltage cannot destroy the gate oxide.

### Duty ceiling

`DUTY_MAX` is set to 1536 out of 2047 (0.75) and is enforced in `setDuty()` on every call, not just at the tuning constants.

This is an output capacitor rating limit, not a control limit. The output cap is rated 16 V, and on the 19 V bench supply a duty of 0.75 puts Vout at roughly 14 V. **Raising this ceiling without fitting a higher-voltage output cap will destroy the cap.**

### Bring-up procedure

Power-stage bring-up was staged deliberately to separate control faults from power faults:

1. Logic and PWM verified first at low supply voltage, gate waveform checked on the scope before load, then supply voltage raised
2. Gate-to-source waveforms were monitored on the oscilloscope to confirm clean switching and to check for shoot-through before applying full supply voltage.

During bring-up, two big issues were revealed.

1. Inductor mismatch: When running the first iteration of testing the DC-DC converter design, the inductor would heat up incessantly within 10 seconds when the power supply (19V) was turned on. Based on the math, this shouldn't occur, and a duty of 1024 should be easily maintained by the prototype. Further testing revealed that the output voltages shot up to 16V after a few seconds of maintaining the correct output of 4V at duty 1024. This revealed that the inductors used originally were actually signal chokes, and not suitable for the projects.
2. Inductor saturation under transient inrush: When running the first iteration of testing the DC-DC converter design, the high-side MOSFET would instantly vaporize within 2 seconds when the current exceeded 2A, despite the inductor being rated for a much higher continuous current. Based on the math, a 2A load should be easily maintained by the prototype at a constant 1024 duty. Further testing under low power revealed that the inductor's magnetic core was physically saturating during sudden duty cycle changes. This caused its inductance to drop by 90% instantly, turning the inductor into a plain copper wire and creating an uncontrollable current spike that destroyed the switches.

---

## Run Modes

The firmware switches between circuits over serial at runtime, so all three configurations can be tested outdoors under the same sky without reflashing. This matters more than it sounds: solar conditions change minute to minute, and a comparison that requires a laptop and a reflash between circuits is not measuring the same conditions.

| Key | Mode | Behaviour |
|---|---|---|
| `a` | Circuit A, direct connect | PWM off, logging only. Panel straight to load |
| `b` | Circuit B, fixed duty | Converter runs at a fixed duty, no tracking |
| `c` | Circuit C, MPPT | Perturb and observe tracking |
| `s` | Duty sweep | Steps duty across its full range and logs power at each point |
| `x` | Stop | Duty to 0 |
| `+` / `-` | Nudge duty by one step (mode `b` only) |
| `?` | Print current mode, duty, and sample count |

### The sweep mode is the ground truth

`s` steps duty from 128 to 1536 in increments of 32, dwelling 300 ms at each point and logging panel and load power. The row with peak `Pin` is the true maximum power point for the conditions at that moment.

This exists to validate the tracker rather than to run in normal operation: the sweep finds the peak by brute force, and MPPT mode should converge on that same duty. Without it, there is no way to tell whether P&O has found the real peak or settled on a local artifact.

---

## Testing Results

All three circuits were run against a 33 ohm load and logged over 5 minutes under a clear sky with some cloud inconsistency.

| Circuit | Avg power delivered | vs. direct |
|---|---|---|
| A: Direct to load | ~5.9 W | baseline |
| B: LM2596 commercial buck | ~7.6 W | 27% |
| C: Custom buck with MPPT | ~8.7 W | 42% |

The headline results:

- **15% more power than a commercial buck converter** (LM2596) in the same conditions. This is the meaningful comparison, since both circuits are doing voltage conversion and only one is tracking.
- **42% more power than direct-to-load.** The 33 ohm load was chosen to match the panel's maximum power point at high light conditions, so at that operating point the direct connection is already optimal — it is not an arbitrary resistor. The 42% gap opens up because a fixed resistor stays matched at exactly one irradiance while the panel's MPP moves with the sun. This is the clearest statement of what tracking actually buys: not beating a bad baseline, but holding the peak that a correctly-sized fixed load only hits momentarily.
- Panel voltage held at 18.3 V with an average deviation of 2% when connected to the custom-built converter. This is at at the maximum power point given the environmental conditions of the trials.

---

## Control Architecture

### Perturb and observe

Every 100 ms, the loop compares the panel power now against the panel power at the last perturbation:

```
if (lastPin >= 0 && Pin < lastPin) poDir = -poDir;
lastPin = Pin;
duty += poDir;
```

If the last duty change increased panel power, keep moving the same direction. If it decreased power, reverse. The perturbation size is 8 counts out of an 11-bit range.

Duty is clamped to `[DUTY_MIN, DUTY_MAX]` = `[64, 1536]`, and hitting either rail forces the direction to reverse rather than pinning at the limit.

### Tracking on panel power, not load power

The P&O loop reads `Pin` — panel-side power — not `Pout`. This is deliberate. The maximum power point is a property of the panel's I-V curve, so the quantity to maximise is what the panel is delivering. Load-side power is what the converter passes after losses, and tracking on it would conflate panel behaviour with converter efficiency.

Both meters together give live converter efficiency (`Pout / Pin`), which is logged but never used as a control input.

### Steady-state behaviour

P&O never settles: it oscillates around the peak by ±1 perturbation step by design, since it has to keep moving to know which way is uphill. Step size trades tracking speed against how tightly it holds the peak — 8 counts was chosen for a 2-minute window to run more inconsistent, worsening daylight conditions.

### Soft start

Entering fixed-duty or MPPT mode ramps duty from 0 to `DUTY_START` (512) over 5 seconds in 50 steps rather than stepping straight to it. This avoids a large inrush transient through the inductor and output cap at switch-on.

---

## Measurement

### Shunt replacement

The stock INA226 breakouts ship with 0.1 ohm shunts, which clip at 0.82 A (81.92 mV full-scale shunt voltage / 0.1 ohm). Both were replaced with 0.02 ohm shunts, giving 4.1 A full scale.

### Reading the shunt register directly

Current is computed from the shunt voltage register (0x01) rather than the INA226's own current register:

```cpp
int16_t s = (int16_t)raw;              // shunt register is signed
return (s * SHUNT_LSB) / rshunt;
```

The chip's current register requires a correctly written calibration register. If that value is wrong, every current reading is silently scaled by the wrong factor with no error and no obvious symptom. Reading the raw shunt voltage and dividing by the known shunt resistance skips the calibration register entirely, so a wrong constant cannot corrupt the measurement.

The shunt register is signed, so the raw 16-bit value is cast to `int16_t` before scaling — treating it as unsigned would turn negative currents into very large positive ones.

### Configuration

`INA_CONFIG` is `0x4527`: 16-sample averaging, 1.1 ms conversion time on both bus and shunt channels, continuous mode on both. That works out to roughly 35 ms per conversion, comfortably faster than the 100 ms perturbation interval so each P&O decision reads fresh data.

### Address scanning

The INA226 can sit anywhere in 0x40 to 0x4F depending on how A0 and A1 are strapped. Rather than assuming, `findIna()` scans that range on each bus at boot and uses whatever actually responds, so a wrong address constant can never be the fault being debugged.

The probe is a bare address ACK check rather than a register read, because a register read additionally requires repeated-start to work correctly — a probe that requires more of the bus than necessary can fail for reasons unrelated to whether the device is present.

---

## Software

The firmware is a single Arduino sketch with no external INA226 or PWM libraries. Register access, PWM setup, and the tracking loop are written directly against the ESP32 Arduino core APIs.

### Safe startup ordering

PWM is attached and written to 0 *before* the I2C buses come up:

```cpp
if (!ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES)) {
  Serial.println(F("!! ledcAttach FAILED — check pin/freq/resolution"));
}
ledcWrite(PWM_PIN, 0);
```

This guarantees the FET is definitely off before anything else runs. If I2C setup blocks or fails with the gate pin floating, the switch could sit in an undefined state with the panel connected.

### Mode constants are plain ints, not an enum

```cpp
const int MODE_STOP   = 0;
const int MODE_DIRECT = 1;
// ...
```

The Arduino IDE auto-inserts function prototypes above the first declaration in the sketch. A user-defined type used in a function signature therefore gets referenced before it exists, and the sketch fails to compile. Plain `int` constants avoid the problem entirely.

### Duty ceiling enforced at the setter

```cpp
void setDuty(int d) {
  if (d < 0) d = 0;
  if (d > DUTY_MAX) d = DUTY_MAX;      // hard ceiling, always enforced
  duty = d;
  ledcWrite(PWM_PIN, duty);
}
```

Every path that changes duty — P&O, sweep, soft start, manual nudge — goes through this one function. The hardware limit that protects the output cap is enforced in one place rather than checked at each call site, so a new mode cannot forget it.

### NaN handling

Every measurement can return `NAN` if a meter is missing or a read fails, and that propagates into `Pin`, `Pout`, and efficiency rather than producing a plausible-looking wrong number. The P&O loop is gated on `!isnan(Pin)`, so a failed read leaves duty unchanged instead of perturbing on garbage.

### Logging

CSV rows are emitted every 200 ms in the format `ms,mode,duty,Vin,Iin,Pin,Vout,Iout,Pout,eff`, with a header printed at boot. Efficiency is only computed when `Pin > 0.05 W` to avoid dividing by near-zero at night or under a covered panel.

---

## Setup

1. Build the power stage per [Power Stage Design](#power-stage-design). Confirm the output capacitor's voltage rating before choosing a supply voltage, and check `DUTY_MAX` against it.
2. Replace both INA226 shunts with 0.02 ohm parts if the modules are stock.
3. Wire per [Wiring](#wiring). Both meters go on separate I2C buses.
4. Flash the sketch and open Serial Monitor at 115200 baud. Confirm both meters report `OK` at boot — if either is `NOT FOUND`, check pull-ups, address straps, and that neither bus landed on an input-only or strapping pin.
5. Bring up the power stage at low supply voltage first with the load disconnected, and verify gate switching on the scope before raising voltage or connecting the panel.
6. Run `s` to sweep duty and identify the true maximum power point for current conditions.
7. Run `c` for MPPT and confirm the tracker converges near the duty the sweep identified.
8. For comparison runs, cycle `a`, `b`, and `c` back to back so all three circuits see the same conditions.
