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
- Replaces the old stub. Auto-detects `0x76`/`0x77`, verifies chip-id `0x61`, soft-resets, reads + dumps the 40-byte calibration blob, pre-computes the gas heater set-point.
- **Raw logging** (offline compensation). Fixed **10-byte** payload: `press[3] temp[3] hum[2] gas[2]`.
- **Mode-adaptive profiles:** PAD (POST/ARMED) ~10 Hz T/P/H, gas off · BOOST ~50 Hz T/P/H, IIR off, gas off · COAST ~50 Hz T/P/H **+ a gas measurement every ~1 s** (interleaved so altitude stays fast).
- Forced-mode reads wait out the conversion (the gas heater needs ~100 ms) before reading.

### Bench-test scaffolding — **REMOVE BEFORE FLIGHT**
- `main.c` boots into `MODE_ARMED`; the **BOOT button (GPIO0)** cycles ARMED → BOOST → COAST to watch each sensor's per-mode behaviour live.
- The reset-reason log-kill (`esp_log_level_set("*", ESP_LOG_NONE)`) is disabled so serial survives USB resets.
- Per-sensor debug `ESP_LOGI`s are enabled (BME680 prints a ~2 Hz, mode-labelled echo — note the echo is throttled, so it does **not** reflect the real sampling rate).
- **Live rate monitor** (`flight_stats.c`): a 1 Hz task logs the *achieved* samples/sec for every sensor, tagged with the flight mode — e.g. `[BOOST] mpu=80  ina=10  bme=28  gps=1  (Hz)`. This is the real per-mode rate (the per-sensor echoes are throttled and hide it). Each sensor task calls `flight_stats_tick()` on every produced sample.

## On-SD / over-BT frame format

Active writer: `main/frame_logger.c` (→ `/sd/fly.bin`). Authoritative parser: `SD-Parser/frameparser.py` (also reads the same frames over Bluetooth at 115200).

```
[0xAA 0xAA 0xAA] [type:u8] [timestamp_ms:u32 LE]  <payload>  [CRC16:2]
   3-byte sync      mode      4 bytes  (header = 8)            footer
```

| Sensor | type | payload | format |
|---|---|---|---|
| MPU6500 | 0x01 | 14 B | 7×int16 (ax ay az temp gx gy gz) |
| BME680 | 0x02 | **10 B (raw)** | press[3] temp[3] hum[2] gas[2] |
| GPS | 0x04 | 68 B | NMEA fields (ASCII) |
| INA219 | 0x08 | 4 B | shunt:int16, bus:int16 |
| SYSSTATE | 0x10 | — | (not yet emitted) |
| BME680 calib | 0x20 | **40 B**, once | calib blob for offline compensation |

## ⚠️ Firmware ↔ parser mismatches to resolve (`SD-Parser/frameparser.py`)

1. **BME680:** parser expects 12 B / 3 floats (compensated); firmware now sends **10 B raw** + a **40 B calib** frame (0x20). Parser needs to decode raw and apply the Bosch compensation using the calib blob (incl. gas).
2. **MPU accel scale (now mode-dependent):** firmware uses ±16 g (2048 LSB/g) in BOOST and ±2 g (16384 LSB/g) in every other phase. The parser hard-codes 2048 LSB/g, so it is only correct for BOOST frames — it must choose the divisor from the frame's flight phase (needs #5).
3. **MPU temperature:** parser uses the MPU6050 formula; chip is an MPU6500 (`/333.87 + 21`).
4. **CRC:** firmware computes CRC-16/CCITT; parser verification is stubbed (`return 0xFFFF`).
5. **No per-frame mode tag:** frames don't record which flight mode produced them, so the parser can't know the accel range or that a BME680 temp sample is heater-biased (gas cycles). Consider a periodic `SYSSTATE` (0x10) frame carrying `context.mode`.

## Legacy / unused (cleanup candidates)

`bme280.c/.h`, `gps.c` (duplicate `app_main`), `sd_logger.c` (SDMMC CSV, not built), `i2c_basic_example_main.c`, `dual_core_blink.c`, root `health_monitoring(maybe).c`, the raw-sector code in `sdif.c`. Also `Embedded-Code/README.md` is still the stock ESP-IDF I²C example readme.
