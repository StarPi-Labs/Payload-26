# Sensor Data Format Guide

## Expected TXT File Format

Your sensor data TXT file from the SD card should follow this format (comma-separated values):

```
time,altitude,altitudeMSL,velocity,horizontalVelocity,acceleration,accelerationX,accelerationY,accelerationZ,temperature,pressure,humidity,gpsLat,gpsLon,gpsAlt,pitch,roll,yaw
0.0,0,12,0,0,0,0,0,0,22.5,101.3,45,37.7749,-122.4194,15,0,0,0
0.1,5,17,10,2,9.8,9.5,0.5,0.3,22.4,101.2,44,37.7750,-122.4193,15,2,1,5
0.2,15,27,25,5,12.5,12.0,1.2,0.8,22.3,101.1,44,37.7751,-122.4192,16,4,2,10
...
```

### Column Descriptions

| Column | Description | Unit |
|--------|-------------|------|
| time | Timestamp from launch | seconds |
| altitude | Height above the pad (barometric, referenced to the first pressure sample) | meters |
| altitudeMSL | Height above sea level (altitude + departure/launch-site elevation) | meters |
| velocity | Vertical velocity | m/s |
| horizontalVelocity | Horizontal velocity | m/s |
| acceleration | Total acceleration magnitude | m/s² |
| accelerationX | X-axis acceleration | m/s² |
| accelerationY | Y-axis acceleration | m/s² |
| accelerationZ | Z-axis acceleration | m/s² |
| temperature | Ambient temperature | °C |
| pressure | Atmospheric pressure | kPa |
| humidity | Relative humidity | % |
| gpsLat | GPS latitude | degrees |
| gpsLon | GPS longitude | degrees |
| gpsAlt | GPS altitude | meters |
| pitch | Pitch angle | degrees |
| roll | Roll angle | degrees |
| yaw | Yaw angle | degrees |

## Metadata Lines

Optional `#key=value` lines before the header are parsed and returned via
the `/api/flights/<id>/telemetry` endpoint's `meta` object:

```
# targetAltitude=3000.0
# departureAltitude=12.0
# t0=2026-07-23T14:02:11+00:00
# modeTransitions=0.000:INIT,0.412:POST,2.100:ARMED,5.884:BOOST,9.220:COAST
```

`modeTransitions` marks each point where the flight's state machine changed
mode: a comma-separated list of `time:mode` pairs (`time` in seconds from
launch, `mode` one of `INIT`/`POST`/`SNSCHK`/`ARMED`/`BOOST`/`COAST`, matching
the firmware enum in `Embedded-Code/main/systemp2i.h`). `json2telemetry.py`
computes this automatically from SYSSTATE frames; the Dashboard renders it as
tick marks on the timeline scrubber.

## Flexible Parsing

The server supports:
- **Delimiters**: comma (`,`), semicolon (`;`), tab, or space
- **Headers**: Optional first row with column names (auto-detected)
- **Partial data**: Missing columns will be filled with defaults

## Minimal Format

If you have limited sensors, you can use a simpler format:

```
time,altitude,altitudeMSL,velocity,acceleration,temperature
0.0,0,12,0,0,22.5
0.1,5,17,10,9.8,22.4
```

Missing columns will default to zero (for numeric) or standard values (101.3 for pressure, 50 for humidity, etc.)

## Producing this file from a flight capture

`SD-Parser/json2telemetry.py` builds this format directly from a decoded
flight capture (`SD-Parser/bin2json.py`'s JSON output): it forward-fills
each sensor's last known value into one merged row per instant, converts
units to match this schema (acceleration g→m/s², horizontal velocity
km/h→m/s), derives `altitude`/`velocity`/`altitudeMSL` from the BME680
pressure trace, and back-fills GPS position for any samples before the
first fix. By default it writes straight into this Dashboard's `data/`
folder (see below) rather than a standalone file — pass `--output` to get
a plain `.txt` file instead.

## File Naming

- Name your files descriptively: `sensor_data.txt`, `flight_log.txt`, etc.
- Multiple files per flight are supported and will be merged

## SD Card / Flight Folder Structure

The server looks for flights as `data/<YYYY-MM-DD>/`, each containing:
- `telemetry/` — `.txt`, `.csv`, or `.log` files (merged and sorted by time)
- `videos/` — `.mp4`, `.avi`, `.mov`, or `.mkv` files

`json2telemetry.py` creates both subfolders for you (the `videos/` one
empty, ready for your camera footage) — see its `--date` option.
