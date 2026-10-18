# SD-PARSER

## Current support:
|MODULE|status|
|------|------|
|GPS   | ready|
|INA219 | ready |
|MPU6050 | ready |
|BME680 | ongoing... |

## Formating from binary to json

NOTE: The json file is over 10x larger than the binary file, for example: a 52M file can easily translate translate into more than 600MB.

```
python3 bin2json.py --source <path/to/bin/file> --output <path/to/json/file>
```

The json file is just the direct translation of the python dictionary in `vanilla-websocket.py`, variable `SCHEMA`:

``` json
[
  {
    "frame-status": "1", 
     "sys-timestamp-ms": 23378, 
     <sensor 0 information: ....>"
  }, 
  {
    "frame-status": "1",
    "sys-timestamp-ms": 24378,
    <sensor 1 information: ...>"
  },
  {
    ...
  },
  {}
]
```




## Calibrating the IMU

`imu_calibration.py` solves the accelerometer's offset and scale from a
recording of the payload held still in six poses, and the gyro's scale from
counted turns. The procedure is in `Documentation/CALIBRATION.md`.

```
python3 imu_calibration.py RX_0001.BIN --output calibration.json
python3 imu_calibration.py turns.bin --gyro-turns 10
```

The same solver is a block in the Filter Lab (`FilterLab/`), where you can also
build and compare filters on any recording without writing code.

## GPS coordinates

`json2telemetry.py` applies the hemisphere letters, so south and west come out
negative. Older Dashboard flights converted before Oct 2026 had positive
longitudes even west of Greenwich (no effect around Pisa, but EuRoC in Portugal
is 8.2° W): re-run `json2telemetry.py` for those.

## Running for health monitoring

1. (Ubuntu based) Run from file
``` bash
$ python3 vanilla-websocket.py --type file --source armed_file_gps.bin
```

​		it has an emulator of sampling rate, so the file is not dump entirely on the parser.

2. (Ubuntu based) run from serial

``` bash
$ python3 vanilla-websocket.py --type serial --source /dev/ttyUSB0
```

## Observation:

The payload has **no Bluetooth** any more (the ESP32-S3 has no Classic
Bluetooth, and the stack was removed in Oct 2026). Live data reaches a PC over
the wired telemetry UART — the DevKit's "UART" USB port, 115200 baud
(`/dev/ttyUSB0` on Linux, a `COMx` port on Windows) — and reaches the P4 ground
station over ESP-NOW. See `Embedded-Code/CHANGES.md`, "Telemetry transports".
