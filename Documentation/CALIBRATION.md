# Calibrating the Pi-LOG payload

What to calibrate, how, how often, and where the numbers go. Uncertainties
quoted are the datasheet values the P4 display shows next to every reading.

**Principle: recordings stay raw.** The payload logs raw sensor values and the
calibration is applied afterwards, in the Filter Lab or SD-Parser. A wrong or
outdated calibration therefore never spoils a flight log: it can be redone at
any time.

| Sensor | What | How often | Method | Section |
|---|---|---|---|---|
| Accelerometer | offset + scale per axis | once per build, and after reassembly | six positions | [1](#1-accelerometer-six-position-calibration) |
| Gyroscope | zero-rate offset | **every flight**, automatically on the pad | still before launch | [2](#2-gyroscope) |
| Gyroscope | scale factor | once | counted turns | [2](#2-gyroscope) |
| Mounting | axes, tilt, heading | after assembly, and every flight | poses / pad notes | [3](#3-mounting-axes-and-the-pad) |
| BME680 | temperature offset | once | reference thermometer | [4](#4-bme680) |
| INA219 | current gain/offset | once | multimeter | [5](#5-ina219) |
| GPS | none (verify) | every flight | fix check | [6](#6-gps) |

## 0. Getting continuous data on the bench

On the ground (POST/ARMED) the payload writes only a **0.4 s slice every 10 s**
to its SD card, plus the last ~10 s at full rate when BOOST triggers. That is
right for flights but too sparse for calibration.

- **Best:** record with the P4 ground station. It writes every frame the radio
  receives to its own SD card as `/PILOG/RX_nnnn.BIN`, at full rate. Put a
  FAT32 card in the P4 before powering it.
- **Without the P4:** hold every pose for at least 40 s, so several 0.4 s
  slices land in each, and set "shortest still stretch" to 0.3 s in the
  six-position block (`--min-still 0.3` on the command line).

All analysis below works on `.bin` files directly: open them in the Filter Lab
(*Upload…*) or run the SD-Parser scripts.

## 1. Accelerometer: six-position calibration

About 15 minutes. You need a flat, level, vibration-free surface (check it with
a spirit level), something square to rest the payload against, and nobody
leaning on the table.

1. Power the payload as in flight and let it warm up for **10 minutes**. The
   offset drifts with temperature (datasheet: X/Y ±0.64 mg/°C, Z ±1 mg/°C).
2. Start recording (P4 backup, see section 0).
3. Put the payload in each of these six poses (payload CAD frame: +X away from
   the rail, +Y right seen from the front, +Z nose). In each pose, hold
   completely still for **at least 15 s**, then move to the next:

   | Pose | How it lies | Reads about |
   |---|---|---|
   | +Z up | standing, nose up (as in the rocket) | ( 0, 0, +1) g |
   | −Z up | upside down, nose down | ( 0, 0, −1) g |
   | +X up | on its back: rail side down, front facing up | (+1, 0, 0) g |
   | −X up | on its front: front down, rail side up | (−1, 0, 0) g |
   | +Y up | on its left side: right side facing up | ( 0, +1, 0) g |
   | −Y up | on its right side: left side facing up | ( 0, −1, 0) g |

4. Stop and copy the recording.
5. Solve, in either of two ways:
   - Filter Lab: preset **Calibrate the accelerometer (6 positions)**. It lists
     every still stretch, the offset and scale, and checks that |a| now reads
     1.000 g. *Download calibration.json*.
   - Command line:

     ```bash
     python SD-Parser/imu_calibration.py RX_0001.BIN --output calibration.json
     ```

6. Check the result:
   - all six poses found;
   - residual **< 5 mg rms**;
   - every scale within **1 ± 0.03** (datasheet ±3 %);
   - offsets within **±60 mg** (datasheet zero-g tolerance).

   Larger values mean a pose was not still or not level. Redo that pose.
7. Apply it: *Apply IMU calibration* in any recipe, with the numbers from
   `calibration.json`, or straight from a six-position block in the same
   recipe.

Notes:
- **Two ranges.** The firmware uses ±2 g on the ground and ±16 g only in
  BOOST, so bench data is always ±2 g. Each range has its own offset and scale.
  Calibrating ±16 g needs a bench firmware that logs in that range and tells
  the parser so; it does not exist yet. Until then, apply the ±2 g offsets and
  read boost accelerations as ±3 %.
- **Temperature.** Calibrate close to the flight temperature: a lab at 25 °C
  versus a 12 °C morning on the pad is up to ±13 mg on Z.
- What it improves: tilt from gravity (the rocket view, the attitude filters)
  and horizontal acceleration. Altitude and vertical speed depend on it less,
  because the trajectory pipeline subtracts the 1 g the accelerometer itself
  reads on the pad, which cancels most of the vertical offset and scale error.
  The uncalibrated MPU reads about 0.96 g.

## 2. Gyroscope

### Zero-rate offset: on the pad, every flight

This MPU's gyro offset measured 3-5 °/s, and it moves with temperature
(datasheet ±0.24 °/s per °C). A value from the lab would be wrong in flight, so
every tool measures it in the stillest stretch before launch: trajectory.py,
the Dashboard's rocket view and the Pad calibration block.

**Your part: nobody touches the rocket during the last 30 s before launch.**
The pre-trigger buffer holds the last ~10 s at full rate, and those seconds are
the calibration window. Rail adjustments, a hand on the airframe or a closing
hatch in that window show up as attitude error for the whole flight.

### Scale factor: once

1. Payload still on a table for 15 s.
2. Turn it exactly **10 full turns** about one axis, at a moderate speed (well
   under the ±500 °/s range), and stop where you started: mark a line on the
   table and on the payload. A lazy Susan or a turntable makes it easy (a
   record player at 33⅓ rpm turns at exactly 200 °/s).
3. Still for 15 s again. Repeat for the other two axes (lying on its back for
   X, on its side for Y).
4. Analyse:

   ```bash
   python SD-Parser/imu_calibration.py turns.bin --gyro-turns 10
   ```

   Scale per axis, expected 1 ± 0.03. Apply it with a *Scale & offset* block on
   that axis.

The range is ±500 °/s. If the rocket may roll faster than that (canted fins,
spin), log at ±1000 or ±2000 °/s instead: the calibration frame carries the
range and the parser follows it.

## 3. Mounting, axes and the pad

**Axis check (after every reassembly).** Put the payload in three of the poses
above (standing → Z ≈ +1 g, on its back → X ≈ +1 g, on its left side →
Y ≈ +1 g) and look at the IMU page of the P4 or at the raw data in the Filter
Lab. A wrong sign means the mapping in `Embedded-Code/main/sensor_config.h`
(`IMU_CAD_*`) no longer matches the board.

**Misalignment (optional).** With the assembled rocket held exactly vertical
(rail at 90°, or a plumb line along the airframe), record 30 s. After the
six-position calibration, the remaining X/Y readings are the tilt between the
payload and the rocket's axis: θ ≈ asin(a) per axis. Below 1° it can be
ignored.

**On the pad, every flight**, write down (or photograph with a compass app):
- the rail elevation (e.g. 84°);
- the compass direction the rocket's **front** (+X, the side away from the rail)
  faces.

The heading goes into `json2telemetry.py --kalman --heading-deg <deg>` and into
the Filter Lab's "front faces …°" field. Without it, east/north are rotated by
an unknown angle; altitude and vertical speed do not need it.

## 4. BME680

**Temperature.** The sensor sits next to warm electronics and heats its gas
plate every 10 s, so it reads high (about 29 °C at room temperature).
Calibrate it in the flight configuration (powered, enclosure closed): 30 min in
still air next to a reference thermometer. Offset = reading − reference; apply
it with *Scale & offset* (gain 1, offset −Δ). Altitude does not use
temperature (only pressure ratios), so this is for the atmospheric data only.

**Pressure.** Altitude is measured relative to the pad, so the absolute offset
(±0.6 hPa) cancels. What matters:
- the reference is taken just before launch (the tools use the stillest
  pre-launch stretch);
- weather drifts the pressure by about 0.1-0.3 hPa per hour, 1-3 m of fake
  altitude per hour on the pad. Keep long pad waits in mind when you read the
  pre-launch altitude.

Optional absolute check: the nearest airport's QNH converted to station
pressure, p = QNH · (1 − 2.2558·10⁻⁵ · h)^5.2559 with h the site elevation in m.

**Humidity (optional).** The salt test: sensor in a sealed jar above saturated
table salt (NaCl) slurry, 12-24 h, is 75.3 % RH at 20-25 °C. Magnesium chloride
gives 33 %. Two points give offset and gain; not needed for the flight.

**Gas.** Nothing to calibrate. Read it as relative change. A new BME680 needs
about 48 h of operation before its gas readings settle (Bosch's burn-in
recommendation).

## 5. INA219

1. Multimeter in series with the payload's supply (DC mA range).
2. Three operating points: idle (~25-30 mA), GPS + radio, everything on. Note
   meter and INA reading for each (the P4 power page, or the raw data).
3. Gain = I_meter / I_INA (expected within ±1-2 %: 0.1 Ω ±1 % shunt plus the INA's
   ±0.5 %). Offset = the INA reading with the load disconnected.
4. Bus voltage: multimeter on the INA's load side against the reading. Expect
   ±0.5 %, 4 mV steps.
5. Apply with *Scale & offset* on `power.current` / `power.voltage`.

Negative current means the shunt is wired the other way round:
`CONFIG_INA219_SHUNT_REVERSED` (on by default) flips the sign in firmware.

## 6. GPS

Nothing to calibrate. Before every flight check:
- a fix with **≥ 6 satellites** (the P4 GPS page shows both);
- the antenna has a clear sky view through the airframe. Carbon fibre blocks
  GPS and radio; glass fibre does not.

Static accuracy is about 1.5 m CEP (10 minutes outdoors, then *GPS to metres*
→ *Statistics* in the Filter Lab shows the scatter). The datasheet does not
specify GPS altitude: use the barometer for altitude.

## 7. Timestamps

Frames are stamped with the 10 ms FreeRTOS tick while the IMU samples at
83.3 Hz, so logged steps alternate 10/10/10/10/20 ms. Integrating with them adds
up to 8 ms error per step. trajectory.py and the *Fix IMU timestamps* block
rebuild the real sample times. Stamping frames with `esp_timer_get_time()` in
the firmware would remove the problem at the source.

## Checklist

**Once, before the campaign**
- [ ] six-position accelerometer calibration → `calibration.json` (note date and temperature)
- [ ] gyro turns test
- [ ] axis check after final assembly
- [ ] INA219 against a multimeter
- [ ] BME680 temperature offset, powered and closed

**Launch day**
- [ ] payload powered ≥ 10 min before it goes to the pad (warm-up)
- [ ] GPS fix with ≥ 6 satellites
- [ ] rail elevation and front heading written down / photographed
- [ ] **last 30 s before launch: nobody touches the rocket**

**After the flight**
- [ ] copy `fly.bin` (and the P4's `RX_*.BIN` backup)
- [ ] `bin2json.py` → `json2telemetry.py --kalman --heading-deg <deg>` → Dashboard
- [ ] Filter Lab: apply `calibration.json`, compare with the website's trajectory
