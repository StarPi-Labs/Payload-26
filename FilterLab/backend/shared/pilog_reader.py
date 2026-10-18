"""
pilog_reader.py -- load Pi-LOG recordings into numpy arrays, fast.

Reads either the raw SD/telemetry binary (.bin) or bin2json.py output (.json)
and returns one array set per sensor. Physical conversion is delegated to
frameparser.py's own proc_* functions, so every value is identical to what
bin2json.py produces -- this module only changes *how fast* frames are found:

  * the whole file is scanned through a memoryview instead of re-slicing a
    bytes buffer after every frame (frameparser's streaming parser copies the
    remaining buffer each time, which is quadratic on a 200 MB file), and
  * CRC-16/CCITT is computed with binascii.crc_hqx, a C implementation that is
    bit-identical to frameparser.calculate_crc16 (verified: check value 0x29B1).

JSON input is streamed object by object, so a multi-gigabyte bin2json export
never has to fit in memory as Python objects.

One correction relative to frameparser: GPS latitude/longitude are signed here
using the N/S and E/W hemisphere letters. frameparser stores those letters but
never applies them, so positions south of the equator or west of Greenwich come
out mirrored there.
"""
import binascii
import json
import os
import struct
from array import array
from dataclasses import dataclass, field

import numpy as np

import frameparser as fp

SESSION_GAP_MS = 5000   # onboard millis going backwards by more than this = reboot

_FP_DEFAULTS = {
    "current_mode": fp.current_mode,
    "crc_fail_count": 0,
    "mpu_accel_sens_low": fp.mpu_accel_sens_low,
    "mpu_accel_sens_high": fp.mpu_accel_sens_high,
    "mpu6050_gyro_sensitivity": fp.mpu6050_gyro_sensitivity,
    "INA219_SHUNT_OMH": fp.INA219_SHUNT_OMH,
    "bme_cal": None,
    "_bme_t_fine": 0.0,
}


def _reset_frameparser():
    """frameparser keeps conversion state in module globals (current mode,
    calibration constants). Reset it so reading one file cannot leak state
    into the next."""
    for name, value in _FP_DEFAULTS.items():
        setattr(fp, name, value)


# ---------------------------------------------------------------------------
# Frame iteration
# ---------------------------------------------------------------------------

_DISPATCH = {
    fp.PACKET_TYPE_MPU6050: fp.proc_mpu6050,
    fp.PACKET_TYPE_BME680: fp.proc_bme680,
    fp.PACKET_TYPE_GPS: fp.proc_gps,
    fp.PACKET_TYPE_INA219: fp.proc_ina219,
    fp.PACKET_TYPE_SYSSTATE: fp.proc_sysstate,
    fp.PACKET_TYPE_GAS: fp.proc_gas,
}


class _Stats:
    frames = 0
    crc_failures = 0


def iter_frames_bin(path, stats=None):
    """Yield bin2json-equivalent frame dicts from a raw .bin recording."""
    _reset_frameparser()
    with open(path, "rb") as f:
        data = f.read()
    mv = memoryview(data)
    n = len(data)
    hdr, ftr = fp.HEADER_SIZE, fp.FOOTER_SIZE
    defs = {t: (d["size"], struct.Struct(d["fmt"])) for t, d in fp.PACKET_DEFS.items()}
    sync = b"\xaa\xaa\xaa"
    crc = binascii.crc_hqx
    i = 0

    while True:
        i = data.find(sync, i)
        if i < 0 or i + hdr > n:
            return
        ptype = data[i + 3]
        entry = defs.get(ptype)
        if entry is None:
            i += 1                                   # sync bytes inside payload
            continue
        size, st = entry
        total = hdr + size + ftr
        if i + total > n:
            return                                   # truncated final frame
        expected = data[i + total - 2] | (data[i + total - 1] << 8)
        if crc(mv[i:i + total - 2], 0xFFFF) != expected:
            if stats is not None:
                stats.crc_failures += 1
            i += 1
            continue
        try:
            payload = st.unpack_from(data, i + hdr)
        except struct.error:
            i += 1
            continue

        frame = {"frame-status": "1",
                 "sys-timestamp_ms": int.from_bytes(data[i + 4:i + 8], "little")}
        if ptype == fp.PACKET_TYPE_CALIB:
            fp.proc_calib(frame, payload[0])
        else:
            handler = _DISPATCH.get(ptype)
            if handler is not None:
                handler(frame, payload)
        if stats is not None:
            stats.frames += 1
        yield frame
        i += total


def iter_frames_json(path, stats=None):
    """Stream frame dicts from a bin2json.py export without loading it whole."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        first = f.read(1 << 16)
        f.seek(0)
        # Older bin2json wrote everything on one line; that format is small
        # enough in practice to parse directly.
        if first.count("\n") < 3:
            for d in json.load(f):
                if isinstance(d, dict) and "sys-timestamp_ms" in d:
                    if stats is not None:
                        stats.frames += 1
                    yield d
            return

        cur = None
        for line in f:
            s = line.strip()
            if s.startswith("{"):
                cur = ["{"]
            elif cur is not None:
                if s.startswith("}"):
                    cur.append("}")
                    try:
                        d = json.loads("".join(cur))
                    except ValueError:
                        d = None
                    cur = None
                    if isinstance(d, dict) and "sys-timestamp_ms" in d:
                        if stats is not None:
                            stats.frames += 1
                        yield d
                else:
                    cur.append(s)


def iter_frames(path, stats=None):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".bin":
        return iter_frames_bin(path, stats)
    if ext == ".json":
        return iter_frames_json(path, stats)
    raise ValueError(f"unsupported recording type '{ext}' (expected .bin or .json)")


# ---------------------------------------------------------------------------
# Arrays
# ---------------------------------------------------------------------------

@dataclass
class Recording:
    path: str
    imu: dict = field(default_factory=dict)     # t [s], ax ay az [g], gx gy gz [deg/s], temp [C]
    baro: dict = field(default_factory=dict)    # t [s], pressure [kPa], temperature [C], humidity [%]
    gps: dict = field(default_factory=dict)     # t [s], lat lon [deg, signed], alt [m], hvel [m/s], sats
    ina: dict = field(default_factory=dict)     # t [s], bus_volts [V], current [mA]
    modes: list = field(default_factory=list)   # [(t [s], mode, session)]
    calib: dict = None
    sessions: list = field(default_factory=list)  # [{"id", "t_start", "t_end", "frames"}]
    frames: int = 0
    crc_failures: int = 0

    def duration(self):
        return (self.imu["t"][-1] - self.imu["t"][0]) if len(self.imu.get("t", [])) > 1 else 0.0


def _buf(names):
    return {name: array("d") for name in names}


def load_recording(path, progress=None):
    """Read a .bin or .json recording into per-sensor numpy arrays."""
    stats = _Stats()
    return recording_from_frames(iter_frames(path, stats), path=path, stats=stats,
                                 progress=progress)


def recording_from_frames(frames, path="", stats=None, progress=None):
    """Build a Recording from any iterable of bin2json-style frame dicts.

    Every sensor array carries a `session` column. The onboard clock restarts
    on reboot, so a capture spanning a reboot has timestamps that run backwards;
    those are split into sessions rather than merged into nonsense. Frames must
    therefore arrive in recording order - not re-sorted by timestamp.
    """
    stats = stats if stats is not None else _Stats()
    seen = 0     # counted here: a plain list of frames never touches stats
    imu = _buf(["t", "ax", "ay", "az", "gx", "gy", "gz", "temp", "session"])
    baro = _buf(["t", "pressure", "temperature", "humidity", "session"])
    gps = _buf(["t", "lat", "lon", "alt", "hvel", "sats", "session"])
    ina = _buf(["t", "bus_volts", "current", "session"])
    modes = []
    calib = None
    sessions = []

    session = 0
    last_ms = None
    sess_start = None
    sess_frames = 0

    for d in frames:
        seen += 1
        if progress is not None and seen % 500_000 == 0:
            progress(seen)
        if d.get("frame-status") == "0":
            continue
        ms = d["sys-timestamp_ms"]
        if last_ms is not None and ms + SESSION_GAP_MS < last_ms:
            sessions.append({"id": session, "t_start": sess_start / 1000.0,
                             "t_end": last_ms / 1000.0, "frames": sess_frames})
            session += 1
            sess_start = None
            sess_frames = 0
        if sess_start is None:
            sess_start = ms
        last_ms = ms
        sess_frames += 1
        t = ms / 1000.0

        if "accelerationX" in d:
            b = imu
            b["t"].append(t); b["session"].append(session)
            b["ax"].append(d["accelerationX"]); b["ay"].append(d["accelerationY"])
            b["az"].append(d["accelerationZ"])
            b["gx"].append(d.get("roll", 0.0)); b["gy"].append(d.get("pitch", 0.0))
            b["gz"].append(d.get("yaw", 0.0))
            b["temp"].append(d.get("imu_temp", float("nan")))
        elif "pressure" in d:
            b = baro
            b["t"].append(t); b["session"].append(session)
            b["pressure"].append(d["pressure"])
            b["temperature"].append(d.get("temperature", float("nan")))
            b["humidity"].append(d.get("humidity", float("nan")))
        elif "gpsLat" in d and d.get("gpsLock") == "A":
            lat = d["gpsLat"]
            lon = d.get("gpsLon", float("nan"))
            if d.get("gpsLat_N") == "S":
                lat = -lat
            if d.get("gpsLon_W") == "W":
                lon = -lon
            b = gps
            b["t"].append(t); b["session"].append(session)
            b["lat"].append(lat); b["lon"].append(lon)
            b["alt"].append(d.get("gpsAlt", float("nan")))
            b["hvel"].append(d.get("horizontalVelocity", float("nan")) / 3.6)
            b["sats"].append(d.get("sat_count", float("nan")))
        elif "bus_volts" in d:
            b = ina
            b["t"].append(t); b["session"].append(session)
            b["bus_volts"].append(d["bus_volts"]); b["current"].append(d.get("current", float("nan")))
        elif "mode" in d:
            modes.append((t, int(d["mode"]), session))
        elif "calib_version" in d and calib is None:
            calib = {k: v for k, v in d.items() if k not in ("frame-status", "sys-timestamp_ms")}

    if last_ms is not None:
        sessions.append({"id": session, "t_start": sess_start / 1000.0,
                         "t_end": last_ms / 1000.0, "frames": sess_frames})

    def to_np(bufs):
        return {k: np.frombuffer(v, dtype=np.float64).copy() for k, v in bufs.items()}

    return Recording(path=path, imu=to_np(imu), baro=to_np(baro), gps=to_np(gps),
                     ina=to_np(ina), modes=modes, calib=calib, sessions=sessions,
                     frames=seen, crc_failures=stats.crc_failures)


def timing_quality(t):
    """Characterise a sensor's sample timing, separating timestamp quantisation
    from genuinely missing samples.

    Frames are stamped with the FreeRTOS tick (xTaskGetTickCount, 10 ms at
    CONFIG_FREERTOS_HZ=100), while each sensor runs on its own clock - the MPU at
    83.33 Hz. A steady 12 ms rhythm stamped on a 10 ms tick logs as
    10, 10, 10, 10, 20 ms: it looks exactly like "every sixth sample is missing",
    yet nothing is. From timestamps alone the two cases are indistinguishable,
    except for the pattern:

      * quantisation is a beat between two clocks, so the long steps recur at a
        fixed spacing, and every step is an exact multiple of the stamp resolution;
      * real drops are irregular.

    Only when both quantisation signatures are present is the true period taken
    from sample count over time, and only steps longer than three true periods
    are counted as lost data.
    """
    t = np.asarray(t, dtype=np.float64)
    n = len(t)
    if n < 10:
        return {"samples": int(n)}
    steps = np.diff(t)
    duration = float(t[-1] - t[0])
    pos = np.round(steps[steps > 0], 4)
    if pos.size == 0 or duration <= 0:
        return {"samples": int(n)}

    vals, counts = np.unique(pos, return_counts=True)
    base = float(vals[np.argmax(counts)])                       # most common step
    resolution = float(vals[0])                                  # finest step seen
    multiples = np.all(np.abs(pos / base - np.round(pos / base)) < 0.05)
    long_idx = np.nonzero(steps > 1.5 * base)[0]
    periodic = False
    beat = None
    if long_idx.size >= 20:
        spacing = np.diff(long_idx)
        sv, sc = np.unique(spacing, return_counts=True)
        periodic = sc.max() / spacing.size > 0.8
        beat = int(sv[np.argmax(sc)])

    quantised = bool(multiples and periodic and abs(resolution - base) < 1e-9)
    clock = "fixed"
    if quantised:
        period = duration / (n - 1)
        gaps = steps > 3 * period
        if np.any(gaps):                                         # refine without gap time
            period = (duration - steps[gaps].sum()) / (n - 1 - gaps.sum())
            gaps = steps > 3 * period
    else:
        period = base
        gaps = steps > 1.5 * period
        if np.any(gaps):
            # A fixed-rate clock that loses samples leaves steps at whole multiples
            # of its period. A polled sensor's own cycle does not (the BME680's
            # 200 ms gas-heater pause is 6.7 T/P/H steps), and those are not losses.
            ratio = steps[gaps] / period
            if np.mean(np.abs(ratio - np.round(ratio)) < 0.15) < 0.8:
                clock = "irregular"

    if clock == "fixed":
        missing = int(np.sum(np.maximum(np.round(steps[gaps] / period) - 1, 0))) if np.any(gaps) else 0
        missing_pct = round(100.0 * missing / (n + missing), 3)
    else:
        missing = missing_pct = None

    long_typical = None
    typical_per_minute = None
    if np.any(gaps):
        lv, lc = np.unique(np.round(steps[gaps], 3), return_counts=True)
        long_typical = float(lv[np.argmax(lc)])
        # Count only gaps of about that length: a polled sensor can have more
        # than one kind of long step (the BME680 shows 50 ms and 200 ms ones).
        near = np.abs(steps[gaps] - long_typical) <= 0.2 * long_typical
        typical_per_minute = round(60.0 * float(np.sum(near)) / duration, 3)
    return {
        "samples": int(n),
        "rate_hz": round(1.0 / period, 3),
        "rate_effective_hz": round((n - 1) / duration, 3),
        "period_s": period,
        "clock": clock,
        "timestamp_resolution_s": round(resolution, 4),
        "timestamps_quantised": quantised,
        "quantisation_beat_steps": beat if quantised else None,
        "gaps": int(np.sum(gaps)),
        "gap_typical_s": long_typical,
        "gaps_per_minute": round(60.0 * float(np.sum(gaps)) / duration, 3),
        "typical_gaps_per_minute": typical_per_minute,
        "samples_missing_est": missing,
        "samples_missing_pct": missing_pct,
        "largest_step_s": round(float(steps.max()), 4),
        "duration_s": round(duration, 3),
    }


def dequantise_times(t, q=None):
    """Recover real sample times from tick-quantised timestamps.

    Only meaningful for a fixed-rate sensor whose stamps timing_quality() found
    quantised (the interrupt-driven IMU). Between genuine gaps the sensor clock
    is steady, so each gap-free run is refitted as a straight line of timestamp
    against sample index - which also absorbs any rate mismatch between the
    sensor oscillator and the ESP32 tick. xTaskGetTickCount truncates, so stamps
    sit on average half a tick early; that bias is added back.

    Returns t unchanged when the stamps are not quantised.
    """
    t = np.asarray(t, dtype=np.float64)
    q = q if q is not None else timing_quality(t)
    if not q.get("timestamps_quantised"):
        return t
    period = q["period_s"]
    tick = q["timestamp_resolution_s"]
    out = t.copy()
    breaks = np.nonzero(np.diff(t) > 3 * period)[0] + 1
    for seg in np.split(np.arange(len(t)), breaks):
        if len(seg) >= 2:
            k = seg - seg[0]
            slope, intercept = np.polyfit(k, t[seg], 1)
            out[seg] = intercept + slope * k + 0.5 * tick
        else:
            out[seg] = t[seg] + 0.5 * tick
    return out


def timing_warnings(name, q, tick_s=0.010):
    """Human-readable findings for timing_quality() output."""
    out = []
    if q.get("timestamps_quantised"):
        out.append(
            f"{name} timestamps are quantised to {q['timestamp_resolution_s'] * 1000:.0f} ms while "
            f"samples arrive every {q['period_s'] * 1000:.2f} ms ({q['rate_hz']} Hz): logged spacing "
            f"jumps between {q['timestamp_resolution_s'] * 1000:.0f} and "
            f"{2 * q['timestamp_resolution_s'] * 1000:.0f} ms although the sensor is steady - no data "
            "is lost. Cause: frames are stamped with the FreeRTOS tick; stamping with "
            "esp_timer_get_time() would give real sample times.")
    if (q.get("samples_missing_pct") or 0) > 1.0:
        out.append(f"{name}: about {q['samples_missing_pct']}% of samples are missing "
                   f"({q['gaps']} gaps, longest {q['largest_step_s']} s)")
    if q.get("clock") == "irregular" and q.get("gap_typical_s") and (q.get("typical_gaps_per_minute") or 0) >= 1:
        out.append(f"{name} pauses for {q['gap_typical_s'] * 1000:.0f} ms about "
                   f"{q['typical_gaps_per_minute']:.0f} times per minute (its own measurement cycle, e.g. "
                   f"the BME680 gas heater) - effective rate {q['rate_effective_hz']} Hz")
    return out


def select_session(rec, session=None):
    """Keep one session. Default: the one with the most IMU samples."""
    if not rec.sessions:
        return rec
    if session is None:
        ids, counts = np.unique(rec.imu["session"], return_counts=True) \
            if len(rec.imu.get("session", [])) else (np.array([0]), np.array([0]))
        session = int(ids[np.argmax(counts)])

    def pick(arrays):
        if not arrays or len(arrays.get("session", [])) == 0:
            return arrays
        keep = arrays["session"] == session
        return {k: v[keep] for k, v in arrays.items()}

    out = Recording(path=rec.path, imu=pick(rec.imu), baro=pick(rec.baro), gps=pick(rec.gps),
                    ina=pick(rec.ina), modes=[m for m in rec.modes if m[2] == session],
                    calib=rec.calib, sessions=[s for s in rec.sessions if s["id"] == session],
                    frames=rec.frames, crc_failures=rec.crc_failures)
    return out
