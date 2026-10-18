"""
Signals and the workspace a recipe passes from block to block.

A signal is one named series with its own x axis - usually time, because every
sensor of the payload runs at its own rate (IMU 83 Hz, barometer 10-50 Hz, GPS
1 Hz...). Blocks never modify arrays in place: they put new Signal objects into
a copy of the workspace, so the snapshot after every block stays intact and the
lab can show any block's "before" and "after" for free.
"""
import fnmatch
import math
from dataclasses import dataclass, replace

import numpy as np


@dataclass(frozen=True)
class Signal:
    name: str
    t: np.ndarray            # x values: seconds, or Hz / seconds of averaging time
    v: np.ndarray
    unit: str = ""
    desc: str = ""
    axis: str = "time"       # "time" | "frequency" (spectra) | "tau" (Allan deviation)
    step: bool = False       # a state, drawn as steps (flight mode)

    def with_values(self, v, **changes):
        return replace(self, v=np.asarray(v, dtype=np.float64), **changes)

    def renamed(self, name):
        return replace(self, name=name)

    @property
    def n(self):
        return len(self.t)

    def info(self):
        out = {"name": self.name, "unit": self.unit, "desc": self.desc, "axis": self.axis,
               "step": self.step, "n": int(self.n)}
        if self.n:
            out["x0"] = float(self.t[0])
            out["x1"] = float(self.t[-1])
            if self.axis == "time" and self.n > 1 and self.t[-1] > self.t[0]:
                out["rate"] = round((self.n - 1) / float(self.t[-1] - self.t[0]), 3)
        return out


def stats(v):
    """mean/std/min/max/rms of the finite values, as plain floats (or None)."""
    v = np.asarray(v, dtype=np.float64)
    f = v[np.isfinite(v)]
    if f.size == 0:
        return {"n": 0, "mean": None, "std": None, "min": None, "max": None, "rms": None}
    return {"n": int(f.size), "mean": float(f.mean()), "std": float(f.std()),
            "min": float(f.min()), "max": float(f.max()),
            "rms": float(math.sqrt(float(np.mean(f * f))))}


def effective_rate(t):
    """Samples per second from count over time (robust to tick-quantised stamps)."""
    t = np.asarray(t, dtype=np.float64)
    if len(t) < 2 or t[-1] <= t[0]:
        return float("nan")
    return (len(t) - 1) / float(t[-1] - t[0])


def values_at(sig, t_new):
    """sig's values linearly interpolated at t_new; NaN outside its range."""
    t_new = np.asarray(t_new, dtype=np.float64)
    if sig.n == 0:
        return np.full(len(t_new), np.nan)
    out = np.interp(t_new, sig.t, sig.v)
    out[(t_new < sig.t[0]) | (t_new > sig.t[-1])] = np.nan
    return out


def decimate(t, v, max_points):
    """At most max_points points that keep every bucket's minimum and maximum,
    so spikes survive any zoom level. Returns index array into t/v."""
    n = len(t)
    if n <= max_points:
        return np.arange(n)
    buckets = max(1, max_points // 2)
    size = int(math.ceil(n / buckets))
    pad = buckets * size - n
    vv = np.asarray(v, dtype=np.float64)
    if pad:
        vv = np.concatenate([vv, np.full(pad, np.nan)])
    grid = vv.reshape(buckets, size)
    finite = np.isfinite(grid)
    has = finite.any(axis=1)
    lo = np.where(finite, grid, np.inf).argmin(axis=1)
    hi = np.where(finite, grid, -np.inf).argmax(axis=1)
    base = np.arange(buckets) * size
    idx = np.concatenate([base + lo, base + hi, base[~has]])   # all-NaN bucket: keep its first
    idx = idx[idx < n]
    idx = np.unique(np.concatenate([idx, [0, n - 1]]))
    return idx


class Workspace:
    """Signals plus named intermediate results (attitude quaternions, pad
    statistics, a calibration...) that later blocks may use."""

    def __init__(self, signals=None, objects=None, meta=None):
        self.signals = dict(signals or {})
        self.objects = dict(objects or {})
        self.meta = meta or {}

    def copy(self):
        return Workspace(self.signals, self.objects, self.meta)

    def names(self):
        return list(self.signals)

    def select(self, pattern, axis="time"):
        """Signal names matching a pattern: one or more comma-separated names
        or globs ("imu.a*", "baro.pressure, gps.alt"), in workspace order."""
        if pattern is None:
            return []
        parts = [p.strip() for p in str(pattern).split(",") if p.strip()]
        out = []
        for name, sig in self.signals.items():
            if axis and sig.axis != axis:
                continue
            if any(fnmatch.fnmatchcase(name, p) for p in parts) and name not in out:
                out.append(name)
        return out

    def get(self, name):
        try:
            return self.signals[name]
        except KeyError:
            raise BlockError(f"there is no signal '{name}' at this point of the recipe") from None

    def put(self, sig):
        self.signals[sig.name] = sig


class BlockError(Exception):
    """A problem the user can fix (wrong signal, bad parameter): shown on the block."""
