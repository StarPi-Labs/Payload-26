#!/usr/bin/env python3
"""
json_to_telemetry.py -- bridge between the ground-station JSON produced by
bin2json.py (one dict per decoded sensor frame, already physically converted
by frameparser.py) and the flat per-instant schema the Star-PI Dashboard
backend expects (Dashboard/Backend/SENSOR_FORMAT.md):

    time,altitude,altitudeMSL,velocity,horizontalVelocity,acceleration,
    accelerationX,accelerationY,accelerationZ,temperature,pressure,humidity,
    gpsLat,gpsLon,gpsAlt,pitch,roll,yaw

This exact order matters -- Dashboard/Backend/server.py parses it
positionally. If you add/reorder columns here, update server.py to match.

Each raw frame only carries the fields for ONE sensor (e.g. an MPU frame has
accelerationX/Y/Z but no temperature). This script walks the frames in time
order and forward-fills a single merged state, emitting one row per frame --
the flat per-timestamp series the dashboard's graphs expect.

It also derives what the raw stream cannot measure directly:
  - altitude [m]    : barometric, referenced to the first pressure sample
                       seen (so it reads ~0 on the pad and rises in flight)
  - velocity [m/s]  : vertical speed, first difference of that altitude
                       (reads ~0 if the payload never actually gained/lost
                       altitude during the capture -- that is not a bug)
  - altitudeMSL [m] : altitude + --departure-altitude, i.e. the barometric
                       reading translated to true sea-level altitude
These are simple estimates for a quick review, not a sensor-fused solution.

GPS position (gpsLat/gpsLon/gpsAlt) is forward-filled sample to sample as
usual, AND back-filled: every row before the first fix is set to the first
fix's position too, on the assumption the payload had not moved yet (it was
still on the pad/bench) -- so the track does not appear to teleport in from
(0, 0) the instant the receiver locks.

Units are also converted to match SENSOR_FORMAT.md exactly:
  - acceleration, accelerationX/Y/Z : g (frameparser's unit) -> m/s^2
  - horizontalVelocity              : km/h (frameparser's unit) -> m/s

Note: 'pitch'/'roll'/'yaw' are passed through as-is. In the current protocol
they are gyroscope angular RATES in deg/s (there is no attitude/orientation
estimator on board), not integrated Euler angles -- keep that in mind when
reading the dashboard's attitude display.

If --target-altitude / --departure-altitude are not given, the script asks
for them interactively (press Enter to skip).

By default this writes directly into the Dashboard's data folder, creating
the whole flight folder (telemetry/ + an empty videos/ ready for your camera
files) so the flight already shows up for the backend -- named after
--date (YYYY-MM-DD), or today's date if you don't pass one. Pass --output to
instead write a single flat .txt file anywhere, bypassing the Dashboard
folder entirely.

Usage:
    python json2telemetry.py --source flight.json
    python json2telemetry.py --source flight.json --date 2026-07-23 \\
        --target-altitude 3000 --departure-altitude 12
    python json2telemetry.py --source flight.json --output flight.txt
"""
import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

# Exact order Dashboard/Backend/server.py expects (positional parsing) --
# do not reorder without updating parse_sensor_file_with_meta() there too.
SCHEMA_FIELDS = [
    "time", "altitude", "altitudeMSL", "velocity", "horizontalVelocity",
    "acceleration", "accelerationX", "accelerationY", "accelerationZ",
    "temperature", "pressure", "humidity",
    "gpsLat", "gpsLon", "gpsAlt", "pitch", "roll", "yaw",
]

# SD-Parser/json2telemetry.py -> ../Dashboard/Backend/data
DASHBOARD_DATA_DIR = Path(__file__).resolve().parent.parent / "Dashboard" / "Backend" / "data"

G = 9.80665            # standard gravity: g -> m/s^2
KMH_TO_MS = 1.0 / 3.6


def load_frames(path):
    """Load a bin2json.py output file. Tolerates the older minified format
    (one giant line, trailing empty-dict sentinel) and the newer pretty one."""
    with open(path, "r") as f:
        raw = json.load(f)
    frames = [d for d in raw
              if isinstance(d, dict) and "sys-timestamp_ms" in d
              and d.get("frame-status", "1") != "0"]
    frames.sort(key=lambda d: d["sys-timestamp_ms"])
    return frames


def convert(frames, target_altitude=None, departure_altitude=0.0):
    t0_ms = frames[0]["sys-timestamp_ms"]

    # Anchor onboard millis to a real UTC time using the first GPS fix seen.
    t0_epoch_ms = None
    for d in frames:
        if d.get("gpsLock") == "A" and "time" in d:
            t0_epoch_ms = int(d["time"] * 1000) - (d["sys-timestamp_ms"] - t0_ms)
            break

    state = {k: 0.0 for k in SCHEMA_FIELDS}
    state["temperature"], state["pressure"], state["humidity"] = 20.0, 101.3, 50.0
    ground_pressure = None
    last_alt, last_t = None, None
    first_gps_row = None   # index of the first row with a real GPS fix
    rows = []

    for d in frames:
        t_s = (d["sys-timestamp_ms"] - t0_ms) / 1000.0

        if "accelerationX" in d:
            state["accelerationX"] = d["accelerationX"] * G
            state["accelerationY"] = d["accelerationY"] * G
            state["accelerationZ"] = d["accelerationZ"] * G
            state["acceleration"] = d["acceleration"] * G
        if "roll" in d:
            state["roll"] = d["roll"]
            state["pitch"] = d["pitch"]
            state["yaw"] = d["yaw"]
        if "temperature" in d:
            state["temperature"] = d["temperature"]
        if "pressure" in d:
            state["pressure"] = d["pressure"]
            if ground_pressure is None:
                ground_pressure = d["pressure"]
        if "humidity" in d:
            state["humidity"] = d["humidity"]
        if "gpsLat" in d:
            state["gpsLat"] = d["gpsLat"]
            state["gpsLon"] = d["gpsLon"]
            state["gpsAlt"] = d.get("gpsAlt", state["gpsAlt"])
            if first_gps_row is None:
                first_gps_row = len(rows)
        if "horizontalVelocity" in d:
            state["horizontalVelocity"] = d["horizontalVelocity"] * KMH_TO_MS

        # Barometric altitude above the ground reference (first pressure sample).
        if ground_pressure and state["pressure"] > 0:
            ratio = state["pressure"] / ground_pressure
            state["altitude"] = 44330.0 * (1.0 - ratio ** (1.0 / 5.255))
        state["altitudeMSL"] = state["altitude"] + departure_altitude

        # Vertical speed: first difference of the altitude estimate.
        if last_alt is not None and t_s > last_t:
            state["velocity"] = (state["altitude"] - last_alt) / (t_s - last_t)
        last_alt, last_t = state["altitude"], t_s

        row = dict(state)
        row["time"] = t_s
        rows.append(row)

    # Back-fill GPS position: rows before the first fix get the first fix's
    # lat/lon/alt instead of 0.0 -- the payload had not moved yet.
    if first_gps_row is not None and first_gps_row > 0:
        lat0 = rows[first_gps_row]["gpsLat"]
        lon0 = rows[first_gps_row]["gpsLon"]
        alt0 = rows[first_gps_row]["gpsAlt"]
        for r in rows[:first_gps_row]:
            r["gpsLat"], r["gpsLon"], r["gpsAlt"] = lat0, lon0, alt0

    iso_t0 = None
    if t0_epoch_ms is not None:
        iso_t0 = datetime.fromtimestamp(t0_epoch_ms / 1000, tz=timezone.utc).isoformat()
    else:
        print("[!] no GPS fix found in the capture -- the dashboard will use "
              "the import time as t0 instead of the real flight time.",
              file=sys.stderr)

    meta = {
        "targetAltitude": target_altitude,
        "departureAltitude": departure_altitude,
        "t0": iso_t0,
        "t0EpochMs": t0_epoch_ms,
    }
    return rows, meta


def write_txt(rows, meta, path):
    with open(path, "w") as f:
        if meta.get("targetAltitude") is not None:
            f.write(f"# targetAltitude={meta['targetAltitude']}\n")
        f.write(f"# departureAltitude={meta.get('departureAltitude', 0.0)}\n")
        if meta.get("t0") is not None:
            f.write(f"# t0={meta['t0']}\n")
        f.write(",".join(SCHEMA_FIELDS) + "\n")
        for r in rows:
            f.write(",".join(str(r[k]) for k in SCHEMA_FIELDS) + "\n")


def write_flight_folder(rows, meta, date_str):
    """Create Dashboard/Backend/data/<date_str>/{telemetry,videos}/ (matching
    what server.py's get_flights() looks for) and write the telemetry file.
    Returns the path actually written."""
    flight_dir = DASHBOARD_DATA_DIR / date_str
    telemetry_dir = flight_dir / "telemetry"
    videos_dir = flight_dir / "videos"
    telemetry_dir.mkdir(parents=True, exist_ok=True)
    videos_dir.mkdir(parents=True, exist_ok=True)

    # Don't clobber a previous conversion for the same date -- pick the next
    # free telemetry_N.txt instead.
    target = telemetry_dir / "telemetry.txt"
    n = 2
    while target.exists():
        target = telemetry_dir / f"telemetry_{n}.txt"
        n += 1

    write_txt(rows, meta, target)
    return flight_dir, target, videos_dir


def _prompt_float(question):
    try:
        reply = input(question).strip()
    except EOFError:
        return None
    if not reply:
        return None
    try:
        return float(reply)
    except ValueError:
        print(f"[!] '{reply}' is not a number -- skipping.", file=sys.stderr)
        return None


def prompt_target_altitude():
    """Ask the user for the target apogee (m); Enter/invalid input skips it."""
    return _prompt_float("Target altitude in meters (Enter to skip): ")


def prompt_departure_altitude():
    """Ask for the launch site's elevation above sea level (m), so the
    barometric altitude can also be reported as true MSL altitude. Enter or
    invalid input defaults to 0 m (altitudeMSL then equals the pad-relative
    altitude)."""
    val = _prompt_float(
        "Departure (launch site) altitude above sea level in meters "
        "(Enter to skip, defaults to 0): ")
    return val if val is not None else 0.0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True,
                    help="JSON file produced by bin2json.py")
    ap.add_argument("--output", required=False, default=None,
                    help="write a single flat .txt file here instead of "
                         "creating a Dashboard flight folder")
    ap.add_argument("--date", required=False, default=None,
                    help="flight date, used as the Dashboard flight folder "
                         "name (YYYY-MM-DD); defaults to today's date if "
                         "omitted. Ignored if --output is given.")
    ap.add_argument("--target-altitude", type=float, default=None,
                    help="target apogee (m) written as dashboard metadata; "
                         "if omitted you'll be prompted for it")
    ap.add_argument("--departure-altitude", type=float, default=None,
                    help="launch site elevation above sea level (m), used to "
                         "compute altitudeMSL from the barometric altitude; "
                         "if omitted you'll be prompted for it (default 0)")
    args = ap.parse_args()

    target_altitude = args.target_altitude
    if target_altitude is None:
        target_altitude = prompt_target_altitude()
    departure_altitude = args.departure_altitude
    if departure_altitude is None:
        departure_altitude = prompt_departure_altitude()

    frames = load_frames(args.source)
    if not frames:
        print("[!] no usable frames found in the source file.", file=sys.stderr)
        sys.exit(1)

    rows, meta = convert(frames, target_altitude, departure_altitude)

    if args.output is not None:
        write_txt(rows, meta, args.output)
        print(f"[+] {len(rows)} rows written to {args.output}")
        return

    date_str = args.date or datetime.now().strftime("%Y-%m-%d")
    try:
        datetime.strptime(date_str, "%Y-%m-%d")
    except ValueError:
        print(f"[!] --date must be YYYY-MM-DD, got '{date_str}' -- the "
              "dashboard only lists flight folders in that exact format.",
              file=sys.stderr)
        sys.exit(1)

    flight_dir, telemetry_path, videos_dir = write_flight_folder(rows, meta, date_str)
    print(f"[+] {len(rows)} rows written to {telemetry_path}")
    print(f"[+] flight folder ready: {flight_dir}")
    print(f"[+] drop your camera videos into: {videos_dir}")


if __name__ == "__main__":
    main()
