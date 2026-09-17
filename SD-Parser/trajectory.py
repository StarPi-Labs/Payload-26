#!/usr/bin/env python3
"""
trajectory.py -- rocket trajectory from a Pi-LOG recording, using the linear
Kalman filter and Rauch-Tung-Striebel smoother from matlab/LKF.mlx and
matlab/RTS smoother.mlx (implemented in kalman.py).

    python trajectory.py flight.bin
    python trajectory.py flight.bin --params noise_params.json --plot
    python trajectory.py 20hourTest.bin --output-rate 1 --plot

Outputs, next to the source unless --output is given:
    <name>_trajectory.csv           smoothed position/velocity (+1-sigma) per step
    <name>_trajectory_summary.json apogee, velocities, consistency diagnostics
    <name>_trajectory.png          with --plot

What the Live Scripts leave open, and how it is filled in here
--------------------------------------------------------------
The filter's input u is AxLKF/AyLKF/AzLKF: acceleration in the SAME frame as the
position states, with gravity removed. The MPU measures body-frame specific
force and the firmware has no attitude estimate, so this script builds that
input:

  * Rest window. The quietest few seconds before launch (smallest spread of |a|)
    give the gravity direction, the magnitude the accelerometer reads for g, the
    gyro bias, and the pressure reference for barometric altitude.

  * Frame. Local East-North-Up. "Up" is exact, from gravity. The horizontal axes
    need the payload's heading, which nothing on board observes (there is no
    magnetometer), so --heading-deg states it: the compass heading of the body
    X axis. Only the horizontal channels depend on it -- altitude and vertical
    velocity, the ones that matter for apogee, do not.

  * Attitude over time. 'static' (default) holds the rest orientation: right for
    a bench recording and a near-vertical boost. 'gyro' integrates the
    bias-corrected gyro, following a rotating vehicle but drifting over hours.

  * Gravity is removed using the magnitude the accelerometer itself reads at
    rest, not 9.80665 m/s^2. That cancels the static scale/bias error along the
    vertical: the 20 h bench recording reads 0.967 g at rest, so subtracting 1 g
    would inject a constant 0.33 m/s^2 of phantom acceleration.

Measurements enter on the IMU time grid exactly like the script's
Gpsready/Baroready flags: each fix/sample marks the first IMU step at or after
its own timestamp.

Process noise: Qimu = diag(sigma_acc^2). By default sigma_acc is the spread of
the navigation-frame acceleration over the rest window -- "derived directly from
the noise characteristics of the IMU", as LKF.mlx puts it. A real flight adds
vibration and attitude error on top of sensor noise; --q-scale inflates it.
"""
import argparse
import csv
import io
import json
import math
import os
import sys
from contextlib import redirect_stdout
from dataclasses import dataclass, field

import numpy as np

import kalman
import pilog_reader

G0 = 9.80665
EARTH_RADIUS_M = 6371008.8
MODE_NAMES = ["INIT", "POST", "SNSCHK", "ARMED", "BOOST", "COAST"]
MODE_BOOST = 4

# LKF.mlx: devGpsx = devGpsy = 2.5, devbaro = 1.0
DEFAULT_SIGMA_GPS = 2.5
DEFAULT_SIGMA_BARO = 1.0
MIN_SIGMA_ACC = 0.005


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

def level_rotation(f_rest, heading_deg=0.0):
    """Body -> ENU rotation matrix from the specific force measured at rest.

    At rest the accelerometer reads the reaction to gravity, which points up, so
    f_rest/|f_rest| is the body-frame direction of nav +Z. The horizontal pair is
    fixed by assuming the body X axis (or Y, if X is near vertical) points to
    `heading_deg` on the compass. a_nav = R @ a_body.
    """
    up = np.asarray(f_rest, dtype=np.float64)
    up = up / np.linalg.norm(up)
    ref = np.array([1.0, 0.0, 0.0]) if abs(up[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
    hx = ref - np.dot(ref, up) * up
    hx /= np.linalg.norm(hx)
    hl = np.cross(up, hx)                 # 90 deg to the left of hx, seen from above
    psi = math.radians(heading_deg)
    north = math.cos(psi) * hx + math.sin(psi) * hl
    east = math.sin(psi) * hx - math.cos(psi) * hl
    return np.vstack([east, north, up])


def _rotmat_to_quat(R):
    """Rotation matrix -> unit quaternion (w, x, y, z), Shepherd's method."""
    m = R
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    cands = [tr, m[0, 0], m[1, 1], m[2, 2]]
    i = int(np.argmax(cands))
    if i == 0:
        s = math.sqrt(tr + 1.0) * 2.0
        q = (0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s)
    elif i == 1:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2.0
        q = ((m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s)
    elif i == 2:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2.0
        q = ((m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s)
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2.0
        q = ((m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s)
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q)


def rotate_gyro_propagated(R0, acc_body_mps2, gyro_dps, gyro_bias_dps, dt):
    """Rotate body acceleration into ENU with an attitude integrated from the gyro.

    q (body->nav) starts from R0 and is right-multiplied by the incremental body
    rotation each step. Pure-Python scalar quaternion math; fine for flights,
    slow and drifting for multi-hour recordings.
    """
    w, x, y, z = _rotmat_to_quat(R0)
    n = len(dt)
    out = np.empty((n, 3))
    ax, ay, az = acc_body_mps2[:, 0].tolist(), acc_body_mps2[:, 1].tolist(), acc_body_mps2[:, 2].tolist()
    d2r = math.pi / 180.0
    gx = ((gyro_dps[:, 0] - gyro_bias_dps[0]) * d2r).tolist()
    gy = ((gyro_dps[:, 1] - gyro_bias_dps[1]) * d2r).tolist()
    gz = ((gyro_dps[:, 2] - gyro_bias_dps[2]) * d2r).tolist()
    dts = dt.tolist()

    for k in range(n):
        h = dts[k]
        rx, ry, rz = gx[k] * h, gy[k] * h, gz[k] * h
        ang = math.sqrt(rx * rx + ry * ry + rz * rz)
        if ang > 1e-12:
            half = 0.5 * ang
            s = math.sin(half) / ang
            dw, dx, dy, dz = math.cos(half), rx * s, ry * s, rz * s
            # q = q * dq
            w, x, y, z = (w * dw - x * dx - y * dy - z * dz,
                          w * dx + x * dw + y * dz - z * dy,
                          w * dy - x * dz + y * dw + z * dx,
                          w * dz + x * dy - y * dx + z * dw)
            norm = math.sqrt(w * w + x * x + y * y + z * z)
            w, x, y, z = w / norm, x / norm, y / norm, z / norm
        # v' = v + 2w (u x v) + 2 u x (u x v)
        vx, vy, vz = ax[k], ay[k], az[k]
        cx, cy, cz = y * vz - z * vy, z * vx - x * vz, x * vy - y * vx
        ccx, ccy, ccz = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
        out[k, 0] = vx + 2.0 * (w * cx + ccx)
        out[k, 1] = vy + 2.0 * (w * cy + ccy)
        out[k, 2] = vz + 2.0 * (w * cz + ccz)
    return out


def barometric_altitude(pressure_kpa, p_ref_kpa):
    """Same formula as json2telemetry.py, so filtered and raw altitudes compare."""
    ratio = np.asarray(pressure_kpa, dtype=np.float64) / p_ref_kpa
    return 44330.0 * (1.0 - np.power(ratio, 1.0 / 5.255))


def gps_to_enu(lat, lon, lat0, lon0):
    """Local east/north metres (equirectangular; accurate over a few km)."""
    k = math.pi / 180.0 * EARTH_RADIUS_M
    east = (np.asarray(lon) - lon0) * k * math.cos(math.radians(lat0))
    north = (np.asarray(lat) - lat0) * k
    return east, north


def align_to_grid(t_grid, t_meas, values):
    """Mark the first grid step at or after each measurement; latest one wins."""
    n = len(t_grid)
    ready = np.zeros(n, dtype=bool)
    out = np.zeros((n,) + tuple(values.shape[1:]), dtype=np.float64)
    if len(t_meas) == 0:
        return out, ready
    idx = np.searchsorted(t_grid, t_meas, side="left")
    ok = idx < n
    idx, vals = idx[ok], values[ok]
    if len(idx) == 0:
        return out, ready
    # np.unique on the reversed indices keeps the LAST measurement per step.
    _, first_rev = np.unique(idx[::-1], return_index=True)
    keep = len(idx) - 1 - first_rev
    ready[idx[keep]] = True
    out[idx[keep]] = vals[keep]
    return out, ready


def quietest_window(t, x, duration, t_limit):
    """Start/end indices of the window of `duration` seconds with the smallest
    standard deviation of x, among samples with t <= t_limit."""
    lim = int(np.searchsorted(t, t_limit, side="right"))
    lim = max(lim, 2)
    if lim < 3:
        return 0, min(len(t), 2)
    # Rate from sample count over time, NOT the median step: with duplicated or
    # tick-quantised stamps the median step can be 0, which made the "5 s"
    # window swallow the entire pre-launch recording.
    span = float(t[lim - 1] - t[0])
    fs = (lim - 1) / span if span > 0 else 100.0
    w = int(round(duration * fs))
    if w >= lim:
        return 0, lim
    w = max(w, 2)
    xs = x[:lim].astype(np.float64)
    c1 = np.concatenate([[0.0], np.cumsum(xs)])
    c2 = np.concatenate([[0.0], np.cumsum(xs * xs)])
    s1 = c1[w:] - c1[:-w]
    s2 = c2[w:] - c2[:-w]
    var = np.maximum(s2 / w - (s1 / w) ** 2, 0.0)
    i = int(np.argmin(var))
    return i, i + w


# ---------------------------------------------------------------------------
# Preprocessing
# ---------------------------------------------------------------------------

@dataclass
class Prepared:
    t: np.ndarray                 # s since the first IMU sample of the session
    t_origin: float               # onboard seconds of that first sample
    dt: np.ndarray
    acc: np.ndarray               # (N, 3) ENU, gravity removed, m/s^2
    baro_z: np.ndarray
    baro_ready: np.ndarray
    gps_xy: np.ndarray            # (N, 2) east, north metres
    gps_ready: np.ndarray
    baro_raw: tuple               # (t, z) for plotting
    gps_raw: tuple                # (t, east, north)
    rest: dict = field(default_factory=dict)
    origin: dict = field(default_factory=dict)
    modes: list = field(default_factory=list)
    quality: dict = field(default_factory=dict)
    warnings: list = field(default_factory=list)


def prepare(rec, heading_deg=0.0, attitude="static", rest_duration=5.0, rest_search=120.0):
    imu = rec.imu
    if len(imu.get("t", [])) < 10:
        raise ValueError("recording has fewer than 10 IMU samples")

    order = np.argsort(imu["t"], kind="stable")
    t_logged = imu["t"][order]
    warnings = []

    # Frame stamps are FreeRTOS ticks (10 ms), while the IMU samples every 12 ms
    # (83.33 Hz). Integrating with the logged 10/20 ms steps would put up to 8 ms
    # of error into every dt, so rebuild real sample times first - see
    # pilog_reader.timing_quality() / dequantise_times().
    imu_timing = pilog_reader.timing_quality(t_logged)
    t_raw = pilog_reader.dequantise_times(t_logged, imu_timing)
    # Barometer and GPS stamps come from the same truncating tick: shift them by
    # the same half tick so all sensors stay on one time base.
    stamp_bias = 0.5 * imu_timing["timestamp_resolution_s"] if imu_timing.get("timestamps_quantised") else 0.0
    t_origin = float(t_raw[0])
    t = t_raw - t_origin
    acc_body_g = np.column_stack([imu["ax"][order], imu["ay"][order], imu["az"][order]])
    gyro_dps = np.column_stack([imu["gx"][order], imu["gy"][order], imu["gz"][order]])

    steps = np.diff(t)
    nominal = imu_timing.get("period_s") or (float(np.median(steps[steps > 0])) if np.any(steps > 0) else 0.01)
    dt = np.concatenate([[nominal], np.maximum(steps, 0.0)])
    duration = float(t[-1])
    quality = {"imu": imu_timing, "duration_s": round(duration, 3)}
    warnings.extend(pilog_reader.timing_warnings("IMU", imu_timing))

    modes = sorted((m[0] - t_origin, MODE_NAMES[m[1]] if 0 <= m[1] < len(MODE_NAMES) else str(m[1]))
                   for m in rec.modes)
    launch = next((mt for mt, name in modes if name == "BOOST" and mt >= 0), None)

    # ---- rest window: quietest stretch before launch -------------------------
    amag = np.linalg.norm(acc_body_g, axis=1)
    limit = rest_search if launch is None else min(rest_search, max(launch - 1.0, rest_duration))
    i0, i1 = quietest_window(t, amag, rest_duration, limit)
    f0 = acc_body_g[i0:i1].mean(axis=0)
    g_local = float(np.linalg.norm(f0))
    gyro_bias = gyro_dps[i0:i1].mean(axis=0)
    amag_std = float(amag[i0:i1].std())
    if amag_std > 0.02:
        warnings.append(f"rest window is not still (|a| std {amag_std:.3f} g) - gravity "
                        "alignment and bias estimates will be poor")
    if abs(g_local - 1.0) > 0.05:
        warnings.append(f"accelerometer reads {g_local:.3f} g at rest - calibrate it; "
                        "subtracting the measured value compensates along the vertical")

    R0 = level_rotation(f0, heading_deg)
    acc_body = acc_body_g * G0
    if attitude == "gyro":
        acc_nav = rotate_gyro_propagated(R0, acc_body, gyro_dps, gyro_bias, dt)
        if duration > 1800:
            warnings.append("gyro attitude over a long recording drifts; 'static' is usually "
                            "the better choice for bench data")
    else:
        acc_nav = acc_body @ R0.T
    acc_nav[:, 2] -= g_local * G0

    # ---- barometer -------------------------------------------------------------
    b = rec.baro
    baro_z = np.zeros(len(t)); baro_ready = np.zeros(len(t), dtype=bool)
    baro_raw = (np.empty(0), np.empty(0))
    p_ref = None
    if len(b.get("t", [])):
        quality["barometer"] = pilog_reader.timing_quality(b["t"])
        warnings.extend(pilog_reader.timing_warnings("barometer", quality["barometer"]))
        bt = b["t"] + stamp_bias - t_origin
        good = np.isfinite(b["pressure"]) & (b["pressure"] > 1.0)
        bt, bp = bt[good], b["pressure"][good]
        in_rest = (bt >= t[i0]) & (bt <= t[i1 - 1])
        p_ref = float(np.mean(bp[in_rest])) if np.any(in_rest) else (float(bp[0]) if len(bp) else None)
        if p_ref:
            bz = barometric_altitude(bp, p_ref)
            baro_z, baro_ready = align_to_grid(t, bt, bz)
            baro_raw = (bt, bz)
    else:
        warnings.append("no barometer samples: vertical position is unconstrained")

    # ---- GPS ---------------------------------------------------------------------
    gp = rec.gps
    gps_xy = np.zeros((len(t), 2)); gps_ready = np.zeros(len(t), dtype=bool)
    gps_raw = (np.empty(0), np.empty(0), np.empty(0))
    lat0 = lon0 = None
    if len(gp.get("t", [])):
        quality["gps"] = pilog_reader.timing_quality(gp["t"])
        gt = gp["t"] + stamp_bias - t_origin
        good = np.isfinite(gp["lat"]) & np.isfinite(gp["lon"])
        gt, glat, glon = gt[good], gp["lat"][good], gp["lon"][good]
        if len(gt):
            in_rest = (gt >= t[i0] - 5.0) & (gt <= t[i1 - 1] + 5.0)
            lat0 = float(np.mean(glat[in_rest])) if np.any(in_rest) else float(glat[0])
            lon0 = float(np.mean(glon[in_rest])) if np.any(in_rest) else float(glon[0])
            ge, gn = gps_to_enu(glat, glon, lat0, lon0)
            gps_xy, gps_ready = align_to_grid(t, gt, np.column_stack([ge, gn]))
            gps_raw = (gt, ge, gn)
    if not np.any(gps_ready):
        warnings.append("no GPS fixes: horizontal position is unconstrained")

    return Prepared(
        t=t, t_origin=t_origin, dt=dt, acc=acc_nav,
        baro_z=baro_z, baro_ready=baro_ready, gps_xy=gps_xy, gps_ready=gps_ready,
        baro_raw=baro_raw, gps_raw=gps_raw,
        rest={"t_start": round(float(t[i0]), 3), "t_end": round(float(t[i1 - 1]), 3),
              "samples": int(i1 - i0), "g_measured": round(g_local, 5),
              "accel_mean_g": [round(float(v), 5) for v in f0],
              "gyro_bias_dps": [round(float(v), 4) for v in gyro_bias],
              "accel_magnitude_std_g": round(amag_std, 5),
              "acc_nav_std_mps2": [round(float(v), 5) for v in acc_nav[i0:i1].std(axis=0)],
              "pressure_ref_kpa": None if p_ref is None else round(p_ref, 5)},
        origin={"lat0": lat0, "lon0": lon0, "heading_deg": heading_deg, "attitude": attitude},
        modes=modes, quality=quality, warnings=warnings,
    )


# ---------------------------------------------------------------------------
# Filtering (segmented) and result collection
# ---------------------------------------------------------------------------

class Collector:
    """Receives each kept stretch of smoothed output: tracks statistics, writes
    decimated CSV rows, gathers plot series and (optionally) full-resolution
    channels, without ever holding the whole result for a long recording."""

    COLUMNS = ["time", "x", "vx", "y", "vy", "z", "vz",
               "sigma_x", "sigma_vx", "sigma_y", "sigma_vy", "sigma_z", "sigma_vz",
               "z_filtered", "vz_filtered", "ax_nav", "ay_nav", "az_nav"]

    def __init__(self, prep, csv_path=None, output_rate=0.0, plot_points=20000, keep_full=False):
        self.prep = prep
        self.output_rate = output_rate
        self.stride = max(1, len(prep.t) // plot_points)
        self.keep_full = keep_full
        self.full = ({k: [] for k in ("t", "z", "vz", "vx", "vy", "sigma_z", "z_fwd")}
                     if keep_full else None)
        self.plot = {k: [] for k in ("t", "z", "vz", "zf", "vzf", "sz", "svz", "x", "y", "az")}
        self.last_bin = None
        self.rows = 0
        self.stats = {"z_max": -math.inf, "t_z_max": None, "vz_max": -math.inf, "t_vz_max": None,
                      "vz_min": math.inf, "t_vz_min": None, "horiz_max": 0.0,
                      "a_max": 0.0, "t_a_max": None}
        self.nis = {"baro": [0.0, 0], "gps_x": [0.0, 0], "gps_y": [0.0, 0]}
        self.resid = {"baro": [0.0, 0], "gps": [0.0, 0]}
        self._fh = None
        self._writer = None
        if csv_path:
            self._fh = open(csv_path, "w", newline="")
            self._writer = csv.writer(self._fh)
            self._writer.writerow(self.COLUMNS)

    def close(self):
        if self._fh:
            self._fh.close()

    def consume(self, start, keep_end, res):
        n = keep_end - start
        p = self.prep
        t = p.t[start:keep_end]
        x, y, z = res["x"], res["y"], res["z"]
        zz, vz = z.p[:n], z.v[:n]
        xs, ys = x.p[:n], y.p[:n]
        acc = p.acc[start:keep_end]

        st = self.stats
        i = int(np.argmax(zz))
        if zz[i] > st["z_max"]:
            st["z_max"], st["t_z_max"] = float(zz[i]), float(t[i])
        i = int(np.argmax(vz))
        if vz[i] > st["vz_max"]:
            st["vz_max"], st["t_vz_max"] = float(vz[i]), float(t[i])
        i = int(np.argmin(vz))
        if vz[i] < st["vz_min"]:
            st["vz_min"], st["t_vz_min"] = float(vz[i]), float(t[i])
        st["horiz_max"] = max(st["horiz_max"], float(np.max(np.hypot(xs, ys))))
        amag = np.linalg.norm(acc, axis=1)
        i = int(np.argmax(amag))
        if amag[i] > st["a_max"]:
            st["a_max"], st["t_a_max"] = float(amag[i]), float(t[i])

        # Innovation consistency: count each update once (overlaps are recomputed).
        for key, axis in (("baro", z), ("gps_x", x), ("gps_y", y)):
            m = axis.update_steps < n
            if np.any(m):
                self.nis[key][0] += float(np.sum(axis.innovations[m] ** 2 / axis.innovation_var[m]))
                self.nis[key][1] += int(np.sum(m))
        br = p.baro_ready[start:keep_end]
        if np.any(br):
            d = zz[br] - p.baro_z[start:keep_end][br]
            self.resid["baro"][0] += float(np.sum(d * d)); self.resid["baro"][1] += int(d.size)
        gr = p.gps_ready[start:keep_end]
        if np.any(gr):
            gxy = p.gps_xy[start:keep_end][gr]
            d = np.hypot(xs[gr] - gxy[:, 0], ys[gr] - gxy[:, 1])
            self.resid["gps"][0] += float(np.sum(d * d)); self.resid["gps"][1] += int(d.size)

        # CSV rows at the requested output rate.
        if self._writer is not None:
            if self.output_rate and self.output_rate > 0:
                bins = np.floor(t * self.output_rate).astype(np.int64)
                first = np.ones(n, dtype=bool)
                first[1:] = bins[1:] != bins[:-1]
                if self.last_bin is not None and n:
                    first[0] = bins[0] != self.last_bin
                self.last_bin = int(bins[-1]) if n else self.last_bin
                sel = np.nonzero(first)[0]
            else:
                sel = np.arange(n)
            if len(sel):
                block = np.column_stack([
                    t[sel], xs[sel], x.v[:n][sel], ys[sel], y.v[:n][sel], zz[sel], vz[sel],
                    np.sqrt(x.var_p[:n][sel]), np.sqrt(x.var_v[:n][sel]),
                    np.sqrt(y.var_p[:n][sel]), np.sqrt(y.var_v[:n][sel]),
                    np.sqrt(z.var_p[:n][sel]), np.sqrt(z.var_v[:n][sel]),
                    z.p_fwd[:n][sel], z.v_fwd[:n][sel],
                    acc[sel, 0], acc[sel, 1], acc[sel, 2]])
                buf = io.StringIO()
                np.savetxt(buf, block, delimiter=",", fmt="%.5f")
                self._fh.write(buf.getvalue())
                self.rows += len(sel)

        # Plot series, globally striding.
        gidx = np.arange(start, keep_end)
        ps = np.nonzero(gidx % self.stride == 0)[0]
        pl = self.plot
        pl["t"].append(t[ps]); pl["z"].append(zz[ps]); pl["vz"].append(vz[ps])
        pl["zf"].append(z.p_fwd[:n][ps]); pl["vzf"].append(z.v_fwd[:n][ps])
        pl["sz"].append(np.sqrt(z.var_p[:n][ps])); pl["svz"].append(np.sqrt(z.var_v[:n][ps]))
        pl["x"].append(xs[ps]); pl["y"].append(ys[ps]); pl["az"].append(acc[ps, 2])

        if self.keep_full:
            f = self.full
            f["t"].append(t); f["z"].append(zz.copy()); f["vz"].append(vz.copy())
            f["vx"].append(x.v[:n].copy()); f["vy"].append(y.v[:n].copy())
            f["sigma_z"].append(np.sqrt(z.var_p[:n]))
            f["z_fwd"].append(z.p_fwd[:n].copy())

    def finish_series(self, d):
        return {k: (np.concatenate(v) if v else np.empty(0)) for k, v in d.items()}


def run_filter(prep, sigma_acc, sigma_gps, sigma_baro, collector, segment_seconds=3600.0,
               overlap_seconds=300.0, smooth=True, progress=None):
    """LKF + RTS over the whole recording, in overlapping segments.

    Each segment's forward pass starts from the previous segment's forward
    state, so the forward (filtered) result is identical to one unbroken run.
    Only the smoother's look-ahead is bounded, by the overlap, which is far
    longer than the smoother's memory at these measurement rates.
    """
    n = len(prep.t)
    nominal = prep.quality["imu"]["period_s"]
    seg = n if not segment_seconds else max(1000, int(segment_seconds / nominal))
    # The overlap must stay below the segment length so each segment still
    # advances; half a segment is plenty of smoother look-ahead.
    ov = min(int(overlap_seconds / nominal), seg // 2)
    axes = {
        "x": (prep.acc[:, 0], prep.gps_xy[:, 0], prep.gps_ready, sigma_acc[0], sigma_gps[0]),
        "y": (prep.acc[:, 1], prep.gps_xy[:, 1], prep.gps_ready, sigma_acc[1], sigma_gps[1]),
        "z": (prep.acc[:, 2], prep.baro_z, prep.baro_ready, sigma_acc[2], sigma_baro),
    }
    state = {k: (0.0, 0.0, kalman.DEFAULT_P0) for k in axes}   # m0 = 0, P0 from LKF.mlx
    start = 0
    while start < n:
        end = min(n, start + seg)
        keep_end = n if end == n else max(start + 1, end - ov)
        res = {}
        for k, (acc, meas, ready, s_acc, s_meas) in axes.items():
            p0, v0, P0 = state[k]
            res[k] = kalman.filter_smooth_axis(prep.dt[start:end], acc[start:end], meas[start:end],
                                               ready[start:end], s_acc, s_meas,
                                               p0=p0, v0=v0, P0=P0, smooth=smooth)
        collector.consume(start, keep_end, res)
        j = keep_end - 1 - start
        for k in axes:
            r = res[k]
            state[k] = (float(r.p_fwd[j]), float(r.v_fwd[j]),
                        (float(r.var_p_fwd[j]), float(r.cov_pv_fwd[j]), float(r.var_v_fwd[j])))
        if progress:
            progress(keep_end / n)
        start = keep_end


# ---------------------------------------------------------------------------
# Top level
# ---------------------------------------------------------------------------

def resolve_sigmas(prep, params=None, sigma_acc=None, sigma_gps=None, sigma_baro=None, q_scale=1.0):
    rec = (params or {}).get("recommended", {})
    if sigma_acc is None:
        sigma_acc = rec.get("sigma_acc") or prep.rest["acc_nav_std_mps2"]
    sigma_acc = [max(MIN_SIGMA_ACC, float(s)) * q_scale
                 for s in np.broadcast_to(np.asarray(sigma_acc, dtype=float), (3,))]
    if sigma_gps is None:
        sigma_gps = rec.get("sigma_gps") or [DEFAULT_SIGMA_GPS, DEFAULT_SIGMA_GPS]
    sigma_gps = [float(s) for s in np.broadcast_to(np.asarray(sigma_gps, dtype=float), (2,))]
    if sigma_baro is None:
        sigma_baro = rec.get("sigma_baro") or DEFAULT_SIGMA_BARO
    return sigma_acc, sigma_gps, float(sigma_baro)


def estimate(rec, heading_deg=0.0, attitude="static", params=None, sigma_acc=None,
             sigma_gps=None, sigma_baro=None, q_scale=1.0, csv_path=None, output_rate=0.0,
             segment_seconds=3600.0, overlap_seconds=300.0, keep_full=False, smooth=True,
             progress=None):
    """Run the whole pipeline on a Recording. Returns (prep, collector, sigmas)."""
    prep = prepare(rec, heading_deg=heading_deg, attitude=attitude)
    sigmas = resolve_sigmas(prep, params, sigma_acc, sigma_gps, sigma_baro, q_scale)
    collector = Collector(prep, csv_path=csv_path, output_rate=output_rate, keep_full=keep_full)
    try:
        run_filter(prep, *sigmas, collector, segment_seconds=segment_seconds,
                   overlap_seconds=overlap_seconds, smooth=smooth, progress=progress)
    finally:
        collector.close()
    nis = collector.nis["baro"]
    if nis[1] and nis[0] / nis[1] > 3.0:
        prep.warnings.append(
            f"barometer NIS {nis[0] / nis[1]:.1f} (should be ~1): the filter is over-confident in "
            "the IMU. Sensor noise alone does not cover flight vibration or accelerometer scale "
            "error - raise --q-scale until NIS approaches 1.")
    return prep, collector, sigmas


def build_summary(rec, prep, col, sigmas):
    st = col.stats
    launch = next((mt for mt, name in prep.modes if name == "BOOST" and mt >= 0), None)

    def rel(tv):
        return None if tv is None or launch is None else round(tv - launch, 3)

    def mean_nis(key):
        s, c = col.nis[key]
        return None if c == 0 else round(s / c, 3)

    def rms(key):
        s, c = col.resid[key]
        return None if c == 0 else round(math.sqrt(s / c), 3)

    summary = {
        "source": os.path.abspath(rec.path),
        "session": prep_session(rec),
        "data_quality": prep.quality,
        "rest_window": prep.rest,
        "frame": prep.origin,
        "mode_transitions": [{"time_s": round(mt, 3), "mode": name} for mt, name in prep.modes],
        "noise_parameters": {
            "sigma_acc_mps2": [round(s, 5) for s in sigmas[0]],
            "sigma_gps_m": sigmas[1], "sigma_baro_m": sigmas[2],
            "P0": list(kalman.DEFAULT_P0),
        },
        "results": {
            "apogee_m": round(st["z_max"], 3), "apogee_time_s": st["t_z_max"],
            "apogee_time_after_launch_s": rel(st["t_z_max"]),
            "max_vertical_velocity_mps": round(st["vz_max"], 3), "max_vertical_velocity_time_s": st["t_vz_max"],
            "max_descent_rate_mps": round(-st["vz_min"], 3), "max_descent_time_s": st["t_vz_min"],
            "max_acceleration_mps2": round(st["a_max"], 3), "max_acceleration_time_s": st["t_a_max"],
            "max_horizontal_distance_m": round(st["horiz_max"], 3),
            "launch_time_s": None if launch is None else round(launch, 3),
        },
        "consistency": {
            "about": "Mean normalised innovation squared (NIS) should be close to 1 when the "
                     "noise parameters match reality: well above 1 means the filter is "
                     "over-confident (raise sigma_acc via --q-scale, or the measurement sigma); "
                     "well below 1 means it is too pessimistic.",
            "nis_baro": mean_nis("baro"), "nis_gps_x": mean_nis("gps_x"), "nis_gps_y": mean_nis("gps_y"),
            "baro_updates": col.nis["baro"][1], "gps_updates": col.nis["gps_x"][1],
            "rms_smoothed_minus_baro_m": rms("baro"), "rms_smoothed_minus_gps_m": rms("gps"),
        },
        "warnings": prep.warnings,
        "csv_rows": col.rows,
    }
    return summary


def prep_session(rec):
    return rec.sessions[0] if len(rec.sessions) == 1 else rec.sessions


def plot(path, prep, col, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    s = col.finish_series(col.plot)
    t = s["t"]
    hours = t[-1] > 7200 if len(t) else False
    scale, unit = (3600.0, "h") if hours else (1.0, "s")
    tt = t / scale

    fig, axs = plt.subplots(4, 1, figsize=(12, 13), sharex=False)
    fig.suptitle(title, fontsize=11)

    bt, bz = prep.baro_raw
    if len(bt):
        stride = max(1, len(bt) // 20000)
        axs[0].plot(bt[::stride] / scale, bz[::stride], ".", ms=1.5, color="0.7", label="barometer (raw)")
    axs[0].plot(tt, s["zf"], lw=0.8, color="tab:orange", label="filtered (LKF)")
    axs[0].fill_between(tt, s["z"] - 2 * s["sz"], s["z"] + 2 * s["sz"], color="tab:blue", alpha=0.15,
                        label="smoothed ±2σ")
    axs[0].plot(tt, s["z"], lw=1.0, color="tab:blue", label="smoothed (RTS)")
    axs[0].set_ylabel("altitude above pad [m]")

    axs[1].plot(tt, s["vzf"], lw=0.8, color="tab:orange", label="filtered")
    axs[1].fill_between(tt, s["vz"] - 2 * s["svz"], s["vz"] + 2 * s["svz"], color="tab:blue", alpha=0.15)
    axs[1].plot(tt, s["vz"], lw=1.0, color="tab:blue", label="smoothed")
    axs[1].set_ylabel("vertical velocity [m/s]")

    axs[2].plot(tt, s["az"], lw=0.5, color="tab:green")
    axs[2].set_ylabel("vertical acceleration\ninput u_z [m/s²]")

    gt, ge, gn = prep.gps_raw
    if len(gt):
        stride = max(1, len(gt) // 20000)
        axs[3].plot(ge[::stride], gn[::stride], ".", ms=2, color="0.7", label="GPS fixes")
    axs[3].plot(s["x"], s["y"], lw=1.0, color="tab:blue", label="smoothed track")
    axs[3].set_xlabel("east [m]"); axs[3].set_ylabel("north [m]")
    axs[3].set_aspect("equal", adjustable="datalim")

    for ax in axs[:3]:
        ax.set_xlabel(f"time [{unit}]")
        for mt, name in prep.modes:
            if name in ("BOOST", "COAST"):
                ax.axvline(mt / scale, color="tab:red", lw=0.8, ls="--", alpha=0.6)
    for ax in axs:
        ax.grid(alpha=0.3)
        if ax.get_legend_handles_labels()[0]:
            ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="recording: raw .bin, or bin2json.py output .json")
    ap.add_argument("--output", help="output path prefix (default: next to the source)")
    ap.add_argument("--params", help="noise_params.json from noise_characterization.py")
    ap.add_argument("--sigma-acc", type=float, nargs="+", metavar="MPS2",
                    help="accelerometer noise std: one value, or x y z (default: from the rest window)")
    ap.add_argument("--sigma-gps", type=float, nargs="+", metavar="M",
                    help=f"GPS std: one value, or east north (default {DEFAULT_SIGMA_GPS})")
    ap.add_argument("--sigma-baro", type=float, metavar="M",
                    help=f"barometric altitude std (default {DEFAULT_SIGMA_BARO})")
    ap.add_argument("--q-scale", type=float, default=1.0,
                    help="multiply sigma_acc, to cover flight vibration and attitude error")
    ap.add_argument("--heading-deg", type=float, default=0.0,
                    help="compass heading of the body X axis (0 = north). Horizontal only.")
    ap.add_argument("--attitude", choices=["static", "gyro"], default="static")
    ap.add_argument("--session", type=int, help="session id (default: the longest)")
    ap.add_argument("--output-rate", type=float,
                    help="CSV rows per second (default: every step, or 10 Hz above 1 hour)")
    ap.add_argument("--segment-hours", type=float, default=1.0,
                    help="split long recordings into segments of this length (memory bound)")
    ap.add_argument("--no-smooth", action="store_true", help="forward filter only")
    ap.add_argument("--plot", action="store_true", help="write a PNG overview")
    args = ap.parse_args()

    prefix = args.output or os.path.splitext(args.source)[0] + "_trajectory"
    print(f"[+] reading {args.source}")
    with redirect_stdout(io.StringIO()):
        rec = pilog_reader.load_recording(args.source)
    print(f"    {rec.frames:,} frames, {len(rec.sessions)} session(s), CRC failures {rec.crc_failures}")
    if len(rec.sessions) > 1:
        for s in rec.sessions:
            print(f"      session {s['id']}: {s['t_start']:.2f}-{s['t_end']:.2f} s, {s['frames']:,} frames")
    rec = pilog_reader.select_session(rec, args.session)

    params = None
    if args.params:
        with open(args.params, "r", encoding="utf-8") as f:
            params = json.load(f)

    duration = float(rec.imu["t"][-1] - rec.imu["t"][0]) if len(rec.imu.get("t", [])) else 0.0
    rate = args.output_rate if args.output_rate is not None else (10.0 if duration > 3600 else 0.0)

    def progress(frac):
        print(f"\r    filtering {frac * 100:5.1f}%", end="", flush=True)

    prep, col, sigmas = estimate(
        rec, heading_deg=args.heading_deg, attitude=args.attitude, params=params,
        sigma_acc=args.sigma_acc, sigma_gps=args.sigma_gps, sigma_baro=args.sigma_baro,
        q_scale=args.q_scale, csv_path=prefix + ".csv", output_rate=rate,
        segment_seconds=args.segment_hours * 3600.0, smooth=not args.no_smooth, progress=progress)
    print()

    summary = build_summary(rec, prep, col, sigmas)
    with open(prefix + "_summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    r, c = summary["results"], summary["consistency"]
    print(f"[+] {col.rows:,} rows -> {prefix}.csv")
    # Console output stays ASCII: Windows consoles fall back to cp1252 when output
    # is piped, which cannot encode characters such as the square-root sign.
    print(f"    sigma_acc {[round(s, 4) for s in sigmas[0]]} m/s^2  sigma_gps {sigmas[1]} m  "
          f"sigma_baro {sigmas[2]} m")
    print(f"    apogee {r['apogee_m']:.1f} m @ {r['apogee_time_s']:.1f} s   "
          f"max vz {r['max_vertical_velocity_mps']:.2f} m/s   "
          f"max |a| {r['max_acceleration_mps2']:.1f} m/s^2")
    print(f"    NIS baro {c['nis_baro']}  gps {c['nis_gps_x']}/{c['nis_gps_y']}   "
          f"(~1 means the noise model fits)")
    for w in prep.warnings:
        print(f"[!] {w}")
    if args.plot:
        plot(prefix + ".png", prep, col, os.path.basename(args.source))
        print(f"[+] plot -> {prefix}.png")
    print(f"[+] summary -> {prefix}_summary.json")


if __name__ == "__main__":
    main()
