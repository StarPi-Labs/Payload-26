"""
The recordings the lab can open, and turning them into signals.

The lab works on processed but not yet cleaned data: the JSON files bin2json.py
writes, one dict per decoded frame. Every value is already in physical units
(frameparser.py did the conversion, with the payload's calibration frame), but
nothing is merged, forward-filled, smoothed or derived. Every sensor keeps its
own samples at its own rate. That is the starting point a filter should see.

    FilterLab/recordings/       the lab's recordings directory - drop .json files here
    <lab data>/uploads/         files uploaded through the editor

A .bin goes through bin2json.py first:
    python SD-Parser/bin2json.py --source fly.bin --output FilterLab/recordings/fly.json

Reading a big JSON takes a while (about a minute per GB), so the result is cached
as .npz in the lab's data folder, keyed by path, size and modification time.
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
_LAB = os.path.abspath(os.path.join(_HERE, "..", ".."))          # the FilterLab folder

RECORDINGS_DIR = os.path.abspath(os.environ.get(
    "PILOG_RECORDINGS_DIR", os.path.join(_LAB, "recordings")))
LAB_DATA_DIR = os.path.abspath(os.environ.get(
    "PILOG_LAB_DATA_DIR", os.path.join(_LAB, "lab-data")))
UPLOAD_DIR = os.path.join(LAB_DATA_DIR, "uploads")
CACHE_DIR = os.path.join(LAB_DATA_DIR, "cache")

EXTENSIONS = (".json",)
# Not recordings: outputs of the SD-Parser tools that may sit in the same folder.
SKIP_SUFFIXES = ("_summary.json", "_noise.json", "calibration.json")
SKIP_DIRS = {"node_modules", ".cache", "__pycache__", ".git", "cache"}
CACHE_VERSION = 2
SESSION_GAP_MS = 5000

_parse_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Listing
# ---------------------------------------------------------------------------

def _entry(rid, path, label, group):
    st = os.stat(path)
    return {"id": rid, "label": label, "group": group, "size": st.st_size,
            "modified": st.st_mtime, "path": path}


def _is_recording(name):
    low = name.lower()
    return low.endswith(EXTENSIONS) and not low.endswith(SKIP_SUFFIXES) and not name.startswith(".")


def list_recordings():
    """Every recording the lab may open. Ids, not paths, travel to the
    browser; load() only accepts ids from this list."""
    out = []
    if os.path.isdir(RECORDINGS_DIR):
        for root, dirs, files in os.walk(RECORDINGS_DIR):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith("."))
            for f in sorted(files):
                if _is_recording(f):
                    rel = os.path.relpath(os.path.join(root, f), RECORDINGS_DIR).replace(os.sep, "/")
                    out.append(_entry("rec/" + rel, os.path.join(root, f), rel, "Recordings"))
    if os.path.isdir(UPLOAD_DIR):
        for f in sorted(os.listdir(UPLOAD_DIR)):
            p = os.path.join(UPLOAD_DIR, f)
            if os.path.isfile(p) and _is_recording(f):
                out.append(_entry("upload/" + f, p, f, "Uploaded"))
    return out


def resolve(rid):
    for r in list_recordings():
        if r["id"] == rid:
            return r
    raise BlockError(f"recording '{rid}' not found - pick one from the list")


def safe_upload_name(name):
    base = os.path.basename(name or "")
    stem, ext = os.path.splitext(base)
    if ext.lower() not in EXTENSIONS:
        raise ValueError("only bin2json .json recordings can be uploaded "
                         "(convert a .bin with SD-Parser/bin2json.py first)")
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
    """Per-sensor arrays from bin2json frame dicts, in recording order. The
    onboard clock restarts at every reboot, so time running backwards starts a
    new session (as in pilog_reader)."""
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
                # frameparser keeps the hemisphere as a letter: south and west are negative
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


def _load_json(path):
    stats = pilog_reader._Stats()
    with _parse_lock, contextlib.redirect_stdout(io.StringIO()):
        arrays, meta = _collect_frames(pilog_reader.iter_frames_json(path, stats))
    if meta["frames"] == 0:
        raise BlockError("no frames in this file - is it a bin2json.py output?")
    return arrays, meta


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
    arrays, meta = _load_json(path)
    meta.update({"id": rid, "label": rec["label"], "loadSeconds": round(time.time() - t0, 2)})
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
    ("gps", "sats", "gps.sats", "", "satellites used in the fix", True),
    ("fix", "fix", "gps.fix", "", "GPS fix (1) or searching (0)", True),
    ("mode", "mode", "mode", "", "flight mode: 0 INIT 1 POST 2 SNSCHK 3 ARMED 4 BOOST 5 COAST", True),
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
    return out
