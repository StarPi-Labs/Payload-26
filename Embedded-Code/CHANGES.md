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
- **On-device Bosch compensation.** Logs `SBIT_BME680` (0x02) as **3 floats** `{temperature °C, pressure Pa, humidity %RH}` — matches the protocol, so no parser change needed for T/P/H.
- **Gas** logged separately as `SBIT_GAS` (0x20) — **1 float = resistance (ohm)**, emitted in POST/ARMED/COAST (interleaved ~1 s, only when the sensor flags it valid), OFF in BOOST.
- **Mode-adaptive profiles:** PAD (POST/ARMED) ~10 Hz T/P/H + gas · BOOST ~50 Hz T/P/H, IIR off, gas off (low-lag altitude) · COAST ~50 Hz T/P/H + gas.
- Forced-mode reads wait out the conversion (the gas heater needs ~100 ms). Validated vs references: pressure within ~1.5 hPa, humidity within ~3 %; temperature reads ~2 °C high (BME680 self-heating — subtract an offset in post if needed).

### Bench-test scaffolding — **REMOVE BEFORE FLIGHT**
- `main.c` boots into `MODE_ARMED`; the **BOOT button (GPIO0)** cycles ARMED → BOOST → COAST to watch each sensor's per-mode behaviour live.
- The reset-reason log-kill (`esp_log_level_set("*", ESP_LOG_NONE)`) is disabled so serial survives USB resets.
- Per-sensor debug `ESP_LOGI`s are enabled (BME680 prints a ~2 Hz, mode-labelled echo — note the echo is throttled, so it does **not** reflect the real sampling rate).
- **Live rate monitor** (`flight_stats.c`): a 1 Hz task logs the *achieved* samples/sec for every sensor, tagged with the flight mode — e.g. `[BOOST] mpu=80  ina=10  bme=28  gps=1  (Hz)`. This is the real per-mode rate (the per-sensor echoes are throttled and hide it). Each sensor task calls `flight_stats_tick()` on every produced sample.

### Telemetry transport (wired, for now)
- The S3 has **no Classic BT/SPP** (BLE-only), so the BT-disabled branch of `bt_serial_bridge.c` now streams the `hm_send` telemetry over **UART1 (TX=GPIO10) @ 115200** instead of SPP — same frame format, off a wire. Init moved out of the power-on-only path so it runs on every reset.
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
| MPU6500 | 0x01 | 14 B | 7×int16 (ax ay az temp gx gy gz); accel scale per flight mode |
| BME680 | 0x02 | 12 B | 3×float (temperature °C, pressure Pa, humidity %RH) |
| GPS | 0x04 | 68 B | NMEA fields (ASCII) |
| INA219 | 0x08 | 4 B | shunt:int16, bus:int16 |
| SYSSTATE | 0x10 | 1 B | uint8 flight mode — emitted on every mode change |
| GAS | 0x20 | 4 B | 1×float (BME680 gas resistance, ohm) |

## Firmware ↔ parser alignment

Resolved — firmware and `SD-Parser/frameparser.py` now agree:
1. **BME680** — compensated on-device into 3 floats; parser `proc_bme680` reads them (pressure → kPa for the dashboard). ✅
2. **GAS** — new `0x20` packet; parser `proc_gas` (resistance, ohm). ✅
3. **MPU accel scale** — firmware switches ±16 g (BOOST) / ±2 g (else) and emits a `SYSSTATE` (0x10) marker on each change; parser `proc_sysstate` tracks the mode and `proc_mpu6050` picks 2048 vs 16384 LSB/g accordingly. ✅
4. **CRC** — parser implements CRC-16/CCITT matching the firmware (the verify check is present but still commented out).

Still open:
- **MPU temperature:** parser uses the MPU6050 formula (`/340 + 36.53`); chip is an MPU6500 (`/333.87 + 21`).
- **GPS `+1` patch:** `frameparser.py` still does `total_packet_sz += 1` for GPS (flagged "DEPRECATED"). The firmware now sends 68 B in all paths, so that `+1` should be removed.

## Legacy / unused (cleanup candidates)

`bme280.c/.h`, `gps.c` (duplicate `app_main`), `sd_logger.c` (SDMMC CSV, not built), `i2c_basic_example_main.c`, `dual_core_blink.c`, root `health_monitoring(maybe).c`, the raw-sector code in `sdif.c`. Also `Embedded-Code/README.md` is still the stock ESP-IDF I²C example readme.
