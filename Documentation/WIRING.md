# Pi-LOG Payload — Pin-to-Pin Wiring

Every connection below is taken from the firmware as committed, not from a
schematic. Pin numbers come from `Embedded-Code/sdkconfig` through the Kconfig
options referenced in [`main/sensor_config.h`](../Embedded-Code/main/sensor_config.h).
Each row names the file and line it came from, so it can be re-checked after any
`menuconfig` change.

- **MCU:** ESP32-S3-WROOM-1 **N8R8** (8 MB flash, 8 MB octal PSRAM)
- **Firmware tree:** commit `c023f37`
- **Generated:** 2026-10-15

> If you change a pin in `idf.py menuconfig` → *Star-PI Payload Configuration*,
> this table is out of date. The macros are indirections; `sdkconfig` holds the
> real values.

---

## 1. Everything at a glance

| ESP32-S3 pin | Goes to | Function | Source |
|---|---|---|---|
| GPIO8 | MPU6500 `SDA` | I2C0 data, 400 kHz | `sdkconfig` `CONFIG_I2C0_MASTER_SDA` |
| GPIO9 | MPU6500 `SCL` | I2C0 clock | `CONFIG_I2C0_MASTER_SCL` |
| GPIO4 | MPU6500 `INT` | data-ready interrupt | `sensor_config.h:32` |
| GPIO13 | BME680 `SDA` + INA219 `SDA` | I2C1 data, 400 kHz | `CONFIG_I2C1_MASTER_SDA` |
| GPIO14 | BME680 `SCL` + INA219 `SCL` | I2C1 clock | `CONFIG_I2C1_MASTER_SCL` |
| GPIO6 | GPS `RX` | UART2 TX (ESP → GPS) | `CONFIG_GPS_TXD_PIN`, `main.c:394` |
| GPIO7 | GPS `TX` | UART2 RX (GPS → ESP) | `CONFIG_GPS_RXD_PIN`, `main.c:394` |
| GPIO5 | microSD `CS` | SPI2 chip select | `CONFIG_SD_SPI_CS_PIN` |
| GPIO18 | microSD `SCK` | SPI2 clock | `CONFIG_SD_SPI_SCLK_PIN` |
| GPIO17 | microSD `MOSI` / `DI` | SPI2 MOSI | `CONFIG_SD_SPI_MOSI_PIN` |
| GPIO16 | microSD `MISO` / `DO` | SPI2 MISO | `CONFIG_SD_SPI_MISO_PIN` |
| GPIO38 | WS2812 `DIN` | status LED (RMT) | `status_led.c:16` |
| GPIO0 | BOOT button | re-arm input, active low | `flight_state.c:21` |
| GPIO43 (UART0 TX) | DevKit "UART" USB port (CP210x) | wired telemetry, 115200 baud | `telemetry_uart.c` |
| GPIO19/20 (USB) | DevKit "USB" port | console (USB-Serial-JTAG) | `sdkconfig` |

**Bus assignment matters:** the MPU6500 is alone on **I2C0**, while the BME680
and INA219 **share I2C1**. A fault on I2C1 takes out both of those sensors and
leaves the IMU untouched — which is exactly the failure pattern seen in the
`fly.bin` analysis.

---

## 2. MPU6500 (silkscreened "MPU6050") — I2C0

Address **0x68** (`mpu6050.c:22`), `WHO_AM_I` reads **0x70** (MPU6500 silicon).

| Module pin | ESP32-S3 | Note |
|---|---|---|
| `VCC` | 3V3 | module regulator accepts 3.3 V on GY-521 boards |
| `GND` | GND | |
| `SDA` | **GPIO8** | external pull-up required, see §8 |
| `SCL` | **GPIO9** | external pull-up required |
| `INT` | **GPIO4** | **must be connected** — see below |
| `AD0` | GND / open | low selects 0x68; GY-521 pulls it low on board |
| `XDA`, `XCL`, `NCS` | — | leave open |

The `INT` line is not optional decoration: `mpu6050.c:57-65` configures GPIO4 as
an input with a pull-up and a rising-edge interrupt, and `mpu6050.c:311`
registers an ISR that notifies the sampling task. Sample rate divider `0x0b`
gives **83.33 Hz**, DLPF 21 Hz, gyro ±500 dps, accel ±2 g on the pad and ±16 g
in BOOST.

---

## 3. BME680 — I2C1

Address **0x76** first, falling back to **0x77** (`bme680.c:302`); chip ID 0x61.

| Module pin | ESP32-S3 | Note |
|---|---|---|
| `VIN` / `VCC` | 3V3 | |
| `GND` | GND | |
| `SDI` / `SDA` | **GPIO13** | shared with the INA219 |
| `SCK` / `SCL` | **GPIO14** | shared with the INA219 |
| `SDO` | GND → 0x76, 3V3 → 0x77 | either works, the driver probes both |
| `CS` | 3V3 | **required for I2C mode** on most breakouts; Adafruit boards pull it high already |

Cadence: 100 ms on the pad (`BME680_PERIOD_PAD_MS`), 20 ms in BOOST/COAST
(`BME680_PERIOD_FLIGHT_MS`), gas heater 320 °C once per second
(`BME680_GAS_INTERVAL_MS`).

> The task gives up permanently after 60 consecutive failed reads
> (`bme680.c:365` → `vTaskDelete`). There is no re-init path, so a flaky I2C1
> connection costs you every pressure sample for the rest of the flight.

---

## 4. INA219 — I2C1

Address **0x40** (`ina219.c:29`), shunt **100 mΩ**, sampled at 10 Hz
(`INA219_PERIOD_default`).

| Module pin | ESP32-S3 / rail | Note |
|---|---|---|
| `VCC` | 3V3 | logic supply |
| `GND` | GND | |
| `SDA` | **GPIO13** | shared with the BME680 |
| `SCL` | **GPIO14** | shared with the BME680 |
| `A0`, `A1` | GND / open | both low → 0x40 |
| `VIN+` | upstream side of the rail being measured | high side |
| `VIN−` | downstream side (the load) | |

`VIN+`/`VIN−` go **in series with the load**, not to ground. The bench plan
suggests 5 V → `VIN+`, `VIN−` → GPS `VCC`
([bench_test_plan.md:21](bench_test_plan.md)); the 20 h recording measured
**4.96 V and 65.5 mA median**, so as wired it sits in the 5 V path. Leaving
`VIN+`/`VIN−` open makes the current read ≈ 0 mA, which is harmless but useless.

---

## 5. GPS — SAM-M10Q, UART2 @ 9600 8N1

| Module pin | ESP32-S3 | Note |
|---|---|---|
| `VCC` | 3V3 | fed through the INA219 shunt in the bench wiring |
| `GND` | GND | |
| `RX` | **GPIO6** | ESP transmits here (`uart_set_pin` tx argument) |
| `TX` | **GPIO7** | ESP receives here |

Cross the pair: ESP TX → module RX, module TX → ESP RX. Baud is fixed at 9600,
with no runtime upgrade (`main.c:458`). 1 Hz fix rate; non-essential NMEA
sentences are disabled after a successful probe.

---

## 6. microSD — SPI2_HOST

| Module pin | ESP32-S3 |
|---|---|
| `CS` | **GPIO5** |
| `SCK` | **GPIO18** |
| `MOSI` / `DI` | **GPIO17** |
| `MISO` / `DO` | **GPIO16** |
| `VCC` | 3V3 or 5 V — depends on whether your breakout has a regulator |
| `GND` | GND |

Mounted at `/sd`, log file `/sd/fly.bin` (`sensor_config.h:94-95`). The 1-bit
SDMMC path is **disabled** (`CONFIG_ENABLE_SD_SDIO` is not set), so
`PIN_SD_CLK` / `PIN_SD_CMD` / `PIN_SD_D0` are unused — ignore them when wiring.

---

## 7. Status LED and BOOT button

| Part | ESP32-S3 | Note |
|---|---|---|
| WS2812 `DIN` | **GPIO38** | single addressable LED, RMT driven at 10 MHz (`status_led.c:16`) |
| WS2812 `VCC` / `GND` | 5 V or 3V3 / GND | 3.3 V data usually works at 5 V supply, but 3V3 supply is safest |
| BOOT button | **GPIO0** | to GND, internal pull-up enabled (`flight_state.c:81-82`) |

LED colours map to flight mode: blue POST, green ARMED, red BOOST, amber COAST.

Pulling GPIO0 low **during reset** puts the chip in download mode. For the
re-arm press, release reset first, then pull GPIO0 low within
`FLIGHT_REARM_WAIT_MS` and hold for `FLIGHT_REARM_HOLD_MS`.

---

## 8. Gotchas worth checking before you solder

1. **Internal I2C pull-ups are switched off** — `i2c_bus.c:82` sets
   `flags.enable_internal_pullup = false`. Both buses need **external 4.7 kΩ
   pull-ups to 3.3 V** on SDA and SCL. Without them the bus may appear to work
   and then fail under temperature or load.
2. **GPIO26–37 are unusable** on the N8R8 — SPI flash and the octal PSRAM own
   them — and **GPIO19/20** are the native USB pins. That is why the MPU
   interrupt lives on GPIO4 (see the note at `sensor_config.h:32`, and the pin
   map in `Embedded-Code/CHANGES.md`).
3. **GPIO0 is a strapping pin.** Anything that holds it low at power-up blocks
   normal boot.
4. **UART0 (GPIO43/44) is the wired telemetry**, routed to the DevKit's "UART"
   USB port; the console is on USB-Serial-JTAG (the "USB" port). Don't wire a
   second device to GPIO43/44.
5. **One shared ground** across every module, and keep the SPI runs to the SD
   card short — SPI at 3.3 V over long jumpers is the usual cause of mount
   failures.
6. **These files do *not* define the live pin map** and are not compiled
   (`main/CMakeLists.txt`): `gps.c`, `bme280.c`, `dual_core_blink.c`,
   `i2c_basic_example_main.c`. The compiled GPS path is
   `gps_sensor.c` + `uart.c` + `nmea.c`.

---

## 9. Diagram

```
                        ESP32-S3-WROOM-1 N8R8
                     +-------------------------+
   MPU6500  SDA  <-> | GPIO8                   |
   (0x68)   SCL  <-> | GPIO9       I2C0        |
            INT  --> | GPIO4                   |
                     |                         |
   BME680   SDA  <-> | GPIO13                  |
   (0x76)   SCL  <-> | GPIO14      I2C1        |
   INA219   SDA  <-> |   (same two pins)       |
   (0x40)   SCL  <-> |                         |
                     |                         |
   GPS      RX   <-- | GPIO6       UART2       |
   SAM-M10Q TX   --> | GPIO7       9600 8N1    |
                     |                         |
   microSD  CS   <-- | GPIO5                   |
            SCK  <-- | GPIO18      SPI2        |
            MOSI <-- | GPIO17                  |
            MISO --> | GPIO16                  |
                     |                         |
   WS2812   DIN  <-- | GPIO38      RMT         |
   BOOT btn      --> | GPIO0       (to GND)    |
                     +-------------------------+

   Power:  3V3 -> MPU6500, BME680, INA219 logic, GPS
           5 V -> INA219 VIN+/VIN- in series with the measured load
           one common GND for everything
```

## 10. Current draw, for the supply side

Measured on the INA219's rail over the 20 h bench run, and converted to a 16 V
input through a buck at 85 % efficiency:

| | on the 5 V rail | at 16 V in |
|---|---|---|
| typical | 65.5 mA / 0.33 W | ~25 mA |
| peak observed | 148 mA / 0.74 W | ~55 mA |

Rate the buck and any fuse for **0.5 A at 16 V**. Both figures predate the
ESP-NOW telemetry firmware, so expect roughly double the average once the radio
is transmitting.
