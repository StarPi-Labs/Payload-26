"""Estimate blocks: attitude, Earth-frame acceleration, the Kalman filter and
the website's trajectory pipeline - the EuRoC estimator, block by block."""
import math

import numpy as np

from . import shared
from .blocks_fix import ACCEL, GYRO, imu_signals
from .catalog import Param, Result, block, p_signal
from .signals import BlockError, Signal, values_at

G0 = 9.80665
D2R = math.pi / 180.0
MAX_LOOP_SAMPLES = 5_000_000


# ---------------------------------------------------------------------------
# Attitude
# ---------------------------------------------------------------------------

def _start_quaternion(acc, n_start):
    """Body -> pad orientation from the mean of the first samples' gravity."""
    m = np.array([a.v[:n_start].mean() for a in acc])
    return shared.attitude.initial_orientation(m)


def _run_filter(t, acc, gyr, method, kp, ki, beta, alpha):
    """Classic per-sample attitude filters (no gating), for comparison with the
    Dashboard's. All start levelled from the first 0.5 s and return body -> pad
    quaternions, shape (n, 4)."""
    n = len(t)
    fs = (n - 1) / (t[-1] - t[0]) if t[-1] > t[0] else 100.0
    w, x, y, z = _start_quaternion(acc, max(1, min(n, int(0.5 * fs))))
    tl = t.tolist()
    ax, ay, az = (a.v.tolist() for a in acc)
    gx, gy, gz = ((g.v * D2R).tolist() for g in gyr)
    out = np.empty((n, 4))
    ix = iy = iz = 0.0                                   # Mahony integral term
    sqrt, sin, cos = math.sqrt, math.sin, math.cos
    for k in range(n):
        if k:
            dt = tl[k] - tl[k - 1]
            if dt <= 0.0:
                out[k] = (w, x, y, z)
                continue
            if dt > 0.1:
                dt = 0.1
            wx, wy, wz = gx[k - 1], gy[k - 1], gz[k - 1]
            a1, a2, a3 = ax[k - 1], ay[k - 1], az[k - 1]
            an = sqrt(a1 * a1 + a2 * a2 + a3 * a3)
            if method != "gyro" and an > 1e-9:
                mx, my, mz = a1 / an, a2 / an, a3 / an
                vx = 2.0 * (x * z - w * y)                # estimated up, body frame
                vy = 2.0 * (y * z + w * x)
                vz = w * w - x * x - y * y + z * z
                ex, ey, ez = my * vz - mz * vy, mz * vx - mx * vz, mx * vy - my * vx
                if method == "mahony":
                    ix += ki * ex * dt
                    iy += ki * ey * dt
                    iz += ki * ez * dt
                    wx += kp * ex + ix
                    wy += kp * ey + iy
                    wz += kp * ez + iz
                elif method == "complementary":
                    gain = (1.0 - alpha) / dt
                    wx += gain * ex
                    wy += gain * ey
                    wz += gain * ez
                elif method == "madgwick":
                    # Madgwick's IMU update: q' = 1/2 q (x) omega - beta * grad / |grad|,
                    # grad = J^T f with f = estimated up (body) - measured up.
                    fx_, fy_, fz_ = vx - mx, vy - my, (1.0 - 2.0 * (x * x + y * y)) - mz
                    g0 = -2.0 * y * fx_ + 2.0 * x * fy_
                    g1 = 2.0 * z * fx_ + 2.0 * w * fy_ - 4.0 * x * fz_
                    g2 = -2.0 * w * fx_ + 2.0 * z * fy_ - 4.0 * y * fz_
                    g3 = 2.0 * x * fx_ + 2.0 * y * fy_
                    gn = sqrt(g0 * g0 + g1 * g1 + g2 * g2 + g3 * g3)
                    qd0 = 0.5 * (-x * wx - y * wy - z * wz)
                    qd1 = 0.5 * (w * wx + y * wz - z * wy)
                    qd2 = 0.5 * (w * wy - x * wz + z * wx)
                    qd3 = 0.5 * (w * wz + x * wy - y * wx)
                    if gn > 1e-12:
                        qd0 -= beta * g0 / gn
                        qd1 -= beta * g1 / gn
                        qd2 -= beta * g2 / gn
                        qd3 -= beta * g3 / gn
                    w, x, y, z = w + qd0 * dt, x + qd1 * dt, y + qd2 * dt, z + qd3 * dt
                    nn = 1.0 / sqrt(w * w + x * x + y * y + z * z)
                    w, x, y, z = w * nn, x * nn, y * nn, z * nn
                    out[k] = (w, x, y, z)
                    continue
            hx, hy, hz = 0.5 * wx * dt, 0.5 * wy * dt, 0.5 * wz * dt
            th = sqrt(hx * hx + hy * hy + hz * hz)
            if th > 1e-12:
                s = sin(th) / th
                dw, dx, dy, dz = cos(th), hx * s, hy * s, hz * s
                w, x, y, z = (w * dw - x * dx - y * dy - z * dz, w * dx + x * dw + y * dz - z * dy,
                              w * dy - x * dz + y * dw + z * dx, w * dz + x * dy - y * dx + z * dw)
                nn = 1.0 / sqrt(w * w + x * x + y * y + z * z)
                w, x, y, z = w * nn, x * nn, y * nn, z * nn
        out[k] = (w, x, y, z)
    return out


def _angles(q):
    w, x, y, z = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    tilt = np.degrees(np.arccos(np.clip(1.0 - 2.0 * (x * x + y * y), -1.0, 1.0)))
    spin = np.degrees(np.unwrap(2.0 * np.arctan2(z, w)))      # twist about the long axis
    yaw = np.degrees(np.arctan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)))
    pitch = np.degrees(np.arcsin(np.clip(2 * (w * y - z * x), -1.0, 1.0)))
    roll = np.degrees(np.arctan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)))
    return tilt, spin, yaw, pitch, roll


@block("attitude", "estimate", "Attitude",
       "estimate the orientation with {method}|Mahony: Kp {kp} Ki {ki}|Madgwick: β {beta}|complementary: α {alpha}",
       """
Works out which way the payload points, from the gyro (how fast it turns)
and the accelerometer (where gravity is while it is still).

- "the Dashboard's filter": what the rocket view uses. Gravity only corrects
  while the payload is provably still, never in flight, plus bias tracking and
  data-gap handling.
- Mahony: the filter in the team's MATLAB estimator (imufilter, Mahony). Kp is
  how hard gravity pulls the estimate; Ki slowly learns the gyro bias.
- Madgwick: a popular alternative; β plays Kp's role.
- complementary: each sample, a fraction (1 − α) of the error to gravity is
  corrected.
- gyro only: pure integration, which drifts but is untouched by acceleration.

The classic filters trust the accelerometer all the time, so during boost
they lean towards the thrust: compare them with the Dashboard's.

Outputs: att.tilt (nose angle from vertical), att.spin (turned about the long
axis), and MATLAB's euler(q, 'ZYX') angles att.yaw / att.pitch / att.roll.
""",
       [Param("method", "choice", "dashboard",
              options=[("dashboard", "the Dashboard's filter"), ("mahony", "Mahony"),
                       ("madgwick", "Madgwick"), ("complementary", "complementary"),
                       ("gyro", "gyro only")]),
        Param("kp", "number", 0.5, min=0.0, show_if={"method": ["mahony"]}),
        Param("ki", "number", 0.0, min=0.0, show_if={"method": ["mahony"]}),
        Param("beta", "number", 0.1, min=0.0, show_if={"method": ["madgwick"]}),
        Param("alpha", "number", 0.98, min=0.0, max=1.0, show_if={"method": ["complementary"]})])
def attitude_block(ws, p, ctx):
    acc = imu_signals(ws, ACCEL)
    gyr = imu_signals(ws, GYRO)
    t = acc[0].t
    if not np.array_equal(t, gyr[0].t):
        raise BlockError("accelerometer and gyro must share their samples (resample both or neither)")
    if len(t) > MAX_LOOP_SAMPLES:
        raise BlockError(f"{len(t):,} IMU samples - crop first (attitude runs sample by sample)")
    res = Result()
    if p["method"] == "dashboard":
        launch = ws.meta.get("launch")
        cols, info = shared.attitude.estimate(t, *[a.v for a in acc], *[g.v for g in gyr],
                                              flight_start=launch)
        q = np.column_stack([cols[c] for c in shared.attitude.ATTITUDE_COLUMNS]).astype(np.float64)
        res.notes.append(f"anchored at {info.get('restWindow')}, gyro bias {info.get('gyroBiasStartDps')} °/s, "
                         f"{info.get('correctedRows', 0):,} gravity-corrected samples")
    else:
        q = _run_filter(t, acc, gyr, p["method"], p["kp"], p["ki"], p["beta"], p["alpha"])
    tilt, spin, yaw, pitch, roll = _angles(q)
    for name, v, desc in (("att.tilt", tilt, "nose angle from vertical"),
                          ("att.spin", spin, "turned about the long axis (unwrapped)"),
                          ("att.yaw", yaw, "MATLAB euler ZYX: about Z"),
                          ("att.pitch", pitch, "MATLAB euler ZYX: about Y"),
                          ("att.roll", roll, "MATLAB euler ZYX: about X")):
        ws.put(Signal(name, t, v, "°", desc))
        res.changed.append(name)
    ws.objects["attitude"] = {"t": t, "q": q, "method": p["method"]}
    res.changed = ["att.tilt", "att.spin"]
    return res


@block("earth_frame", "estimate", "Rotate to Earth frame",
       "turn the accelerometer into east/north/up and remove {g}|the payload's front faces {heading}° (compass)",
       """
Uses the attitude above to rotate every accelerometer sample from the payload's
axes into east / north / up, then subtracts gravity. What is left is the
rocket's own acceleration: AxLKF, AyLKF and AzLKF in the MATLAB estimator, the
input of its Kalman filter.

Gravity: "the measured 1 g" subtracts what the accelerometer itself reads at
rest. That cancels its scale error (this MPU reads ~0.96 g); subtracting a
standard 9.81 m/s² would leave a phantom 0.4 m/s² climb.

Heading: nothing on board measures the compass direction, so give the
direction the payload's front (+X, away from the rail) faced on the pad. Only
east/north depend on it, not altitude.
""",
       [Param("g", "choice", "measured", options=[("measured", "the measured 1 g"),
                                                   ("standard", "9.80665 m/s²")]),
        Param("heading", "number", 0.0, min=-360.0, max=360.0, unit="°")])
def earth_frame(ws, p, ctx):
    att = ws.objects.get("attitude")
    if not att:
        raise BlockError("needs an Attitude block above")
    acc = imu_signals(ws, ACCEL)
    t = acc[0].t
    if len(t) != len(att["t"]) or not np.array_equal(t, att["t"]):
        raise BlockError("the accelerometer's samples changed after the Attitude block - "
                         "put this block right after it")
    q = att["q"]
    w, x, y, z = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    fx, fy, fz = (a.v * G0 for a in acc)
    cx, cy, cz = y * fz - z * fy, z * fx - x * fz, x * fy - y * fx
    px = fx + 2.0 * (w * cx + y * cz - z * cy)
    py = fy + 2.0 * (w * cy + z * cx - x * cz)
    pz = fz + 2.0 * (w * cz + x * cy - y * cx)
    if p["g"] == "measured":
        pad = ws.objects.get("pad")
        if pad and pad.get("g_measured"):
            g = pad["g_measured"] * G0
            src = "pad calibration"
        else:
            n0 = max(10, min(len(t), int(2 * (len(t) - 1) / max(t[-1] - t[0], 1e-9))))
            g = float(np.median(np.sqrt(fx[:n0] ** 2 + fy[:n0] ** 2 + fz[:n0] ** 2)))
            src = "first 2 s"
    else:
        g, src = G0, "standard"
    psi = math.radians(p["heading"])
    east = math.sin(psi) * px - math.cos(psi) * py
    north = math.cos(psi) * px + math.sin(psi) * py
    up = pz - g
    for name, v, d in (("nav.ax", east, "acceleration east, gravity removed"),
                       ("nav.ay", north, "acceleration north"),
                       ("nav.az", up, "acceleration up, gravity removed")):
        ws.put(Signal(name, t, v, "m/s²", d))
    return Result(changed=["nav.ax", "nav.ay", "nav.az"],
                  notes=[f"gravity {g:.4f} m/s² ({src})"])


@block("lkf_rts", "estimate", "Kalman filter + RTS",
       "Kalman filter on nav.a* with altitude {altitude} and GPS {east} / {north}|σ accel {sigma_acc} m/s² (0 = from the pad) × {q_scale} · σ GPS {sigma_gps} m · σ baro {sigma_baro} m|look back (RTS) {smooth}",
       """
The team's linear Kalman filter (LKF.mlx) and RTS smoother (RTS smoother.mlx),
computed by SD-Parser's kalman.py. The state is position and velocity on
three axes. The Earth-frame acceleration predicts each step; barometric
altitude and GPS east/north correct it whenever they have a sample.

- σ accel: the accelerometer noise, from Qimu. 0 takes it from the Pad
  calibration block, or from the first two seconds. In flight, vibration adds
  far more than sensor noise, so raise the ×-factor until the baro NIS below
  is near 1.
- σ GPS 2.5 m and σ baro 1.0 m are the MATLAB script's values.
- RTS runs a second pass backwards from the end. It removes the filter's lag
  and is what the website uses.

NIS (normalised innovation squared) checks the noise model: about 1 is
right; well above 1 means the filter trusts the IMU too much.
""",
       [p_signal("altitude", "baro.altitude"), p_signal("east", "gps.east", optional=True),
        p_signal("north", "gps.north", optional=True),
        Param("sigma_acc", "number", 0.0, min=0.0, precision=0.0001),
        Param("q_scale", "number", 1.0, min=0.01), Param("sigma_gps", "number", 2.5, min=0.01),
        Param("sigma_baro", "number", 1.0, min=0.01), Param("smooth", "bool", True)])
def lkf_rts(ws, p, ctx):
    tr = shared.trajectory
    nav = [ws.get(n) for n in ("nav.ax", "nav.ay", "nav.az")]
    t = nav[0].t
    if len(t) < 10:
        raise BlockError("needs the Rotate to Earth frame block above")
    if len(t) > MAX_LOOP_SAMPLES:
        raise BlockError(f"{len(t):,} samples - crop first")
    acc = np.column_stack([s.v for s in nav])
    steps = np.diff(t)
    dt = np.concatenate([[float(np.median(steps)) if len(steps) else 0.01], np.maximum(steps, 0.0)])
    alt = ws.get(p["altitude"])
    baro_z, baro_ready = tr.align_to_grid(t, alt.t, alt.v)
    res = Result()
    have_gps = bool(p["east"] and p["north"])
    if have_gps and not (p["east"] in ws.signals and p["north"] in ws.signals):
        res.warnings.append(f"no {p['east']} / {p['north']} here (no GPS fix, or no 'GPS to metres' block "
                            "above): running without GPS")
        have_gps = False
    if have_gps:
        e, nn = ws.get(p["east"]), ws.get(p["north"])
        gps_xy, gps_ready = tr.align_to_grid(t, e.t, np.column_stack([e.v, values_at(nn, e.t)]))
    else:
        gps_xy, gps_ready = np.zeros((len(t), 2)), np.zeros(len(t), dtype=bool)
    if p["sigma_acc"] > 0:
        sig = [p["sigma_acc"]] * 3
    elif ws.objects.get("pad"):
        sig = ws.objects["pad"]["accel_std_mps2"]
        res.notes.append("σ accel from the pad calibration (Qimu)")
    else:
        n0 = max(10, int(2.0 / max(float(np.median(steps)), 1e-6)))
        sig = list(acc[:n0].std(axis=0))
        res.notes.append("σ accel from the spread of the first 2 s")
    sig = [max(tr.MIN_SIGMA_ACC, s) * p["q_scale"] for s in sig]
    out = shared.kalman.lkf_rts(dt, acc, gps_xy, gps_ready, baro_z, baro_ready,
                                sigma_acc=sig, sigma_gps=(p["sigma_gps"], p["sigma_gps"]),
                                sigma_baro=p["sigma_baro"], smooth=p["smooth"])
    for axis, ax_res, unit_p in (("x", out.x, "east"), ("y", out.y, "north"), ("z", out.z, "up")):
        ws.put(Signal(f"kf.{axis}", t, ax_res.p, "m", f"position {unit_p} (Kalman)"))
        ws.put(Signal(f"kf.v{axis}", t, ax_res.v, "m/s", f"velocity {unit_p} (Kalman)"))
    ws.put(Signal("kf.z.sigma", t, np.sqrt(out.z.var_p), "m", "1-sigma of kf.z"))
    ws.put(Signal("kf.vz.sigma", t, np.sqrt(out.z.var_v), "m/s", "1-sigma of kf.vz"))
    res.changed = ["kf.z", "kf.vz"]
    i = int(np.argmax(out.z.p))
    res.notes.append(f"apogee {out.z.p[i]:.1f} m at {t[i]:.2f} s, max climb {np.max(out.z.v):.1f} m/s")
    for key, axis in (("baro", out.z), ("GPS east", out.x)):
        if len(axis.innovations):
            nis = float(np.mean(axis.innovations ** 2 / axis.innovation_var))
            res.notes.append(f"NIS {key} {nis:.2f} (about 1 = the noise model fits)")
            if key == "baro" and nis > 3:
                res.warnings.append("baro NIS well above 1: the filter trusts the IMU too much - raise the × factor")
    if not gps_ready.any():
        res.notes.append("no GPS used: east/north drift freely")
    res.table = [["", "x (east)", "y (north)", "z (up)"], ["σ accel [m/s²]"] + [f"{s:.4f}" for s in sig]]
    return res


@block("website_trajectory", "estimate", "Website trajectory (one block)",
       "run the Dashboard's trajectory estimate|attitude {attitude} · process noise × {q_scale} · front faces {heading}°",
       """
Exactly what `json2telemetry.py --kalman` does for the website, in one block.
It calls SD-Parser's trajectory.py on this recipe's IMU, barometer and GPS
signals:

1. the stillest stretch before launch gives the gravity direction, the gyro
   bias and the pressure reference;
2. "static" holds that orientation through the flight (right for a
   near-vertical boost), "gyro" integrates the gyro;
3. gravity is removed using the magnitude the accelerometer reads at rest;
4. LKF + RTS, with the timestamps de-quantised.

Outputs kf.altitude, kf.vz and kf.vh, the Dashboard's kfAltitude,
kfVerticalVelocity and kfHorizontalVelocity. Put cleaning blocks above it to
see how they would change the website's result.
""",
       [Param("attitude", "choice", "static", options=[("static", "static"), ("gyro", "gyro")]),
        Param("q_scale", "number", 1.0, min=0.01),
        Param("heading", "number", 0.0, min=-360.0, max=360.0, unit="°")])
def website_trajectory(ws, p, ctx):
    pr, tr = shared.pilog_reader, shared.trajectory

    def arr(name, t_ref=None):
        if name not in ws.signals:
            return None
        s = ws.signals[name]
        return s.v if t_ref is None else values_at(s, t_ref)

    if not all(n in ws.signals for n in ACCEL + GYRO):
        raise BlockError("needs the IMU signals imu.ax ... imu.gz")
    it = ws.signals["imu.ax"].t
    imu = {"t": it, "ax": arr("imu.ax"), "ay": arr("imu.ay"), "az": arr("imu.az"),
           "gx": arr("imu.gx"), "gy": arr("imu.gy"), "gz": arr("imu.gz"),
           "temp": arr("imu.temp", it) if "imu.temp" in ws.signals else np.full(len(it), np.nan),
           "session": np.zeros(len(it))}
    baro = {}
    if "baro.pressure" in ws.signals:
        bt = ws.signals["baro.pressure"].t
        baro = {"t": bt, "pressure": arr("baro.pressure"),
                "temperature": arr("baro.temperature", bt) if "baro.temperature" in ws.signals else np.full(len(bt), np.nan),
                "humidity": arr("baro.humidity", bt) if "baro.humidity" in ws.signals else np.full(len(bt), np.nan),
                "session": np.zeros(len(bt))}
    gps = {}
    if "gps.lat" in ws.signals:
        gt = ws.signals["gps.lat"].t
        gps = {"t": gt, "lat": arr("gps.lat"), "lon": arr("gps.lon", gt), "alt": arr("gps.alt", gt),
               "hvel": arr("gps.speed", gt), "sats": arr("gps.sats", gt), "session": np.zeros(len(gt))}
    modes = []
    if "mode" in ws.signals:
        m = ws.signals["mode"]
        modes = [(float(a), int(b), 0) for a, b in zip(m.t, m.v)]
    rec = pr.Recording(path=str(ws.meta.get("recording", {}).get("label", "")), imu=imu, baro=baro,
                       gps=gps, ina={}, modes=modes,
                       sessions=[{"id": 0, "t_start": float(it[0]), "t_end": float(it[-1]), "frames": len(it)}])
    try:
        prep, col, sigmas = tr.estimate(rec, heading_deg=p["heading"], attitude=p["attitude"],
                                        q_scale=p["q_scale"], keep_full=True)
    except ValueError as exc:
        raise BlockError(str(exc)) from None
    full = col.finish_series(col.full)
    t = full["t"] + prep.t_origin
    ws.put(Signal("kf.altitude", t, full["z"], "m", "website: Kalman/RTS altitude above the pad"))
    ws.put(Signal("kf.vz", t, full["vz"], "m/s", "website: Kalman/RTS vertical velocity"))
    ws.put(Signal("kf.vh", t, np.hypot(full["vx"], full["vy"]), "m/s", "website: Kalman/RTS horizontal speed"))
    ws.put(Signal("kf.altitude.sigma", t, full["sigma_z"], "m", "website: 1-sigma of kf.altitude"))
    summary = tr.build_summary(rec, prep, col, sigmas)
    r, c = summary["results"], summary["consistency"]
    res = Result(changed=["kf.altitude", "kf.vz"])
    res.notes.append(f"apogee {r['apogee_m']:.1f} m, max climb {r['max_vertical_velocity_mps']:.1f} m/s, "
                     f"max |a| {r['max_acceleration_mps2']:.1f} m/s²")
    res.notes.append(f"NIS baro {c['nis_baro']} (about 1 = the noise model fits), "
                     f"σ accel {[round(s, 4) for s in sigmas[0]]} m/s²")
    res.warnings.extend(summary["warnings"])
    return res
