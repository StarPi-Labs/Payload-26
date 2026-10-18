"""Derive blocks: new signals computed from existing ones."""
import numpy as np

from . import shared
from .catalog import Param, Result, block, p_signal
from .signals import BlockError, Signal, values_at


def new_name(name):
    name = (name or "").strip()
    if not name or any(c in name for c in " ,*?[]"):
        raise BlockError(f"'{name}' is not a usable signal name (no spaces, commas or wildcards)")
    return name


@block("math", "derive", "Math",
       "make {name} = {a} {op} {b}|or, with no second signal, the number {value}|unit {unit}",
       """
Combines two signals sample by sample - or a signal and a number when the
second signal is left at "(none)". The second signal is interpolated onto the
first one's samples, so sensors with different rates combine correctly.

Examples: power = power.voltage × power.current ÷ 1000 (two Math blocks),
the difference between two filters, a unit change.
""",
       [Param("name", "text", "power.watts"), p_signal("a", "power.voltage"),
        Param("op", "choice", "*", options=[("+", "+"), ("-", "−"), ("*", "×"), ("/", "÷"),
                                            ("min", "min"), ("max", "max")]),
        p_signal("b", "power.current", optional=True),
        Param("value", "number", 0.001, precision=0.000001, show_if={"b": ["(none)", ""]}),
        Param("unit", "text", "W")])
def math_block(ws, p, ctx):
    a = ws.get(p["a"])
    b = values_at(ws.get(p["b"]), a.t) if p["b"] else np.full(a.n, p["value"])
    op = p["op"]
    with np.errstate(divide="ignore", invalid="ignore"):
        v = {"+": a.v + b, "-": a.v - b, "*": a.v * b, "/": a.v / b,
             "min": np.fmin(a.v, b), "max": np.fmax(a.v, b)}[op]
    name = new_name(p["name"])
    ws.put(Signal(name, a.t, v, p["unit"] or a.unit, f"{p['a']} {op} {p['b'] or p['value']}"))
    res = Result(changed=[name])
    if p["b"] and not np.all(np.isfinite(b)):
        res.warnings.append(f"{p['b']} does not cover all of {p['a']}: those samples are empty")
    return res


@block("magnitude", "derive", "Magnitude",
       "make {name} = length of ({x}, {y}, {z})",
       """
The length of a 3-axis vector, sqrt(x² + y² + z²). For the accelerometer it is
the total specific force: 1 g at rest, whatever the orientation, and 0 g in
free fall.
""",
       [Param("name", "text", "imu.amag"), p_signal("x", "imu.ax"), p_signal("y", "imu.ay"),
        p_signal("z", "imu.az")])
def magnitude(ws, p, ctx):
    x = ws.get(p["x"])
    y = values_at(ws.get(p["y"]), x.t)
    z = values_at(ws.get(p["z"]), x.t)
    name = new_name(p["name"])
    ws.put(Signal(name, x.t, np.sqrt(x.v ** 2 + y ** 2 + z ** 2), x.unit, "vector magnitude"))
    return Result(changed=[name])


@block("derivative", "derive", "Derivative",
       "make {name} = rate of change of {signal}",
       """
How fast a signal changes, per second: altitude to vertical speed, speed to
acceleration. Differentiating amplifies noise a lot, so smooth first (low-pass,
Savitzky-Golay or a Kalman smoother).
""",
       [Param("name", "text", "baro.vspeed"), p_signal("signal", "baro.altitude")])
def derivative(ws, p, ctx):
    s = ws.get(p["signal"])
    if s.n < 3:
        raise BlockError(f"{s.name} has too few samples")
    t, keep = np.unique(s.t, return_index=True)
    name = new_name(p["name"])
    ws.put(Signal(name, t, np.gradient(s.v[keep], t), f"{s.unit}/s" if s.unit else "1/s",
                  f"derivative of {s.name}"))
    return Result(changed=[name])


@block("integral", "derive", "Integral",
       "make {name} = running total of {signal} starting at {start}",
       """
Adds a signal up over time (trapezoid rule): acceleration to speed, speed to
distance, power to energy. Integration drifts, because every bit of bias adds
up, so remove the bias first (pad calibration) or let a Kalman filter tie it
to a real measurement.
""",
       [Param("name", "text", "power.energy"), p_signal("signal", "power.watts"),
        Param("start", "number", 0.0)])
def integral(ws, p, ctx):
    s = ws.get(p["signal"])
    dt = np.diff(s.t)
    v = np.nan_to_num(s.v)
    total = np.concatenate([[0.0], np.cumsum(0.5 * (v[1:] + v[:-1]) * dt)]) + p["start"]
    name = new_name(p["name"])
    ws.put(Signal(name, s.t, total, f"{s.unit}·s" if s.unit else "s", f"integral of {s.name}"))
    res = Result(changed=[name])
    if s.unit == "W":
        res.notes.append(f"{total[-1] - p['start']:.1f} J = {(total[-1] - p['start']) / 3600:.4f} Wh")
    return res


@block("baro_altitude", "derive", "Altitude from pressure",
       "make {name} = height above the pad from {pressure}|reference: mean of the first {seconds} s",
       """
Barometric altitude, 44330 · (1 − (p / p0)^(1/5.255)) m: the formula the
Dashboard and trajectory.py use. p0 is the pressure on the pad: the mean over
the first seconds (0 = the very first sample), so the result reads about 0 m
before launch.

Accuracy: about ±1 m relative (BME680 ±0.12 hPa), plus slow weather drift over
hours.
""",
       [Param("name", "text", "baro.altitude"), p_signal("pressure", "baro.pressure", filter="baro.*"),
        Param("seconds", "number", 5.0, min=0.0, unit="s")])
def baro_altitude(ws, p, ctx):
    s = ws.get(p["pressure"])
    good = np.isfinite(s.v) & (s.v > 1.0)
    if not good.any():
        raise BlockError(f"{s.name} has no valid pressure")
    t0 = s.t[good][0]
    ref = s.v[good & (s.t <= t0 + p["seconds"])]
    p0 = float(np.mean(ref)) if p["seconds"] > 0 and len(ref) else float(s.v[good][0])
    alt = shared.trajectory.barometric_altitude(s.v, p0)
    name = new_name(p["name"])
    ws.put(Signal(name, s.t, alt, "m", f"barometric altitude, p0 = {p0:.4f} {s.unit}"))
    return Result(changed=[name], notes=[f"reference pressure p0 = {p0:.4f} {s.unit}"])


@block("gps_local", "derive", "GPS to metres",
       "make gps.east and gps.north from {lat} and {lon}|origin: mean of the first {fixes} fixes",
       """
Turns latitude/longitude into metres east and north of the launch point (flat
earth, accurate over a few km) - the units the Kalman filter works in. The
origin is the mean of the first fixes (the rocket on the pad).

GPS accuracy: about 1.5 m CEP when still, worse in flight.
""",
       [p_signal("lat", "gps.lat", filter="gps.*"), p_signal("lon", "gps.lon", filter="gps.*"),
        Param("fixes", "int", 10, min=1)])
def gps_local(ws, p, ctx):
    if p["lat"] not in ws.signals or ws.signals[p["lat"]].n == 0:
        return Result(warnings=["this recording has no GPS fix, so there is nothing to convert "
                                "(the Kalman filter then runs without GPS)"])
    lat = ws.get(p["lat"])
    lon = ws.get(p["lon"])
    k = min(p["fixes"], lat.n)
    lat0, lon0 = float(np.mean(lat.v[:k])), float(np.mean(lon.v[:k]))
    east, north = shared.trajectory.gps_to_enu(lat.v, values_at(lon, lat.t), lat0, lon0)
    ws.put(Signal("gps.east", lat.t, east, "m", "metres east of the origin"))
    ws.put(Signal("gps.north", lat.t, north, "m", "metres north of the origin"))
    return Result(changed=["gps.east", "gps.north"],
                  notes=[f"origin {lat0:.6f}, {lon0:.6f}"])
