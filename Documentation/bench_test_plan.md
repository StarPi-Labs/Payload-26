# Pi-LOG Full Bench Test — Wiring & Procedure

Everything except the SD card (procedure for that at the end). Firmware state:
raw frames + calibration frame, flight state machine (trip 1.90 g), PSRAM
pre-trigger buffer, WS2812 status LED, telemetry on UART0/GPIO10.

## 1. Wiring

All sensor breakouts run on **3V3**. Common ground everywhere.
No pin below touches flash (26–32) or octal PSRAM (33–37); GPIO19/20 = native USB.

| Device | Signal | ESP32-S3 GPIO | Notes |
|---|---|---|---|
| **MPU6050** (I2C0, `0x68`) | SDA | **8** | 400 kHz |
| | SCL | **9** | |
| | INT | **4** | data-ready interrupt (required — drives the sample loop) |
| **BME680** (I2C1, `0x76/0x77`) | SDA | **13** | shares bus 1 with INA219 |
| | SCL | **14** | |
| **INA219** (I2C1, `0x40`) | SDA | **13** | same wires as BME680 |
| | SCL | **14** | |
| | VIN+ / VIN− | — | only needed to measure real current: put the shunt in series with a load (e.g. 5 V → VIN+ , VIN− → GPS VCC). Left open ⇒ current reads ~0 mA — that's fine. |
| **SAM-M10Q GPS** (UART2, 9600) | GPS **RX** | **6** (ESP TX) | crossed! |
| | GPS **TX** | **7** (ESP RX) | |
| **Telemetry** (UART0, 115200) | USB-TTL **RX** | **10** (ESP TX) | + GND↔GND. TX-only link |
| **WS2812 LED** | data | 38 | onboard, no wiring |
| **BOOT button** | — | 0 | onboard, used for re-arm |
| *(SD, later)* | MOSI | 17 | |
| | MISO | 16 | |
| | SCLK | 18 | |
| | CS | 5 | |

Two COM ports will exist on the PC: the ESP's **native USB** (flash + serial
monitor) and the **USB-TTL adapter** (telemetry). Don't mix them up.

## 2. Flash

VS Code ESP-IDF extension: Build → Flash → Monitor (target esp32s3).
INA219 + GPS were re-enabled in sdkconfig — flash the fresh build.

## 3. Expected boot sequence (serial monitor)

1. Banner `Star-PI Payload v2.0`
2. `flight: hold BOOT now (~1500 ms) to re-arm...` (1.5 s window each boot)
3. `resumed mode=ARMED from NVS` (first boot: NVS default = ARMED)
4. I2C buses ready, `IMU online, WHO_AM_I = 0x70`, BME680 `calibration parsed`,
   `INA219 initialised @ 0x40`, GPS probe OK
5. `calibration frame ready (49 B)`
6. `frame_logger: log ring = 262144 B in PSRAM` ← **PSRAM proof**
7. (no SD): SD mount fails, logging task not started — expected
8. `flight mode = ARMED (BOOST at >= 1.90 g ...)`
9. 1 Hz rates: `[ARMED] mpu=80 ina=10 bme=10 gps=1 (Hz)`
10. **LED**: blue flicker (POST) → **green slow blink (ARMED)**

GPS indoors: `status=V`, 0 sats — normal. Take it near a window for a fix.

## 4. Dashboard test

```powershell
cd SD-Parser
python vanilla-websocket.py --source COM<TTL-adapter> --type serial
# browser -> http://127.0.0.1:5000/
```

Expect: LINK_STATE ACTIVE → within ≤30 s **Calibration Frame: RECEIVED v1**
(re-sent every 30 s on the pad, so a late-started dashboard still syncs) →
T/P/H panels populate (they stay `0.00` until the calib frame arrives — the
ground can't convert raw registers without it) → gas after a few heater
cycles → accel/gyro/power/GPS panels live → Flight Mode: ARMED.

The bridge also records the raw stream to `armed_file.bin`; validate the
offline chain afterwards with
`python bin2json.py --source armed_file.bin --output bench.json`.

## 5. State machine test

| Step | Action | Expect (serial / LED / dashboard) |
|---|---|---|
| Launch | Shake board hard (>1.9 g sustained ~10 samples) | `state -> BOOST`, `accel range -> +/-16g` / LED **red** / dashboard **freezes** — by design, telemetry is quiet in flight; rates line shows `[BOOST]`, bme ≈ 50 Hz |
| Burnout | Hold still > 0.3 s | `state -> COAST` / LED **amber** / rates `[COAST]`, gas resumes ~1 Hz |
| Power cut | Pull USB while in COAST, replug | boots straight to `resumed mode=COAST from NVS`, LED amber, **no re-arm** |
| Re-arm | Tap EN (or replug), then **within 1.5 s press and hold BOOT ~2 s** | `BOOT held -> re-armed to ARMED`, LED green blink, telemetry + dashboard live again |

⚠ Never hold BOOT *while* pressing EN/replugging — that straps the chip into
ROM download mode (app won't start). Press it only after release, inside the
1.5 s window (the serial line tells you when).

Dashboard freezing in BOOST/COAST is **correct**: hm/telemetry only runs in
POST/ARMED; flight data goes to the PSRAM ring (→ SD when a card is present).

## 6. When the SD card arrives

FAT32 card, wiring per table above. Then expect additionally:
- boot: `calibration frame written (49 B payload)` at the head of `/sd/fly.bin`
- ARMED: sparse writes every 10 s (LOG_SPARSE_MS)
- on BOOST: `launch: dumping N B pre-trigger, then streaming`
- fresh ARMED boot truncates the file (`wb`); resumed BOOST/COAST appends (`ab`)
- post-test: `python bin2json.py --source <fly.bin> --output flight.json`

## Known bench caveats
- BME680 temp reads ~2 °C high (self-heat) — corrected in analysis.
- INA219 current ≈ 0 unless the shunt is in a real load path.
- First gas value takes a few seconds (heater stabilisation), ARMED/COAST only.
- MPU must be the working replacement unit (original is damaged).
