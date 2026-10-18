"""Analyze blocks: numbers, spectra, noise and calibration."""
import json
import math

import numpy as np
from scipy import signal as sps

from . import shared
from .blocks_fix import ACCEL, GYRO, imu_signals
from .catalog import Param, Result, block, p_signal, p_signals
from .signals import BlockError, Signal, effective_rate, stats, values_at


def fmt(v, nd=4):
    return "" if v is None else f"{v:.{nd}g}"


@block("stats", "analyze", "Statistics",
       "measure {signals} from {start} s to {end} s",
       """
Mean, spread (standard deviation), minimum and maximum of signals over a time
window. Leave both at 0 for the whole signal.

The spread over a stretch where nothing happens is the sensor's noise: the
number a Kalman filter wants as its σ.
""",
       [p_signals("imu.*"), Param("start", "number", 0.0, unit="s"), Param("end", "number", 0.0, unit="s")])
def stats_block(ws, p, ctx):
    names = ws.select(p["signals"])
    if not names:
        raise BlockError(f"no signal matches '{p['signals']}'")
    whole = p["start"] == 0 and p["end"] == 0
    rows = [["signal", "unit", "samples", "mean", "std", "min", "max"]]
    for n in names:
        s = ws.signals[n]
        v = s.v if whole else s.v[(s.t >= p["start"]) & (s.t <= p["end"])]
        st = stats(v)
        rows.append([n, s.unit, f"{st['n']:,}", fmt(st["mean"], 6), fmt(st["std"]), fmt(st["min"], 6),
                     fmt(st["max"], 6)])
    return Result(table=rows)


@block("spectrum", "analyze", "Spectrum",
       "make {signal}.psd = frequency content of {signal}|segments of {segment} s",
       """
Shows how strong each frequency is in a signal (power spectral density,
Welch's method), plotted on log axes. Narrow peaks are vibrations: the motor,
a fan on the bench, the mains. The flat floor is white noise. Use it to choose
a filter cutoff, or the frequency for the notch filter.
""",
       [p_signal("signal", "imu.az"), Param("segment", "number", 5.0, min=0.01, unit="s")])
def spectrum(ws, p, ctx):
    s = ws.get(p["signal"])
    fs = effective_rate(s.t)
    if not math.isfinite(fs) or s.n < 16:
        raise BlockError(f"{s.name} is too short for a spectrum")
    nper = max(16, min(s.n, int(p["segment"] * fs)))
    f, pxx = sps.welch(np.nan_to_num(s.v - np.nanmean(s.v)), fs=fs, nperseg=nper)
    keep = f > 0
    name = f"{s.name}.psd"
    ws.put(Signal(name, f[keep], pxx[keep], f"({s.unit})²/Hz" if s.unit else "1/Hz",
                  f"power spectral density of {s.name}", axis="frequency"))
    top = f[keep][int(np.argmax(pxx[keep]))]
    res = Result(changed=[name], notes=[f"sampled at {fs:.1f} Hz (spectrum up to {fs / 2:.1f} Hz); "
                                        f"strongest line at {top:.2f} Hz"])
    steps = np.diff(s.t)
    if steps.size and steps.max() > 5 / fs:
        res.warnings.append("the signal has gaps: the spectrum treats the samples as evenly spaced")
    return res


@block("allan", "analyze", "Allan deviation",
       "make {signal}.adev = noise vs. averaging time of {signal}",
       """
How much a sensor's output wanders when averaged over different times τ,
computed with noise_characterization.py on a still recording. On log axes:

- the slope −½ at short τ is white noise: its value at τ = 1 s is the angle
  (gyro) or velocity (accelerometer) random walk;
- the flat bottom is the bias instability, the best you can ever average down to;
- a rise at long τ is drift (temperature).

Needs a long recording of the payload sitting completely still.
""",
       [p_signal("signal", "imu.gz")])
def allan(ws, p, ctx):
    nc = shared.noise_characterization
    s = ws.get(p["signal"])
    fs = effective_rate(s.t)
    if s.n < 1000:
        raise BlockError("needs at least 1000 samples of the payload sitting still")
    grid, x = nc.uniform(s.t, s.v, fs)
    taus, adev = nc.allan_deviation(x, fs)
    name = f"{s.name}.adev"
    ws.put(Signal(name, taus, adev, s.unit, f"Allan deviation of {s.name}", axis="tau"))
    bi, tau_bi = nc.bias_instability(taus, adev)
    rw = nc.adev_at(taus, adev, 1.0)
    return Result(changed=[name], notes=[f"at τ = 1 s: {rw:.4g} {s.unit} (random walk)",
                                         f"bias instability {bi:.4g} {s.unit} at τ ≈ {tau_bi:.0f} s"])


@block("compare", "analyze", "Compare",
       "compare {a} with {b}",
       """
How far two signals are apart: the second is interpolated onto the first's
samples and the difference summarised (mean = offset, RMS = typical error,
max = worst case). For example: your recipe's altitude against the website's
(dash.kfAltitude), or GPS altitude against barometric altitude.
""",
       [p_signal("a", "kf.z"), p_signal("b", "dash.kfAltitude")])
def compare(ws, p, ctx):
    a = ws.get(p["a"])
    b = values_at(ws.get(p["b"]), a.t)
    d = a.v - b
    st = stats(d)
    if not st["n"]:
        raise BlockError("the two signals do not overlap in time")
    name = f"{a.name}.minus.{p['b']}"
    ws.put(Signal(name, a.t, d, a.unit, f"{a.name} − {p['b']}"))
    return Result(changed=[name], table=[["difference", "mean", "RMS", "std", "max |d|"],
                                          [f"{a.name} − {p['b']} [{a.unit}]", fmt(st["mean"]), fmt(st["rms"]),
                                           fmt(st["std"]), fmt(max(abs(st["min"]), abs(st["max"])))]])


@block("six_position", "analyze", "Six-position calibration",
       "calibrate the accelerometer from still poses|shortest still stretch {min_still} s",
       """
Accelerometer calibration from a recording of the payload lying still in six
poses, each axis once straight up and once straight down, about 10 s each
(Documentation/CALIBRATION.md describes the procedure). The block finds the
still stretches and works out, per axis, the offset and scale that make the
corrected reading exactly ±1 g. It uses SD-Parser/imu_calibration.py.

Apply the result with "Apply IMU calibration" (in this recipe, or typed into
another), or download calibration.json.
""",
       [Param("min_still", "number", 3.0, min=0.5, unit="s")])
def six_position(ws, p, ctx):
    acc = imu_signals(ws, ACCEL)
    gyr = imu_signals(ws, GYRO) if all(n in ws.signals for n in GYRO) else None
    t = acc[0].t
    a = np.column_stack([s.v for s in acc])
    g = np.column_stack([s.v for s in gyr]) if gyr else None
    cal = shared.imu_calibration.calibrate_accel(t, a, g, min_duration_s=p["min_still"])
    ws.objects["imu_calibration"] = cal
    res = Result(warnings=list(cal.get("warnings", [])))
    rows = [["stretch", "pose", "mean X [g]", "mean Y [g]", "mean Z [g]", "error after [mg]"]]
    for s in cal["stretches"]:
        rows.append([f"{s['t_start']:.1f}-{s['t_end']:.1f} s", s["pose"]] + [f"{v:+.4f}" for v in s["mean_g"]]
                    + [f"{s.get('corrected_norm_error_mg', ''):+.1f}" if "corrected_norm_error_mg" in s else ""])
    res.table = rows
    # mark the still stretches on the timeline
    mark = np.zeros(len(t))
    for s in cal["stretches"]:
        mark[(t >= s["t_start"]) & (t <= s["t_end"])] = 1.0
    ws.put(Signal("cal.still", t, mark, "", "1 = a still stretch used for calibration", step=True))
    res.changed = ["imu.ax", "imu.ay", "imu.az", "cal.still"]
    if cal.get("method"):
        res.notes.append(f"{cal['method']}: offset {cal['offset_g']} g, scale {cal['scale']}, "
                         f"residual {cal['residual_rms_mg']} mg rms")
        if cal.get("gyro_offset_dps"):
            res.notes.append(f"gyro zero-rate offset {cal['gyro_offset_dps']['mean']} °/s")
        res.download = {"name": "calibration.json", "content": json.dumps(cal, indent=2)}
    return res
