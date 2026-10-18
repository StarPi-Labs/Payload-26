"""Clean blocks: filters and smoothers. Each works on any signals and either
replaces them or (with "keep original") adds a copy with a suffix."""
from dataclasses import replace

import numpy as np
from scipy import ndimage, signal as sps

from . import shared
from .catalog import Param, Result, block, p_keep, p_signals
from .signals import BlockError, effective_rate


def targets(ws, pattern):
    names = ws.select(pattern)
    if not names:
        raise BlockError(f"no signal matches '{pattern}'")
    return names


def store(ws, res, name, values, keep, suffix, **changes):
    sig = ws.signals[name]
    new = sig.with_values(values, **changes)
    if keep:
        new = new.renamed(f"{name}.{suffix}")
    ws.put(new)
    res.changed.append(new.name)


def rate_of(sig, res, need=8):
    """Sample rate for filters that assume even spacing, with a warning when
    the signal has gaps (ARMED-mode bursts, dropped data)."""
    if sig.n < need:
        raise BlockError(f"{sig.name} has only {sig.n} samples")
    fs = effective_rate(sig.t)
    steps = np.diff(sig.t)
    if steps.size and np.max(steps) > 5.0 / fs:
        res.warnings.append(f"{sig.name} has gaps (up to {np.max(steps):.2f} s): this filter treats its "
                            "samples as evenly spaced - crop to a gap-free part or resample first")
    return fs


def odd_window(seconds, fs, minimum=3):
    k = max(minimum, int(round(seconds * fs)))
    return k if k % 2 else k + 1


@block("average", "clean", "Moving average",
       "smooth {signals} with a moving average of {window} s|keep original {keep}",
       """
Replaces every sample by the mean of its neighbours within the window: the
simplest smoother. It removes noise well but also blurs fast events, and it
lets single spikes through as small bumps (use the median for spikes).

The window counts in seconds, not samples, so irregular sampling is handled
correctly.
""",
       [p_signals("baro.pressure"), Param("window", "number", 0.5, min=0.001, unit="s"), p_keep()])
def average(ws, p, ctx):
    res = Result()
    half = p["window"] / 2.0
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        v = np.where(np.isfinite(s.v), s.v, 0.0)
        ok = np.isfinite(s.v).astype(np.float64)
        c1 = np.concatenate([[0.0], np.cumsum(v)])
        cn = np.concatenate([[0.0], np.cumsum(ok)])
        lo = np.searchsorted(s.t, s.t - half, side="left")
        hi = np.searchsorted(s.t, s.t + half, side="right")
        cnt = cn[hi] - cn[lo]
        out = np.where(cnt > 0, (c1[hi] - c1[lo]) / np.maximum(cnt, 1), np.nan)
        store(ws, res, n, out, p["keep"], "avg")
    return res


@block("median", "clean", "Median filter",
       "remove spikes from {signals} with a median over {window} s|keep original {keep}",
       """
Replaces every sample by the median of its neighbours. Unlike the average it
removes single wild samples (I2C glitches, GPS jumps) completely, and it keeps
sharp steps sharp.
""",
       [p_signals("imu.a*"), Param("window", "number", 0.1, min=0.001, unit="s"), p_keep()])
def median(ws, p, ctx):
    res = Result()
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        k = odd_window(p["window"], rate_of(s, res))
        store(ws, res, n, ndimage.median_filter(s.v, size=k, mode="nearest"), p["keep"], "median")
    return res


def _butter(ws, p, btype, suffix):
    res = Result()
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        fs = rate_of(s, res, need=4 * p["order"] + 4)
        fc = p["cutoff"]
        if fc >= fs / 2:
            raise BlockError(f"{n} is sampled at {fs:.1f} Hz, so the cutoff must stay below {fs / 2:.1f} Hz")
        sos = sps.butter(p["order"], fc, btype=btype, fs=fs, output="sos")
        v = np.nan_to_num(s.v, nan=float(np.nanmean(s.v)) if np.any(np.isfinite(s.v)) else 0.0)
        if p["zero_phase"]:
            out = sps.sosfiltfilt(sos, v)
        else:
            zi = sps.sosfilt_zi(sos) * (v[0] if btype == "lowpass" else 0.0)
            out, _ = sps.sosfilt(sos, v, zi=zi)
        store(ws, res, n, out, p["keep"], suffix)
    res.notes.append(f"Butterworth order {p['order']}, {p['cutoff']:g} Hz, "
                     + ("zero phase (forward + backward)" if p["zero_phase"] else "causal (like on board: delays the signal)"))
    return res


BUTTER_PARAMS = [Param("order", "choice", "4", options=[("2", "2"), ("4", "4"), ("6", "6"), ("8", "8")])]


@block("lowpass", "clean", "Low-pass filter",
       "keep only changes slower than {cutoff} Hz in {signals}|order {order} · zero phase {zero_phase} · keep original {keep}",
       """
Keeps slow changes and removes fast jitter. The cutoff is the fastest change
you want to keep. Barometric altitude in flight: 2-5 Hz. Accelerometer for
trajectory: 10-20 Hz.

Zero phase runs the filter forwards and backwards. Nothing gets delayed, which
is what you want after the flight. Unticked, it behaves like a filter running
live on board: the output lags behind the input.
""",
       [p_signals("baro.pressure"), Param("cutoff", "number", 2.0, min=0.0001, unit="Hz"),
        BUTTER_PARAMS[0], Param("zero_phase", "bool", True), p_keep()])
def lowpass(ws, p, ctx):
    p = dict(p, order=int(p["order"]))
    return _butter(ws, p, "lowpass", "lp")


@block("highpass", "clean", "High-pass filter",
       "keep only changes faster than {cutoff} Hz in {signals}|order {order} · zero phase {zero_phase} · keep original {keep}",
       """
The opposite of the low-pass: removes slow drift (temperature, weather) and
keeps fast changes - vibration, the motor's buzz, the parachute shock.
""",
       [p_signals("imu.a*"), Param("cutoff", "number", 0.1, min=0.0001, unit="Hz"),
        BUTTER_PARAMS[0], Param("zero_phase", "bool", True), p_keep()])
def highpass(ws, p, ctx):
    p = dict(p, order=int(p["order"]))
    return _butter(ws, p, "highpass", "hp")


@block("notch", "clean", "Notch filter",
       "cut {freq} Hz out of {signals}|sharpness {q} · keep original {keep}",
       """
Removes one frequency and leaves the rest alone, for example a vibration line
in the spectrum (Analyze → spectrum shows where it is). Higher sharpness cuts
narrower.
""",
       [p_signals("imu.a*"), Param("freq", "number", 25.0, min=0.001, unit="Hz"),
        Param("q", "number", 10.0, min=0.5, max=200.0), p_keep()])
def notch(ws, p, ctx):
    res = Result()
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        fs = rate_of(s, res, need=20)
        if p["freq"] >= fs / 2:
            raise BlockError(f"{n} is sampled at {fs:.1f} Hz: a notch must stay below {fs / 2:.1f} Hz")
        b, a = sps.iirnotch(p["freq"], p["q"], fs=fs)
        store(ws, res, n, sps.filtfilt(b, a, np.nan_to_num(s.v)), p["keep"], "notch")
    return res


@block("savgol", "clean", "Savitzky-Golay",
       "smooth {signals} over {window} s keeping curves up to order {order}|keep original {keep}",
       """
Fits a small polynomial through each window of samples. It smooths noise like
an average does, but keeps peaks and curves where they are - good for altitude
around apogee, or for a signal you will differentiate next.
""",
       [p_signals("baro.pressure"), Param("window", "number", 1.0, min=0.001, unit="s"),
        Param("order", "int", 3, min=1, max=6), p_keep()])
def savgol(ws, p, ctx):
    res = Result()
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        k = odd_window(p["window"], rate_of(s, res), minimum=p["order"] + 2)
        if k > s.n:
            raise BlockError(f"the window ({k} samples) is longer than {n}")
        store(ws, res, n, sps.savgol_filter(np.nan_to_num(s.v), k, p["order"]), p["keep"], "sg")
    return res


@block("despike", "clean", "Remove outliers",
       "replace samples of {signals} further than {sigmas} σ from the local median ({window} s)|keep original {keep}",
       """
A Hampel filter. A sample further from the median of its neighbours than
`sigmas` times their typical spread is replaced by that median; everything
else is left exactly as it was. That makes it gentler than the median filter.
""",
       [p_signals("gps.alt"), Param("sigmas", "number", 3.0, min=1.0), Param("window", "number", 5.0, min=0.001, unit="s"),
        p_keep()])
def despike(ws, p, ctx):
    res = Result()
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        k = odd_window(p["window"], rate_of(s, res, need=3))
        med = ndimage.median_filter(s.v, size=k, mode="nearest")
        mad = ndimage.median_filter(np.abs(s.v - med), size=k, mode="nearest") * 1.4826
        bad = np.abs(s.v - med) > p["sigmas"] * np.maximum(mad, 1e-12)
        store(ws, res, n, np.where(bad, med, s.v), p["keep"], "clean")
        res.notes.append(f"{n}: {int(bad.sum())} of {s.n} samples replaced")
    return res


@block("resample", "clean", "Resample",
       "put {signals} on an even {rate} Hz grid by {method}",
       """
Puts signals on evenly spaced samples. Do this before filters that assume even
spacing, or to put several sensors on one shared clock (for Math, or for a
CSV export). Hold repeats the last value, which is right for states like the
flight mode.
""",
       [p_signals("baro.*"), Param("rate", "number", 50.0, min=0.001, unit="Hz"),
        Param("method", "choice", "linear", options=[("linear", "linear interpolation"), ("hold", "holding the last value")])])
def resample(ws, p, ctx):
    res = Result()
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        if s.n < 2:
            continue
        grid = np.arange(s.t[0], s.t[-1] + 0.5 / p["rate"], 1.0 / p["rate"])
        if len(grid) > 20_000_000:
            raise BlockError("that would create more than 20 million samples - crop first")
        if p["method"] == "hold" or s.step:
            idx = np.clip(np.searchsorted(s.t, grid, side="right") - 1, 0, s.n - 1)
            v = s.v[idx]
        else:
            v = np.interp(grid, s.t, s.v)
        ws.put(replace(s, t=grid, v=v))
        res.changed.append(n)
    return res


def _rts_level(t, y, r, q):
    """Scalar random-walk Kalman filter + RTS smoother (process noise q per second)."""
    n = len(y)
    xf = np.empty(n); pf = np.empty(n); pp = np.empty(n)
    x, P = float(y[0]), r
    tl, yl = t.tolist(), y.tolist()
    prev = tl[0]
    for k in range(n):
        dt = max(tl[k] - prev, 0.0)
        prev = tl[k]
        P_pred = P + q * dt
        pp[k] = P_pred
        if yl[k] == yl[k]:                       # not NaN
            K = P_pred / (P_pred + r)
            x = x + K * (yl[k] - x)
            P = (1.0 - K) * P_pred
        else:
            P = P_pred
        xf[k], pf[k] = x, P
    xs = xf.copy()
    for k in range(n - 2, -1, -1):
        G = pf[k] / pp[k + 1] if pp[k + 1] > 0 else 0.0
        xs[k] = xf[k] + G * (xs[k + 1] - xf[k])
    return xf, xs


@block("kalman_smooth", "clean", "Kalman smoother",
       "Kalman-smooth {signals} as {model}|sensor noise {noise} · may change by {change} per second · look back (RTS) {rts}|keep original {keep}",
       """
A Kalman filter for one signal at a time. You tell it two things:

- sensor noise: how much a single reading jumps around, in the signal's unit;
  the spread of the signal while nothing happens.
- change per second: how fast the true value may move. Small means very
  smooth but slow to follow; large follows quickly but keeps more noise.

"a level" assumes the value wanders slowly (temperature, pressure on the
pad). "a trend" assumes a slope that changes slowly (altitude in flight), so
it does not lag behind a climb.

Look back (RTS) runs the Rauch-Tung-Striebel smoother afterwards. It uses the
future samples as well, removes the filter's lag, and is what the website does
after the flight.
""",
       [p_signals("baro.pressure"),
        Param("model", "choice", "level", options=[("level", "a level"), ("trend", "a trend")]),
        Param("noise", "number", 0.01, min=1e-9), Param("change", "number", 0.01, min=1e-9),
        Param("rts", "bool", True), p_keep()])
def kalman_smooth(ws, p, ctx):
    res = Result()
    for n in targets(ws, p["signals"]):
        s = ws.signals[n]
        if s.n < 3:
            continue
        if s.n > 3_000_000:
            raise BlockError(f"{n} has {s.n:,} samples - crop first (this smoother runs sample by sample)")
        if p["model"] == "level":
            xf, xs = _rts_level(s.t, s.v, p["noise"] ** 2, p["change"] ** 2)
            out = xs if p["rts"] else xf
        else:
            dt = np.concatenate([[np.median(np.diff(s.t))], np.maximum(np.diff(s.t), 0.0)])
            ready = np.isfinite(s.v)
            ax = shared.kalman.filter_smooth_axis(
                dt, np.zeros(s.n), np.nan_to_num(s.v), ready, p["change"], p["noise"],
                p0=float(s.v[ready][0]) if ready.any() else 0.0, P0=(p["noise"] ** 2, 1.0),
                smooth=p["rts"])
            out = ax.p if p["rts"] else ax.p_fwd
        store(ws, res, n, out, p["keep"], "kf")
    return res
