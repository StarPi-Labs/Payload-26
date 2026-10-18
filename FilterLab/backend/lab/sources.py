"""
Recordings the lab can open, and turning them into signals.

Kinds (all read-only):
  raw        .bin (SD card fly.bin, P4 SD backup RX_nnnn.BIN, wired UART capture)
             and .json (bin2json.py output): the rawest data there is - every
             sensor at its own rate, converted to physical units by frameparser
             exactly as everywhere else, nothing merged, forward-filled or
             derived.
  upload     a .bin/.json dropped into the lab, kept in its data folder.
  dashboard  a Dashboard flight's telemetry file: already merged and
             forward-filled at frame rate. Each sensor is de-duplicated back to
             its own samples; the Dashboard's derived columns come along as
             dash.* for comparison.

Parsing a big recording takes a while (a 200 MB .bin is about a minute), so
the result is cached as .npz in the lab's data folder, keyed by path, size and
modification time.
"""
import contextlib
import hashlib
import io
import json
import os
import re
import threading
import time

import numpy as np

from . import shared
from .signals import BlockError, Signal

pilog_reader = shared.pilog_reader

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", "..", ".."))

RECORDINGS_DIR = os.path.abspath(os.environ.get(
    "PILOG_RECORDINGS_DIR", os.path.join(_REPO, "SD-Parser")))
DASHBOARD_DATA_DIR = os.path.abspath(os.environ.get(
    "PILOG_DASHBOARD_DATA_DIR", os.path.join(_REPO, "Dashboard", "Backend", "data")))
LAB_DATA_DIR = os.path.abspath(os.environ.get(
    "PILOG_LAB_DATA_DIR", os.path.join(_REPO, "FilterLab", "lab-data")))
UPLOAD_DIR = os.path.join(LAB_DATA_DIR, "uploads")
CACHE_DIR = os.path.join(LAB_DATA_DIR, "cache")

RAW_EXTENSIONS = (".bin", ".json")
DASHBOARD_EXTENSIONS = (".txt", ".csv", ".log")
FLIGHT_ID_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
SKIP_DIRS = {"node_modules", ".cache", "__pycache__", ".git", "cache", "build"}
CACHE_VERSION = 1
G0 = 9.80665
MODE_NAMES = ["INIT", "POST", "SNSCHK", "ARMED", "BOOST", "COAST"]
SESSION_GAP_MS = 5000

# frameparser keeps conversion state in module globals: one parse at a time.
_parse_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Listing
# ---------------------------------------------------------------------------

def _entry(rid, path, kind, label, group):
    st = os.stat(path)
    return {"id": rid, "kind": kind, "label": label, "group": group,
            "size": st.st_size, "modified": st.st_mtime, "path": path}


def list_recordings():
    """Every recording the lab may open. Ids, not paths, travel to the
    browser; load() only accepts ids from this list."""
    out = []
    if os.path.isdir(RECORDINGS_DIR):
        for root, dirs, files in os.walk(RECORDINGS_DIR):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".")]
            if os.path.relpath(root, RECORDINGS_DIR).count(os.sep) >= 2:
                dirs[:] = []
            for f in sorted(files):
                if f.lower().endswith(RAW_EXTENSIONS) and not f.endswith("_summary.json") \
                        and not f.endswith("_noise.json"):
                    rel = os.path.relpath(os.path.join(root, f), RECORDINGS_DIR).replace(os.sep, "/")
                    out.append(_entry("raw/" + rel, os.path.join(root, f), "raw", rel, "Recordings"))
    if os.path.isdir(UPLOAD_DIR):
        for f in sorted(os.listdir(UPLOAD_DIR)):
            p = os.path.join(UPLOAD_DIR, f)
            if os.path.isfile(p) and f.lower().endswith(RAW_EXTENSIONS):
                out.append(_entry("upload/" + f, p, "raw", f, "Uploaded"))
    if os.path.isdir(DASHBOARD_DATA_DIR):
        for flight in sorted(os.listdir(DASHBOARD_DATA_DIR), reverse=True):
            tdir = os.path.join(DASHBOARD_DATA_DIR, flight, "telemetry")
            if not FLIGHT_ID_RE.match(flight) or not os.path.isdir(tdir):
                continue
            for f in sorted(os.listdir(tdir)):
                p = os.path.join(tdir, f)
                if os.path.isfile(p) and f.lower().endswith(DASHBOARD_EXTENSIONS) and not f.startswith("."):
                    out.append(_entry(f"dash/{flight}/{f}", p, "dashboard",
                                      f"{flight} / {f}", "Dashboard flights"))
    return out


def resolve(rid):
    for r in list_recordings():
        if r["id"] == rid:
            return r
    raise BlockError(f"recording '{rid}' not found - pick one from the list")


def safe_upload_name(name):
    base = os.path.basename(name or "")
    stem, ext = os.path.splitext(base)
    if ext.lower() not in RAW_EXTENSIONS:
        raise ValueError("only .bin and .json recordings can be uploaded")
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", stem).strip("._") or "recording"
    return stem[:80] + ext.lower()


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

STREAMS = {
    "imu": ("t", "ax", "ay", "az", "gx", "gy", "gz", "temp", "session"),
    "baro": ("t", "pressure", "temperature", "humidity", "session"),
    "gas": ("t", "resistance", "session"),
    "gps": ("t", "lat", "lon", "alt", "speed", "sats", "session"),
    "fix": ("t", "fix", "session"),
    "power": ("t", "voltage", "current", "session"),
    "mode": ("t", "mode", "session"),
}


def _collect_frames(frames):
    """Per-sensor arrays from bin2json-style frame dicts, in recording order.
    The onboard clock restarts at every reboot, so time running backwards
    starts a new session (as in pilog_reader)."""
    bufs = {k: {f: [] for f in fields} for k, fields in STREAMS.items()}
    calib = None
    sessions = []
    session, last_ms, sess_start, sess_frames, seen = 0, None, None, 0, 0

    def add(stream, **vals):
        b = bufs[stream]
        for f in STREAMS[stream]:
            b[f].append(vals.get(f, np.nan))

    for d in frames:
        seen += 1
        if d.get("frame-status") == "0" or "sys-timestamp_ms" not in d:
            continue
        ms = d["sys-timestamp_ms"]
        if last_ms is not None and ms + SESSION_GAP_MS < last_ms:
            sessions.append({"id": session, "t_start": sess_start / 1000.0,
                             "t_end": last_ms / 1000.0, "frames": sess_frames})
            session += 1
            sess_start, sess_frames = None, 0
        if sess_start is None:
            sess_start = ms
        last_ms = ms
        sess_frames += 1
        t = ms / 1000.0
        if "accelerationX" in d:
            add("imu", t=t, ax=d["accelerationX"], ay=d["accelerationY"], az=d["accelerationZ"],
                gx=d.get("roll", np.nan), gy=d.get("pitch", np.nan), gz=d.get("yaw", np.nan),
                temp=d.get("imu_temp", np.nan), session=session)
        elif "pressure" in d:
            add("baro", t=t, pressure=d["pressure"], temperature=d.get("temperature", np.nan),
                humidity=d.get("humidity", np.nan), session=session)
        elif "gas_resistance" in d:
            add("gas", t=t, resistance=d["gas_resistance"], session=session)
        elif "gpsLock" in d:
            fixed = d["gpsLock"] == "A" and "gpsLat" in d
            add("fix", t=t, fix=1.0 if fixed else 0.0, session=session)
            if fixed:
                lat = d["gpsLat"] * (-1.0 if d.get("gpsLat_N") == "S" else 1.0)
                lon = d.get("gpsLon", np.nan) * (-1.0 if d.get("gpsLon_W") == "W" else 1.0)
                add("gps", t=t, lat=lat, lon=lon, alt=d.get("gpsAlt", np.nan),
                    speed=d.get("horizontalVelocity", np.nan) / 3.6,
                    sats=d.get("sat_count", np.nan), session=session)
        elif "bus_volts" in d:
            add("power", t=t, voltage=d["bus_volts"], current=d.get("current", np.nan), session=session)
        elif "mode" in d:
            add("mode", t=t, mode=float(d["mode"]), session=session)
        elif "calib_version" in d and calib is None:
            calib = {k: v for k, v in d.items() if k not in ("frame-status", "sys-timestamp_ms")}
    if last_ms is not None:
        sessions.append({"id": session, "t_start": sess_start / 1000.0,
                         "t_end": last_ms / 1000.0, "frames": sess_frames})
    arrays = {f"{k}__{f}": np.asarray(v, dtype=np.float64)
              for k, b in bufs.items() for f, v in b.items()}
    return arrays, {"calib": calib, "sessions": sessions, "frames": seen}


def _load_raw(path):
    stats = pilog_reader._Stats()
    with _parse_lock, contextlib.redirect_stdout(io.StringIO()):
        arrays, meta = _collect_frames(pilog_reader.iter_frames(path, stats))
    meta["crcFailures"] = stats.crc_failures
    return arrays, meta


# Dashboard telemetry columns the lab uses. Read only these, and sensor values as
# float32: a 20-hour file (9 million rows) then parses in about half the time
# and memory. Time and GPS keep float64 (ms over hours, 1e-7 degrees).
DASHBOARD_COLUMNS = ["time", "accelerationX", "accelerationY", "accelerationZ", "roll", "pitch", "yaw",
                     "pressure", "temperature", "humidity", "gasResistance", "gpsLat", "gpsLon", "gpsAlt",
                     "horizontalVelocity", "busVoltage", "current", "altitude", "velocity",
                     "kfAltitude", "kfVerticalVelocity", "kfHorizontalVelocity"]
FLOAT64_COLUMNS = {"time", "gpsLat", "gpsLon"}


def _load_dashboard(path):
    """Undo the forward-fill of a Dashboard telemetry file, sensor by sensor."""
    import pandas as pd
    header = {}
    columns = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                m = re.match(r"^#\s*([A-Za-z0-9_]+)\s*=\s*(.+)$", line.strip())
                if m:
                    header[m.group(1)] = m.group(2).strip()
            elif line.strip():
                columns = [c.strip() for c in line.split(",")]
                break
    wanted = [c for c in DASHBOARD_COLUMNS if c in columns]
    try:
        df = pd.read_csv(path, comment="#", engine="c", usecols=wanted,
                         dtype={c: ("float64" if c in FLOAT64_COLUMNS else "float32") for c in wanted})
    except (ValueError, TypeError):
        # a damaged line somewhere: the slow, tolerant way
        df = pd.read_csv(path, comment="#", engine="c", on_bad_lines="skip", usecols=wanted)
    df.columns = [c.strip() for c in df.columns]
    t = pd.to_numeric(df["time"], errors="coerce").to_numpy(np.float64)
    order = np.argsort(t, kind="stable")
    df = df.iloc[order]
    t = t[order]

    def col(name):
        if name not in df:
            return None
        s = df[name]
        return (s if s.dtype.kind == "f" else pd.to_numeric(s, errors="coerce")).to_numpy()

    def fresh(*cols):
        """Rows where any of these columns changed: the sensor's own samples."""
        cols = [c for c in cols if c is not None]
        if not cols:
            return np.zeros(len(t), bool)
        m = np.zeros(len(t), bool)
        m[0] = True
        for c in cols:
            m[1:] |= c[1:] != c[:-1]
        return m

    arrays = {}

    def take(stream, mask, **fields):
        arrays[f"{stream}__t"] = t[mask]
        arrays[f"{stream}__session"] = np.zeros(int(mask.sum()))
        for f, values in fields.items():
            arrays[f"{stream}__{f}"] = values[mask] if values is not None else np.full(int(mask.sum()), np.nan)

    ax, ay, az = col("accelerationX"), col("accelerationY"), col("accelerationZ")
    gx, gy, gz = col("roll"), col("pitch"), col("yaw")
    if ax is not None:
        m = fresh(ax, ay, az, gx, gy, gz) & ((ax != 0) | (ay != 0) | (az != 0))
        take("imu", m, ax=ax / G0, ay=ay / G0, az=az / G0, gx=gx, gy=gy, gz=gz, temp=None)
    p, tc, h = col("pressure"), col("temperature"), col("humidity")
    if p is not None:
        m = fresh(p, tc, h)
        m[0] = False                                   # json2telemetry's placeholder row
        m &= ~((p == 101.3) & (tc == 20.0) & (h == 50.0))
        take("baro", m, pressure=p, temperature=tc, humidity=h)
    gas = col("gasResistance")
    if gas is not None:
        m = fresh(gas) & (gas > 0)
        take("gas", m, resistance=gas)
    lat, lon = col("gpsLat"), col("gpsLon")
    if lat is not None:
        m = fresh(lat, lon, col("gpsAlt")) & ((lat != 0) | (lon != 0))
        take("gps", m, lat=lat, lon=lon, alt=col("gpsAlt"), speed=col("horizontalVelocity"), sats=None)
    v, i = col("busVoltage"), col("current")
    if v is not None:
        m = fresh(v, i) & ((v != 0) | (i != 0))
        take("power", m, voltage=v, current=i)
    modes = []
    for part in (header.get("modeTransitions") or "").split(","):
        if ":" in part:
            ts, name = part.split(":", 1)
            try:
                modes.append((float(ts), float(MODE_NAMES.index(name.strip()))))
            except ValueError:
                pass
    arrays["mode__t"] = np.array([m[0] for m in modes])
    arrays["mode__mode"] = np.array([m[1] for m in modes])
    arrays["mode__session"] = np.zeros(len(modes))
    # The Dashboard's own derived columns, to compare a recipe against.
    for name in ("altitude", "velocity", "kfAltitude", "kfVerticalVelocity", "kfHorizontalVelocity"):
        c = col(name)
        if c is not None:
            arrays[f"dash__{name}"] = c
    arrays["dash__t"] = t
    sessions = [{"id": 0, "t_start": float(t[0]) if len(t) else 0.0,
                 "t_end": float(t[-1]) if len(t) else 0.0, "frames": int(len(t))}]
    return arrays, {"calib": None, "sessions": sessions, "frames": int(len(t)),
                    "crcFailures": 0, "header": header}


def load(rid):
    """(arrays, meta) for a recording id - from the cache when possible."""
    rec = resolve(rid)
    path = rec["path"]
    st = os.stat(path)
    key = hashlib.sha1(f"{CACHE_VERSION}|{os.path.abspath(path)}|{st.st_size}|{st.st_mtime}"
                       .encode()).hexdigest()[:20]
    cache_path = os.path.join(CACHE_DIR, key + ".npz")
    if os.path.isfile(cache_path):
        try:
            with np.load(cache_path, allow_pickle=False) as z:
                arrays = {k: z[k] for k in z.files if k != "__meta__"}
                meta = json.loads(str(z["__meta__"]))
            return arrays, meta
        except (OSError, ValueError, KeyError):
            pass                                       # rebuild a damaged cache
    t0 = time.time()
    arrays, meta = (_load_dashboard(path) if rec["kind"] == "dashboard" else _load_raw(path))
    meta.update({"id": rid, "kind": rec["kind"], "label": rec["label"],
                 "loadSeconds": round(time.time() - t0, 2)})
    try:
        os.makedirs(CACHE_DIR, exist_ok=True)
        tmp = cache_path + f".tmp{os.getpid()}.npz"
        np.savez(tmp, __meta__=np.array(json.dumps(meta)), **arrays)
        os.replace(tmp, cache_path)
    except OSError:
        pass                                           # read-only lab data: just slower
    return arrays, meta


# ---------------------------------------------------------------------------
# Arrays -> signals
# ---------------------------------------------------------------------------

SIGNALS = [
    # stream, field, signal name, unit, description, step
    ("imu", "ax", "imu.ax", "g", "accelerometer X (front, away from the rail)", False),
    ("imu", "ay", "imu.ay", "g", "accelerometer Y (right, seen from the front)", False),
    ("imu", "az", "imu.az", "g", "accelerometer Z (nose)", False),
    ("imu", "gx", "imu.gx", "°/s", "gyro rate about X", False),
    ("imu", "gy", "imu.gy", "°/s", "gyro rate about Y", False),
    ("imu", "gz", "imu.gz", "°/s", "gyro rate about Z (spin about the long axis)", False),
    ("imu", "temp", "imu.temp", "°C", "MPU die temperature", False),
    ("baro", "pressure", "baro.pressure", "kPa", "BME680 pressure", False),
    ("baro", "temperature", "baro.temperature", "°C", "BME680 temperature", False),
    ("baro", "humidity", "baro.humidity", "%", "BME680 relative humidity", False),
    ("gas", "resistance", "gas.resistance", "Ω", "BME680 gas resistance", False),
    ("power", "voltage", "power.voltage", "V", "INA219 bus voltage", False),
    ("power", "current", "power.current", "mA", "INA219 current", False),
    ("gps", "lat", "gps.lat", "°", "GPS latitude (north positive)", False),
    ("gps", "lon", "gps.lon", "°", "GPS longitude (east positive)", False),
    ("gps", "alt", "gps.alt", "m", "GPS altitude above sea level", False),
    ("gps", "speed", "gps.speed", "m/s", "GPS ground speed", False),
    ("gps", "sats", "gps.sats", "", "satellites used", True),
    ("fix", "fix", "gps.fix", "", "GPS fix (1) or searching (0)", True),
    ("mode", "mode", "mode", "", "flight mode: 0 INIT 1 POST 2 SNSCHK 3 ARMED 4 BOOST 5 COAST", True),
]
DASH_SIGNALS = [
    ("altitude", "dash.altitude", "m", "Dashboard: barometric altitude"),
    ("velocity", "dash.velocity", "m/s", "Dashboard: vertical speed (difference of altitude)"),
    ("kfAltitude", "dash.kfAltitude", "m", "Dashboard: Kalman/RTS altitude"),
    ("kfVerticalVelocity", "dash.kfVerticalVelocity", "m/s", "Dashboard: Kalman/RTS vertical speed"),
    ("kfHorizontalVelocity", "dash.kfHorizontalVelocity", "m/s", "Dashboard: Kalman/RTS horizontal speed"),
]


def pick_session(arrays, meta, choice):
    """Session id for 'longest' / '1'..'n' (1-based in the UI)."""
    sessions = meta.get("sessions") or [{"id": 0}]
    if choice in (None, "", "longest"):
        sess = arrays.get("imu__session", np.zeros(0))
        if len(sess):
            ids, counts = np.unique(sess, return_counts=True)
            return int(ids[np.argmax(counts)])
        return int(max(sessions, key=lambda s: s.get("frames", 0))["id"])
    k = int(choice) - 1
    if not 0 <= k < len(sessions):
        raise BlockError(f"this recording has {len(sessions)} session(s); there is no session {choice}")
    return int(sessions[k]["id"])


def to_signals(arrays, meta, session):
    """Signals of one session, on the onboard clock (seconds since boot)."""
    out = []
    for stream, field, name, unit, desc, step in SIGNALS:
        t = arrays.get(f"{stream}__t")
        v = arrays.get(f"{stream}__{field}")
        if t is None or v is None or len(t) == 0:
            continue
        keep = arrays[f"{stream}__session"] == session
        if not np.any(keep) or not np.any(np.isfinite(v[keep])):
            continue
        out.append(Signal(name, t[keep], v[keep], unit, desc, step=step))
    if "dash__t" in arrays:
        for field, name, unit, desc in DASH_SIGNALS:
            if f"dash__{field}" in arrays:
                out.append(Signal(name, arrays["dash__t"], arrays[f"dash__{field}"], unit, desc))
    return out
