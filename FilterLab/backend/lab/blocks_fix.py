"""Fix & calibrate blocks: bias, axes, scale, timestamps."""
import math
from dataclasses import replace

import numpy as np

from . import shared
from .catalog import Param, Result, block, p_signals
from .signals import BlockError

G0 = 9.80665
ACCEL = ("imu.ax", "imu.ay", "imu.az")
GYRO = ("imu.gx", "imu.gy", "imu.gz")
# MATLAB calib thresholds: [0.1 0.1 0.1] m/s^2 and [0.01 0.01 0.01] rad/s
MATLAB_ACC_STD = 0.1
MATLAB_GYRO_STD = 0.01


def imu_signals(ws, names):
    missing = [n for n in names if n not in ws.signals]
    if missing:
        raise BlockError(f"needs {', '.join(missing)} (the recording's IMU signals)")
    sigs = [ws.signals[n] for n in names]
    if any(s.n != sigs[0].n for s in sigs):
        raise BlockError(f"{', '.join(names)} have different lengths - "
                         "blocks that resample one axis should resample all three")
    return sigs


@block("pad_calibration", "fix", "Pad calibration (MATLAB calib)",
       "remove {what} using {n} samples of {where}|reject if too noisy {check}",
       """
The pre-flight static calibration from the team's EuRoC estimator (calib in
the MATLAB Live Script). The rocket stands still on the pad, so whatever the IMU
reads there, apart from gravity, is sensor bias. The mean over n samples is
the bias; their spread is the noise the Kalman filter needs (Qimu, which the
"Kalman filter + RTS" block picks up).

"accel + gyro bias" is the MATLAB version. It assumes the rocket stands
exactly vertical, so the accelerometer has to read (0, 0, +1 g) and anything
else is subtracted. On a tilted rail that removes some real gravity too: the
tilt is taken for bias. "gyro bias only" leaves the accelerometer alone and is
right at any tilt (calibrate the accelerometer with the six-position block).

With the check on, the calibration is rejected and nothing is removed when the
spread exceeds the MATLAB thresholds: 0.1 m/s² and 0.01 rad/s (0.57 °/s).
""",
       [Param("what", "choice", "gyro",
              options=[("gyro", "the gyro bias"), ("both", "accel + gyro bias (MATLAB)")]),
        Param("n", "int", 500, min=10),
        Param("where", "choice", "first",
              options=[("first", "the start"), ("still", "the stillest stretch before launch")]),
        Param("check", "bool", True)])
def pad_calibration(ws, p, ctx):
    acc = imu_signals(ws, ACCEL)
    gyr = imu_signals(ws, GYRO)
    t = acc[0].t
    n = p["n"]
    if len(t) < n:
        raise BlockError(f"only {len(t)} IMU samples here, fewer than n = {n}")
    if p["where"] == "first":
        i0 = 0
    else:
        launch = ws.meta.get("launch")
        lim = len(t) if launch is None else int(np.searchsorted(t, launch - 0.5))
        if lim < n:
            raise BlockError("not enough samples before launch for this n")
        amag = np.sqrt(sum(s.v[:lim] ** 2 for s in acc))
        c1 = np.concatenate([[0.0], np.cumsum(amag)])
        c2 = np.concatenate([[0.0], np.cumsum(amag * amag)])
        var = (c2[n:] - c2[:-n]) / n - ((c1[n:] - c1[:-n]) / n) ** 2
        i0 = int(np.argmin(var))
    sl = slice(i0, i0 + n)
    am = np.array([s.v[sl].mean() for s in acc])
    asd = np.array([s.v[sl].std(ddof=1) for s in acc])
    gm = np.array([s.v[sl].mean() for s in gyr])
    gsd = np.array([s.v[sl].std(ddof=1) for s in gyr])
    acc_sd_mps2 = asd * G0
    gyro_sd_rads = gsd * math.pi / 180.0
    under = bool(np.all(acc_sd_mps2 < MATLAB_ACC_STD) and np.all(gyro_sd_rads < MATLAB_GYRO_STD))
    accepted = under or not p["check"]

    res = Result()
    res.notes.append(f"window {t[sl][0]:.2f}-{t[sl][-1]:.2f} s ({n} samples)")
    res.table = [["", "X", "Y", "Z"],
                 ["accel mean [g]"] + [f"{v:+.4f}" for v in am],
                 ["accel std [m/s²]"] + [f"{v:.4f}" for v in acc_sd_mps2],
                 ["gyro mean [°/s]"] + [f"{v:+.3f}" for v in gm],
                 ["gyro std [rad/s]"] + [f"{v:.4f}" for v in gyro_sd_rads]]
    ws.objects["pad"] = {"window": [float(t[sl][0]), float(t[sl][-1])], "accel_mean_g": am.tolist(),
                         "accel_std_mps2": acc_sd_mps2.tolist(), "gyro_mean_dps": gm.tolist(),
                         "gyro_std_dps": gsd.tolist(), "Qimu": (acc_sd_mps2 ** 2).tolist(),
                         "accepted": accepted, "g_measured": float(np.linalg.norm(am))}
    if not accepted:
        res.warnings.append("rejected: the spread exceeds the MATLAB thresholds (0.1 m/s², 0.01 rad/s) - "
                            "the rocket was not still; nothing removed")
        return res
    if not under:
        res.warnings.append("spread above the MATLAB thresholds, applied anyway (check is off)")
    for s, b in zip(gyr, gm):
        ws.put(s.with_values(s.v - b))
        res.changed.append(s.name)
    res.notes.append(f"gyro bias removed: {', '.join(f'{v:+.3f}' for v in gm)} °/s")
    if p["what"] == "both":
        bias = am - np.array([0.0, 0.0, 1.0])
        for s, b in zip(acc, bias):
            ws.put(s.with_values(s.v - b))
            res.changed.append(s.name)
        res.notes.append(f"accel bias removed: {', '.join(f'{v:+.4f}' for v in bias)} g")
        tilt = math.degrees(math.acos(max(-1.0, min(1.0, am[2] / max(np.linalg.norm(am), 1e-9)))))
        if tilt > 1.0:
            res.warnings.append(f"the payload was tilted {tilt:.1f}° from vertical - "
                                "'accel + gyro bias' just removed that gravity as if it were bias")
    return res


AXIS_OPTIONS = [("+x", "+X"), ("-x", "−X"), ("+y", "+Y"), ("-y", "−Y"), ("+z", "+Z"), ("-z", "−Z")]


@block("remap", "fix", "Remap axes",
       "new X = old {x} · new Y = old {y} · new Z = old {z}",
       """
Turns the IMU axes, accelerometer and gyro together: for a sensor mounted
differently than the payload's CAD frame (+X away from the rail, +Y right,
+Z nose). Recordings since 18 Oct 2026 are already in the CAD frame.

The mapping has to be a rotation. A mirror (an odd number of sign flips) would
make the gyro turn the wrong way, so the block warns about it.
""",
       [Param("x", "choice", "+x", options=AXIS_OPTIONS),
        Param("y", "choice", "+y", options=AXIS_OPTIONS),
        Param("z", "choice", "+z", options=AXIS_OPTIONS)])
def remap(ws, p, ctx):
    picks = [p["x"], p["y"], p["z"]]
    src = ["xyz".index(c[1]) for c in picks]
    if len(set(src)) != 3:
        raise BlockError("each old axis may be used once")
    sign = [1.0 if c[0] == "+" else -1.0 for c in picks]
    m = np.zeros((3, 3))
    for r, (c, s) in enumerate(zip(src, sign)):
        m[r, c] = s
    res = Result()
    for names in (ACCEL, GYRO):
        if not all(n in ws.signals for n in names):
            continue
        old = imu_signals(ws, names)
        for k, name in enumerate(names):
            ws.put(old[k].with_values(sign[k] * old[src[k]].v))
            res.changed.append(name)
    if not res.changed:
        raise BlockError("no IMU signals to remap")
    if np.linalg.det(m) < 0:
        res.warnings.append("this mapping is a mirror, not a rotation: the gyro would turn the wrong way")
    return res


@block("apply_calibration", "fix", "Apply IMU calibration",
       "correct the accelerometer with {source}|offset X {ox} Y {oy} Z {oz} g|scale X {sx} Y {sy} Z {sz}",
       """
Applies an accelerometer calibration: corrected = (raw − offset) / scale per
axis. The numbers come from the six-position block above (put it in the same
recipe, or run it on a calibration recording and type its results here) or
from Documentation/CALIBRATION.md.

A good calibration reads exactly 1.000 g at rest in any pose. This MPU reads
about 0.96 g uncalibrated.
""",
       [Param("source", "choice", "typed",
              options=[("typed", "these numbers"), ("block", "the six-position result above")])]
       + [Param(n, "number", 0.0, precision=0.0001, show_if={"source": ["typed"]}) for n in ("ox", "oy", "oz")]
       + [Param(n, "number", 1.0, min=0.5, max=1.5, precision=0.0001, show_if={"source": ["typed"]})
          for n in ("sx", "sy", "sz")])
def apply_calibration(ws, p, ctx):
    if p["source"] == "block":
        cal = ws.objects.get("imu_calibration")
        if not cal or not cal.get("method"):
            raise BlockError("no six-position result above this block")
        offset, scale = cal["offset_g"], cal["scale"]
    else:
        offset = [p["ox"], p["oy"], p["oz"]]
        scale = [p["sx"], p["sy"], p["sz"]]
    acc = imu_signals(ws, ACCEL)
    res = Result()
    for s, o, k in zip(acc, offset, scale):
        ws.put(s.with_values((s.v - o) / k))
        res.changed.append(s.name)
    mag = np.sqrt(sum(ws.signals[n].v ** 2 for n in ACCEL))
    res.notes.append(f"offset {offset} g, scale {scale}; |a| median now {np.median(mag):.4f} g")
    return res


@block("scale", "fix", "Scale & offset",
       "set {signals} to {gain} × value + {offset}|unit {unit}",
       """
Multiplies and shifts signals: unit conversions (g to m/s²: gain 9.80665;
°/s to rad/s: 0.0174533; kPa to hPa: 10), a known gain error, or flipping a
sign (gain −1). Leave the unit empty to keep it.
""",
       [p_signals("imu.a*"), Param("gain", "number", 9.80665, precision=0.000001),
        Param("offset", "number", 0.0, precision=0.000001), Param("unit", "text", "m/s²")])
def scale(ws, p, ctx):
    names = ws.select(p["signals"])
    if not names:
        raise BlockError(f"no signal matches '{p['signals']}'")
    for n in names:
        s = ws.signals[n]
        ws.put(s.with_values(p["gain"] * s.v + p["offset"], unit=p["unit"] or s.unit))
    return Result(changed=names)


@block("fix_timestamps", "fix", "Fix IMU timestamps",
       "rebuild the real IMU sample times",
       """
Frames are stamped with the 10 ms FreeRTOS tick, but the IMU samples every
12 ms (83.3 Hz). The logged steps therefore jump between 10 and 20 ms, even
though the sensor is perfectly steady, and every integration (attitude,
velocity) picks up up to 8 ms of error per step. This refits the IMU times,
run by run between real gaps, as trajectory.py does before its Kalman filter.
Other sensors are moved by the same half tick, so all stay on one time base.
""",
       [])
def fix_timestamps(ws, p, ctx):
    pr = shared.pilog_reader
    base = ws.signals.get("imu.ax")
    if base is None:
        raise BlockError("needs imu.ax")
    q = pr.timing_quality(base.t)
    if not q.get("timestamps_quantised"):
        return Result(notes=["IMU timestamps are not tick-quantised - nothing to fix"])
    t_new = pr.dequantise_times(base.t, q)
    shift = 0.5 * q["timestamp_resolution_s"]
    res = Result(notes=[f"IMU at {q['rate_hz']} Hz stamped on a {q['timestamp_resolution_s'] * 1000:.0f} ms "
                        "tick - sample times rebuilt"])
    for name, s in list(ws.signals.items()):
        if s.axis != "time":
            continue
        if s.n == base.n and np.array_equal(s.t, base.t):
            ws.put(replace(s, t=t_new))
            res.changed.append(name)
        else:
            ws.put(replace(s, t=s.t + shift))
    return res
