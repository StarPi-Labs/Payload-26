# Star-PI Payload — Bring-up Changes & Current State

_First hardware bring-up of the ESP32-S3 board (June 2026)._

## Target hardware

- **MCU:** ESP32-S3-WROOM-1 **N8R8** (8 MB flash + 8 MB octal PSRAM). IDF target switched from classic `esp32` → `esp32s3`.
- **IMU:** labelled "MPU6050" but physically an **MPU6500** (WHO_AM_I `0x70`); register-compatible for accel/gyro/temp.
- **GPS:** u-blox **SAM-M10Q**, runs at **9600 baud** on this board (confirmed by raw NMEA dump).
- **Environment/gas:** **BME680** (chip id `0x61`), I²C `0x77` (auto-detected).
- **Power monitor:** INA219 present but the bench unit is **electrically dead** (likely a wiring short during bring-up). Driver is verified; hardware needs replacement.

## Pin map (S3 — all flash/PSRAM/USB pins avoided)

| Function | Pin |
|---|---|
| I²C0 SDA / SCL (MPU6500) | GPIO8 / GPIO9 |
| MPU data-ready INT | GPIO4 |
| I²C1 SDA / SCL (BME680, INA219) | GPIO13 / GPIO14 |
| GPS UART2 TX / RX | GPIO6 / GPIO7 |
| SD-SPI MOSI / MISO / SCLK / CS | GPIO17 / GPIO16 / GPIO18 / GPIO5 |
| Console UART0 TX / RX | GPIO43 / GPIO44 |
| BOOT button (bench mode switch) | GPIO0 |

> On the N8R8, **GPIO26–37** are flash+octal-PSRAM and **GPIO19/20** are USB — all off-limits — and the S3 has **no GPIO22–25**. The original classic-ESP32 pin numbers (26/27, 34, 32/35, 23/19…) were remapped accordingly.

## Changes by area

### Build / configuration
- `CMakeLists.txt` (top): `set(EXCLUDE_COMPONENTS esp_lcd)` — its S3 RGB-panel driver crashes the GCC 15.2 toolchain and it's unused.
- `main/CMakeLists.txt`: compile `bme680.c` under `CONFIG_ENABLE_BME680` (was the non-existent `CONFIG_ENABLE_BME280`).
- `main/main.c`: `gps_start_task` / `ina219_start_task` / `mpu6050_start_task` and their init calls wrapped in `#if CONFIG_ENABLE_*`, so any sensor can be toggled in menuconfig without breaking the build.
- `main/health_monitoring.c`: removed deprecated `esp_rom_uart.h` include (broke `-Werror` on IDF v6).
- `main/bt_serial_bridge.c`: added the missing `bt_serial_write_chunk` stub for the BT-disabled build.

### Sensor fixes
- **MPU6500** (`mpu6050.c`): WHO_AM_I now accepts `0x68/0x70/0x71`; INT pin moved to GPIO4; **accelerometer range follows the flight mode — ±16 g in BOOST, ±2 g in all other phases** (re-written on each mode transition).
- **INA219** (`ina219.c`): fixed early-`return` that skipped reset/verify/config; added `vTaskDelete` so a missing chip can't panic-reboot; serial log prints mA / V.
- **GPS / SAM-M10Q** (`gps_sensor.c`, `main.c`): native **9600 baud**, removed the M8-only `UBX-CFG-PRT` baud-upgrade (unsupported on M10); task exits cleanly instead of a busy `while(1)`.

### New BME680 driver (`bme680.c` / `bme680.h`)
- Replaces the old stub. Auto-detects `0x76`/`0x77`, verifies chip-id `0x61`, soft-resets, reads the calibration set, pre-computes the gas heater set-point.
- **RAW logging, ground-side compensation.** Logs `SBIT_BME680` (0x02) as **8 raw register bytes** `press[3] temp[3] hum[2]`; the Bosch float compensation runs in `frameparser.py` using the **calibration frame** (see below). The on-device compensation code is kept only for the ~2 Hz bench echo.
- **Gas** logged separately as `SBIT_GAS` (0x20) — **2 raw bytes** `gas_r_msb, gas_r_lsb` (ADC + range + valid bits), emitted in POST/ARMED/COAST (interleaved ~1 s, only when the sensor flags it valid), OFF in BOOST.
- **Mode-adaptive profiles:** PAD (POST/ARMED) ~10 Hz T/P/H + gas · BOOST ~50 Hz T/P/H, IIR off, gas off (low-lag altitude) · COAST ~50 Hz T/P/H + gas.
- Forced-mode reads wait out the conversion (the gas heater needs ~100 ms). Validated vs references: pressure within ~1.5 hPa, humidity within ~3 %; temperature reads ~2 °C high (BME680 self-heating — subtract an offset in post if needed).

### Flight state machine (`flight_state.c` / `.h`)
- Autonomous **ARMED → BOOST → COAST**, driven by the MPU task:
  - **Launch:** ARMED → BOOST when the rolling-average |accel| ≥ `FLIGHT_LAUNCH_TRIP_G` (**1.90 g** — deliberately *below* the ±2 g full-scale rail of 1.99994 g, which a clean axial launch clips at without ever reaching 2.0).
  - **Burnout:** BOOST → COAST when avg |accel| < `FLIGHT_LAUNCH_G` (2 g, checked in ±16 g — no clipping), after ≥ `FLIGHT_MIN_BOOST_MS` in BOOST. COAST is terminal (no apogee/landing yet).
- **Boot resolution first:** `flight_state_preinit()` runs at the top of `app_main` (NVS restore + BOOT-hold re-arm check), so the SD logger can pick its file mode: **fresh ARMED session → new log (`wb`)**, **resumed BOOST/COAST after a power cut → append (`ab`)** — an in-flight brownout no longer truncates the recording.
- Rolling average over `FLIGHT_WINDOW_SAMPLES` (10 samples) — all thresholds are single `#define`s in `flight_state.h`.
- **Persistence:** the mode is written to **NVS** on every transition, so a power-cut mid-flight **resumes** the saved mode. Re-arm only by **holding BOOT ~1.5 s right after startup** (press *after* boot, not through the reset — that triggers ROM download mode).
- The MPU task also owns the **accel-range switch** (±16 g in BOOST / ±2 g else) and emits the **`SYSSTATE`** marker on each transition.

### SD logging: PSRAM pre-trigger buffer (`frame_logger.c`)
The flush-on-full double buffer was replaced with a **circular history buffer + a mode-aware SD writer**, so the launch onset is captured without hammering the card during the long armed wait:
- **Capture (all modes):** every sensor frame is appended to a **256 KB circular buffer** in **PSRAM** (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`, falling back to 32 KB internal RAM if PSRAM is off). It always retains the most-recent history, overwriting the oldest. Sensors now write here in *every* mode (they used to write to SD only in BOOST/COAST); telemetry (`hm_send`) still fires only in POST/ARMED.
- **ARMED/POST:** the writer only trickles a recent slice to SD every `LOG_SPARSE_MS` (10 s) — a light pad record, no card wear over hours of waiting.
- **On BOOST:** it rewinds by `LOG_PRETRIG_BYTES` (~10 s of history) and dumps that to SD, so the log holds the pre-launch idle **and** the launch onset that happened *before* detection fired — then streams continuously.
- **BOOST/COAST:** streams to SD; the big PSRAM buffer also absorbs SD write stalls (only skips oldest data if it ever laps the full 256 KB).
- Tunables are `#define`s at the top of the logger section: `LOG_RING_CAP`, `LOG_PRETRIG_BYTES`, `LOG_SPARSE_MS`, `LOG_STAGING`. Frames are byte-dumped (not frame-aligned); the parser re-syncs on the `0xAA 0xAA 0xAA` marker, so a partial leading frame is harmless.
- **Calibration frame first:** the logging task writes the stashed `SBIT_CALIB` frame at the head of every log session, so a recovered card is always self-describing. The same frame goes out over telemetry at POST and is re-sent every ~30 s while on the pad (late-attached ground station still gets it).
- **PSRAM is enabled** in sdkconfig (octal, 80 MHz; flash bumped 2 → 8 MB). If it ever fails to init, the buffer silently falls back to 32 KB internal.
- Fully exercised only with an SD card mounted (`CONFIG_ENABLE_SD_SPI`); with no card the writer task exits and the buffer just cycles in RAM.

### Status RGB LED (`status_led.c` / `.h`)
- The onboard **addressable WS2812 on GPIO38** shows the flight mode at a glance, driven straight off the RMT peripheral (`esp_driver_rmt`, no managed component): **blue** = POST/boot, **green (slow blink)** = ARMED/waiting, **red** = BOOST, **amber** = COAST, dim white = init/other.
- A small task mirrors `ctx->mode`; brightness kept low (~10–25 mA). GPIO38 is clear of the flash/PSRAM pin range, so it coexists with octal PSRAM.

### Bench-test scaffolding — **REMOVE BEFORE FLIGHT**
- Exercise the state machine on the bench by **shaking the board past 2 g** (→ BOOST) then holding it still (→ COAST); re-arm by holding BOOT ~1.5 s at boot.
- The reset-reason log-kill (`esp_log_level_set("*", ESP_LOG_NONE)`) is disabled so serial survives USB resets.
- Per-sensor debug `ESP_LOGI`s are enabled (BME680 prints a ~2 Hz, mode-labelled echo — note the echo is throttled, so it does **not** reflect the real sampling rate).
- **Live rate monitor** (`flight_stats.c`): a 1 Hz task logs the *achieved* samples/sec for every sensor, tagged with the flight mode — e.g. `[BOOST] mpu=80  ina=10  bme=28  gps=1  (Hz)`. This is the real per-mode rate (the per-sensor echoes are throttled and hide it). Each sensor task calls `flight_stats_tick()` on every produced sample.

### Telemetry transport (wired, for now)
- The S3 has **no Classic BT/SPP** (BLE-only), so the BT-disabled branch of `bt_serial_bridge.c` streams the `hm_send` telemetry over a wired UART instead — same frame format, off a wire. Init moved out of the power-on-only path so it runs on every reset.
- **Console vs. telemetry, board-agnostic:** console stays on the **standard default UART0/USB-serial path** (`idf.py monitor` works the same on any board — DevKitC, bare module, breadboard). Telemetry is a **separate wire, UART1 TX=GPIO10 @ 115200**, wired to an external USB-TTL adapter. (A DevKitC-only trick that instead put the console on native USB-Serial-JTAG and telemetry on UART0's native pins was tried and reverted — it broke once sensors moved to a breadboard build, since native-USB console depends on GPIO19/20 being wired to a real USB connector with correct D+/D- strapping, which a breadboard bring-up won't have.) The parser's CRC check stays on regardless, since a stray ROM-bootloader chirp or noise on either wire is still possible and should be dropped rather than crash the parser.
- The telemetry/`hm` path is active in **POST/ARMED**; BOOST/COAST high-rate data goes to the SD log instead, so the wire is quiet in flight modes (by design).
- Future: replace this one transport with an **ESP-NOW** link to a ground ESP32 that bridges to USB; the rest of the chain is unchanged.

## On-SD / over-BT frame format

Active writer: `main/frame_logger.c` (→ `/sd/fly.bin`). Authoritative parser: `SD-Parser/frameparser.py` (also reads the same frames live over the telemetry UART / future ESP-NOW link at 115200).

```
[0xAA 0xAA 0xAA] [type:u8] [timestamp_ms:u32 LE]  <payload>  [CRC16:2]
   3-byte sync      mode      4 bytes  (header = 8)            footer
```

| Sensor | type | payload | format |
|---|---|---|---|
| MPU6500 | 0x01 | 14 B | 7×int16 RAW (ax ay az temp gx gy gz); accel scale per flight mode |
| BME680 | 0x02 | 8 B | RAW registers press[3] temp[3] hum[2] — ground compensates |
| GPS | 0x04 | 69 B | NMEA fields (ASCII, 1 trailing pad byte) |
| INA219 | 0x08 | 4 B | RAW shunt:int16, bus:int16 |
| SYSSTATE | 0x10 | 1 B | uint8 flight mode — emitted on every mode change |
| GAS | 0x20 | 2 B | RAW gas_r_msb, gas_r_lsb (ADC + range + valid bits) |
| CALIB | 0x40 | 49 B | v1: version u8 · BME680 raw calib blob[40] · accel fs low/high u16 · gyro fs u16 · INA shunt mΩ u16 |

**All sensor payloads are RAW register values.** The `CALIB` (0x40) frame — written at the head of every SD session and re-sent over telemetry every ~30 s on the pad — carries every constant the ground needs; `frameparser.py` does the physics (Bosch BME680 float compensation, MPU scale factors, INA219 shunt math).

## Firmware ↔ parser ↔ dashboard alignment

Resolved — firmware, `SD-Parser/frameparser.py` and the dashboard agree:
1. **BME680 / GAS** — raw registers on the wire; `proc_bme680` / `proc_gas` compensate using the calib frame (pressure → kPa for the dashboard). Until a calib frame is seen, raw ADC values are emitted as `bme_adc` / `gas_adc`. ✅
2. **MPU accel scale** — firmware switches ±16 g (BOOST) / ±2 g (else) and emits a `SYSSTATE` (0x10) marker on each change; the parser tracks the mode and picks the LSB/g (defaults overridden by the calib frame). ✅
3. **MPU temperature** — parser uses the MPU6500 die formula (`/333.87 + 21`). ✅
4. **Dashboard** (`vanilla-websocket.py` → `ws-frameparser.js` → `index.html`) — schema extended with `gas_resistance`, `mode` and `calib_version`; new panels: Temperature / Pressure / Humidity / Gas / Flight Mode / **Calibration Frame** (latches to `RECEIVED v1` — until then T/P/H stay blank because the ground can't convert). `bin2json.py` inherits everything through `frameparser`. ✅
5. **CRC** — parser implements CRC-16/CCITT matching the firmware (the verify check is present but still commented out).

## Legacy / unused (cleanup candidates)

`bme280.c/.h`, `gps.c` (duplicate `app_main`), `sd_logger.c` (SDMMC CSV, not built), `i2c_basic_example_main.c`, `dual_core_blink.c`, root `health_monitoring(maybe).c`, the raw-sector code in `sdif.c`. Also `Embedded-Code/README.md` is still the stock ESP-IDF I²C example readme.
