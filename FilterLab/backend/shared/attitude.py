"""
attitude.py -- payload orientation and motion over a recording, for the
Dashboard's rocket view.

The telemetry carries no orientation: its 'roll'/'pitch'/'yaw' columns are
gyroscope RATES in deg/s about the body X/Y/Z axes (frameparser.py names
them that way). This module integrates them into one orientation quaternion
per row, keeps the tilt honest with gravity whenever the payload is provably
still, and rotates the measured acceleration into a fixed frame so the view
can show the rocket being pushed around by it.

Frames
    body     payload CAD frame: +X away from the launch rail (towards someone
             looking at the front), +Y right as seen from the front, +Z nose -
             what the firmware logs since CONFIG_IMU_REMAP_TO_CAD. Older
             recordings are in another frame and are not supported.
    pad      X towards the viewer, Y to the viewer's right, Z up. Heading is
             unobservable without a magnetometer, so this frame is fixed by
             the payload's own pose while it waits before launch: its front
             (+X) faces the viewer. A rocket standing on the pad is therefore
             shown in exactly the CAD front view, tilted by whatever the rail is.

Algorithm
    * Anchor: the stillest stretch before launch gives the gravity direction
      (the tilt at that moment), the gyro bias, the noise floor of both sensors
      and the magnitude the accelerometer reads for 1 g (subtracting that, not
      9.81, cancels its static scale error).
    * From the anchor the gyro is integrated forward to the end and backward to
      the start - a recording usually begins while the payload is still being
      carried or set down, so the anchor's orientation must not be assumed at
      the first row. Each step holds the logged rate until the next row (the
      zero-order hold the logger's forward-fill implies) and rotates exactly
      (rotation vector -> quaternion).
    * Still = |a| close to the rest value AND a steady gyro: short-term spread
      within a few times the noise floor. That is independent of the bias
      estimate, which drifts by several deg/s while the MPU warms up, and of
      how noisy a particular unit or bench is.
    * Gravity correction (Mahony-style, proportional only) runs while still
      for MIN_STILL_S. Once something accelerates the payload hard (BOOST, or
      any jolt above HIGH_G), nothing the accelerometer reads is gravity -
      thrust, drag, free fall, parachute swing - so correction stays off for
      at least MIN_FLIGHT_S and until the payload has been still for
      STILL_AFTER_FLIGHT_S again (landed, or back on the bench). Drag alone
      can read close to 1 g during coast, but only for a fraction of a second
      while the rocket decelerates through that speed.
    * The gyro bias follows the gyro slowly while still for BIAS_MIN_STILL_S,
      which keeps multi-hour bench recordings from drifting in heading.
    * Data gaps: in ARMED the logger keeps only a 0.4 s burst every 10 s, so
      whatever turned the payload in between is not in the data. After a gap
      longer than GAP_SNAP_S the tilt is snapped to the gravity measured by
      the next sample (heading kept - nothing observes it), instead of waiting
      for the slow correction across many short bursts.
    * Motion: the specific force rotated into the pad frame minus the rest
      gravity is the payload's own acceleration (zero at rest, up in boost,
      down in coast and free fall), lightly smoothed.

Output columns (one value per row):
    attitudeW/X/Y/Z   quaternion rotating body vectors into the pad frame
    padAccelX/Y/Z     acceleration in the pad frame, gravity removed, in the
                      unit the input accelerations were given in

Pure Python in the inner loop on purpose (no numba/scipy in the image): a
20-hour, 9-million-row recording takes about a minute, once, while its column
cache is built.
"""
import math

import numpy as np

D2R = math.pi / 180.0

ACC_TOL = 0.05              # still: |a| within 5 % of the rest magnitude (at least)...
GYRO_STEADY_DPS = 0.5       # ...and the gyro's short-term spread below this, or
STEADY_NOISE_FACTOR = 2.0   # ...this many times the rest window's own spread...
GYRO_STEADY_TAU_S = 0.5     # ...measured over about this long...
MIN_STILL_S = 0.25          # ...for at least this long
HIGH_G = 2.0                # |a| above this many rest magnitudes = flight-like
MIN_FLIGHT_S = 10.0         # a flight-like phase lasts at least this long...
STILL_AFTER_FLIGHT_S = 5.0  # ...and ends after being still this long
KP = 0.5                    # gravity feedback gain [1/s], ~2 s time constant
BIAS_TAU_S = 60.0           # bias tracking time constant
BIAS_MIN_STILL_S = 3.0      # only learn the bias once settled...
BIAS_LEARN_MAX_DPS = 3.0    # ...and never from a rate this far from it: a steady
                            # spin is just as "steady" as rest, but it is no bias
REST_MAX_RATE_DPS = 15.0    # a rest window's mean rate is a bias (ZRO is +-5 dps)
ACCEL_SMOOTH_S = 0.08       # time constant of the pad-frame acceleration
DT_MAX = 0.1                # never hold one logged rate for longer than this
GAP_SNAP_S = 1.0            # after a gap this long, snap the tilt to gravity...
GAP_SNAP_TOL = 0.15         # ...if |a| is within 15 % of the rest value
REST_WINDOW_S = 2.0         # length of the stillest-stretch search window
REST_SEARCH_S = 600.0       # search at most this far into the recording
MIN_REST_SAMPLES = 10
CHUNK_ROWS = 262144

ATTITUDE_COLUMNS = ("attitudeW", "attitudeX", "attitudeY", "attitudeZ")
PAD_ACCEL_COLUMNS = ("padAccelX", "padAccelY", "padAccelZ")
OUTPUT_COLUMNS = ATTITUDE_COLUMNS + PAD_ACCEL_COLUMNS


def _window_var(values, s, e):
    """Mean and variance of values[s:e] for many windows at once, via
    cumulative sums."""
    v = values.astype(np.float64)
    c1 = np.concatenate([[0.0], np.cumsum(v)])
    c2 = np.concatenate([[0.0], np.cumsum(v * v)])
    c = (e - s).astype(np.float64)
    mean = (c1[e] - c1[s]) / c
    return mean, np.maximum((c2[e] - c2[s]) / c - mean * mean, 0.0)


def _rest_window(t, amag, gyros, valid, t_limit):
    """Row indices of the REST_WINDOW_S stretch to take the rest values from,
    among valid rows before t_limit, and whether it is still.

    Of the windows where |a| is steady and the mean rate could be a bias, the
    one with the steadiest gyro wins (the gyro bias comes from it); a steady
    spin is excluded by REST_MAX_RATE_DPS. If |a| is never steady, the window
    where it is steadiest. Sampling may be irregular (forward-filled rows,
    ARMED-mode bursts), so windows are defined in time, not in rows."""
    idx = np.nonzero(valid & (t <= t_limit))[0]
    if len(idx) < MIN_REST_SAMPLES:
        return np.nonzero(valid)[0][:MIN_REST_SAMPLES], False
    tv = t[idx]
    start = np.arange(len(tv))
    end = np.searchsorted(tv, tv + REST_WINDOW_S, side="left")
    cnt = end - start
    # Only complete windows, with a typical number of samples: near the end of
    # the search range windows are truncated, and a window of a dozen samples
    # beats a full one on spread by chance alone.
    ok = (tv + REST_WINDOW_S <= tv[-1]) & (cnt >= MIN_REST_SAMPLES)
    if np.any(ok):
        ok &= cnt >= 0.5 * np.median(cnt[ok])
    if not np.any(ok):
        return idx, False
    s = start[ok]
    e = end[ok]
    mean, var = _window_var(amag[idx], s, e)
    rel = np.sqrt(var) / np.maximum(mean, 1e-12)
    stats = [_window_var(np.asarray(g)[idx], s, e) for g in gyros]
    rate2 = sum(m * m for m, _ in stats)
    gyro_var = sum(v for _, v in stats)
    still = (rel < 0.02) & (rate2 < REST_MAX_RATE_DPS ** 2)
    if np.any(still):
        best = int(np.argmin(np.where(still, gyro_var, np.inf)))
    else:
        best = int(np.argmin(rel + np.where(rate2 < REST_MAX_RATE_DPS ** 2, 0.0, 1.0)))
    return idx[s[best]:e[best]], bool(still[best])


def _matrix_to_quat(m):
    """Rotation matrix -> unit quaternion (w, x, y, z), Shepperd's method."""
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        q = (0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s)
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0
        q = ((m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s)
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0
        q = ((m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s)
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0
        q = ((m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s)
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q)


def initial_orientation(f_rest):
    """Body -> pad quaternion from the specific force measured at rest.

    At rest the accelerometer reads the reaction to gravity, so f_rest points
    up. The pad X axis (towards the viewer) is the payload's front, +X,
    levelled; if the payload lies on its back or front (+X near vertical) the
    nose is laid out to the viewer's right instead.
    """
    up = np.asarray(f_rest, dtype=np.float64)
    up = up / np.linalg.norm(up)
    if abs(up[0]) < 0.9:
        ex = np.array([1.0, 0.0, 0.0]) - up[0] * up       # front, levelled
        ex /= np.linalg.norm(ex)
        ey = np.cross(up, ex)
    else:
        ey = np.array([0.0, 0.0, 1.0]) - up[2] * up       # nose, levelled
        ey /= np.linalg.norm(ey)
        ex = np.cross(ey, up)
    # Rows are the pad axes expressed in body coordinates: v_pad = R v_body.
    return _matrix_to_quat([ex.tolist(), ey.tolist(), up.tolist()])


def _chunks(first, last, step):
    """Row ranges [lo, hi) covering first .. last (exclusive) in processing
    order, CHUNK_ROWS at a time."""
    if step > 0:
        for lo in range(first, last, CHUNK_ROWS):
            yield lo, min(last, lo + CHUNK_ROWS)
    else:
        hi = first + 1
        while hi > last + 1:
            lo = max(last + 1, hi - CHUNK_ROWS)
            yield lo, hi
            hi = lo


def _propagate(t, fresh, data, k_start, k_stop, step, q0, bias, cfg, quat_out, report):
    """Integrate the orientation from row k_start (a fresh IMU sample, which
    gets q0) towards k_stop, exclusive: step = +1 forward, -1 backward.

    fresh: rows that carry a new IMU sample. Telemetry rows are forward-filled
    (every sensor's frame repeats the last IMU values), so only fresh rows are
    integrated, over the time since the previous fresh one; the other rows
    repeat the current orientation. That also keeps a stopped IMU from
    integrating its last rate forever.
    data: the six CAD-frame input arrays (ax, ay, az, gx, gy, gz).
    Each interval holds the EARLIER sample's values (zero-order hold), so a
    backward step uses the current sample and the inverse rotation; the
    gravity correction always pulls towards the measurement.
    Returns (final bias, statistics dict).
    """
    w, x, y, z = q0
    bx, by, bz = bias
    hi2 = cfg["hi2"]
    lo_still2, hi_still2 = cfg["lo_still2"], cfg["hi_still2"]
    snap_lo2, snap_hi2 = cfg["snap_lo2"], cfg["snap_hi2"]
    steady_var = cfg["steady_var"]
    bias_learn2 = BIAS_LEARN_MAX_DPS ** 2
    fs = cfg["flight_start"] if step > 0 else math.inf
    fs_pending = fs != math.inf
    mgx, mgy, mgz = cfg["gyro_rest"]
    vg = steady_var
    flight = False
    flight_left = 0.0
    still_t = 0.0
    stats = {"flightEvents": 0, "correctedRows": 0, "biasUpdates": 0, "gapSnaps": 0}
    sqrt, sin, cos = math.sqrt, math.sin, math.cos
    qw_o, qx_o, qy_o, qz_o = quat_out
    sign = 1.0 if step > 0 else -1.0

    qw_o[k_start], qx_o[k_start], qy_o[k_start], qz_o[k_start] = w, x, y, z
    first = k_start + step
    if (step > 0 and first >= k_stop) or (step < 0 and first <= k_stop):
        return [bx, by, bz], stats

    prev_t = float(t[k_start])
    prev_vals = [float(v[k_start]) for v in data]
    total = abs(k_stop - first)
    done = 0

    for lo, hi in _chunks(first, k_stop, step):
        tl = t[lo:hi].tolist()
        axl, ayl, azl, gxl, gyl, gzl = (np.asarray(v[lo:hi], np.float64).tolist() for v in data)
        fl = fresh[lo:hi].tolist()
        rw, rx_, ry_, rz_ = [], [], [], []
        aw, axo, ayo, azo = rw.append, rx_.append, ry_.append, rz_.append

        for i in (range(hi - lo) if step > 0 else range(hi - lo - 1, -1, -1)):
            if not fl[i]:
                aw(w)
                axo(x)
                ayo(y)
                azo(z)
                continue
            tk = tl[i]
            # the interval's held values belong to the earlier of the two samples
            if step > 0:
                pax, pay, paz, pgx, pgy, pgz = prev_vals
                gap = tk - prev_t
                t_held = prev_t
            else:
                pax, pay, paz = axl[i], ayl[i], azl[i]
                pgx, pgy, pgz = gxl[i], gyl[i], gzl[i]
                gap = prev_t - tk
                t_held = tk
            dt = gap if gap < DT_MAX else DT_MAX
            if dt > 0.0:
                # gyro steadiness, independent of the bias estimate
                al = dt / GYRO_STEADY_TAU_S
                if al > 1.0:
                    al = 1.0
                dmx = pgx - mgx
                dmy = pgy - mgy
                dmz = pgz - mgz
                mgx += al * dmx
                mgy += al * dmy
                mgz += al * dmz
                vg += al * (dmx * dmx + dmy * dmy + dmz * dmz - vg)

                a2 = pax * pax + pay * pay + paz * paz
                launched = fs_pending and t_held >= fs
                if launched or a2 > hi2:
                    if launched:
                        fs_pending = False
                    if not flight:
                        stats["flightEvents"] += 1
                    flight = True
                    flight_left = MIN_FLIGHT_S
                if lo_still2 < a2 < hi_still2 and vg < steady_var:
                    still_t += dt
                else:
                    still_t = 0.0
                if flight:
                    flight_left -= dt
                    if flight_left <= 0.0 and still_t >= STILL_AFTER_FLIGHT_S:
                        flight = False                  # landed / back on the bench

                rx = pgx - bx
                ry = pgy - by
                rz = pgz - bz
                wx = sign * rx * D2R
                wy = sign * ry * D2R
                wz = sign * rz * D2R
                if not flight and still_t >= MIN_STILL_S:
                    inv = 1.0 / sqrt(a2)
                    mx = pax * inv
                    my = pay * inv
                    mz = paz * inv
                    # estimated "up" in body coordinates: third row of R(q)
                    vx = 2.0 * (x * z - w * y)
                    vy = 2.0 * (y * z + w * x)
                    vz = w * w - x * x - y * y + z * z
                    wx += KP * (my * vz - mz * vy)
                    wy += KP * (mz * vx - mx * vz)
                    wz += KP * (mx * vy - my * vx)
                    stats["correctedRows"] += 1
                    if still_t >= BIAS_MIN_STILL_S and rx * rx + ry * ry + rz * rz < bias_learn2:
                        k = dt / BIAS_TAU_S
                        bx += k * rx
                        by += k * ry
                        bz += k * rz
                        stats["biasUpdates"] += 1
                # q = q * exp(omega dt / 2)
                hx = 0.5 * wx * dt
                hy = 0.5 * wy * dt
                hz = 0.5 * wz * dt
                th2 = hx * hx + hy * hy + hz * hz
                if th2 > 1e-24:
                    th = sqrt(th2)
                    s = sin(th) / th
                    dw = cos(th)
                    dx = hx * s
                    dy = hy * s
                    dz = hz * s
                    w, x, y, z = (w * dw - x * dx - y * dy - z * dz,
                                  w * dx + x * dw + y * dz - z * dy,
                                  w * dy - x * dz + y * dw + z * dx,
                                  w * dz + x * dy - y * dx + z * dw)
                    nn = 1.0 / sqrt(w * w + x * x + y * y + z * z)
                    w *= nn
                    x *= nn
                    y *= nn
                    z *= nn
            if gap > GAP_SNAP_S and not flight:
                # After a gap the orientation is unknown: take the tilt from this
                # sample's gravity and keep the heading - the shortest rotation
                # q *= d, d turning the measured up m onto the estimated up v.
                cax, cay, caz = axl[i], ayl[i], azl[i]
                c2 = cax * cax + cay * cay + caz * caz
                if snap_lo2 < c2 < snap_hi2:
                    inv = 1.0 / sqrt(c2)
                    mx = cax * inv
                    my = cay * inv
                    mz = caz * inv
                    vx = 2.0 * (x * z - w * y)
                    vy = 2.0 * (y * z + w * x)
                    vz = w * w - x * x - y * y + z * z
                    d = mx * vx + my * vy + mz * vz
                    if d > -0.999:
                        dw, dx, dy, dz = 1.0 + d, my * vz - mz * vy, mz * vx - mx * vz, mx * vy - my * vx
                    elif mx * mx + my * my > 1e-6:     # upside down: any perpendicular axis
                        dw, dx, dy, dz = 0.0, -my, mx, 0.0
                    else:
                        dw, dx, dy, dz = 0.0, 1.0, 0.0, 0.0
                    nn = 1.0 / sqrt(dw * dw + dx * dx + dy * dy + dz * dz)
                    dw *= nn
                    dx *= nn
                    dy *= nn
                    dz *= nn
                    w, x, y, z = (w * dw - x * dx - y * dy - z * dz,
                                  w * dx + x * dw + y * dz - z * dy,
                                  w * dy - x * dz + y * dw + z * dx,
                                  w * dz + x * dy - y * dx + z * dw)
                    mgx, mgy, mgz = gxl[i], gyl[i], gzl[i]
                    vg = steady_var
                    still_t = 0.0
                    stats["gapSnaps"] += 1
            aw(w)
            axo(x)
            ayo(y)
            azo(z)
            prev_t = tk
            prev_vals = [axl[i], ayl[i], azl[i], gxl[i], gyl[i], gzl[i]]

        if step < 0:
            for r in (rw, rx_, ry_, rz_):
                r.reverse()
        qw_o[lo:hi] = rw
        qx_o[lo:hi] = rx_
        qy_o[lo:hi] = ry_
        qz_o[lo:hi] = rz_
        done += hi - lo
        report(done / max(total, 1))
    return [bx, by, bz], stats


def _pad_acceleration(t, valid, data, quats, g_rest, outs):
    """Specific force rotated into the pad frame, rest gravity removed, then
    exponentially smoothed with time constant ACCEL_SMOOTH_S."""
    n = len(t)
    pnx = pny = pnz = 0.0
    have = False
    prev_t = 0.0
    ox, oy, oz = outs
    for lo in range(0, n, CHUNK_ROWS):
        hi = min(n, lo + CHUNK_ROWS)
        fx, fy, fz = (np.asarray(v[lo:hi], np.float64) for v in data[:3])
        w, x, y, z = (np.asarray(q[lo:hi], np.float64) for q in quats)
        # v' = v + 2w (u x v) + 2 u x (u x v)
        cx = y * fz - z * fy
        cy = z * fx - x * fz
        cz = x * fy - y * fx
        ex = (fx + 2.0 * (w * cx + y * cz - z * cy)).tolist()
        ey = (fy + 2.0 * (w * cy + z * cx - x * cz)).tolist()
        ez = (fz + 2.0 * (w * cz + x * cy - y * cx) - g_rest).tolist()
        tl = t[lo:hi].tolist()
        vl = valid[lo:hi].tolist()
        rx, ry, rz = [], [], []
        for i in range(hi - lo):
            if vl[i]:
                if have:
                    be = (tl[i] - prev_t) / ACCEL_SMOOTH_S
                    if be > 1.0:
                        be = 1.0
                    pnx += be * (ex[i] - pnx)
                    pny += be * (ey[i] - pny)
                    pnz += be * (ez[i] - pnz)
                else:
                    pnx, pny, pnz = ex[i], ey[i], ez[i]
                    have = True
                prev_t = tl[i]
                rx.append(pnx)
                ry.append(pny)
                rz.append(pnz)
            else:
                rx.append(0.0)
                ry.append(0.0)
                rz.append(0.0)
        ox[lo:hi] = rx
        oy[lo:hi] = ry
        oz[lo:hi] = rz


def estimate(t, ax, ay, az, gx, gy, gz, flight_start=None, progress=None):
    """Orientation and pad-frame acceleration for every row of a recording.

    t             (N,) seconds, ascending
    ax, ay, az    (N,) specific force in the CAD frame; the unit carries
                  through to padAccel*
    gx, gy, gz    (N,) body rates in the CAD frame, deg/s
    flight_start  time of the first BOOST, if known (any |a| > HIGH_G jolt
                  switches gravity correction off as well)
    progress      optional callable(fraction)

    Returns ({column name: float32 array} for OUTPUT_COLUMNS, info dict).
    Rows without IMU data (all-zero accelerations, before the first sample)
    hold the nearest orientation and get zero acceleration.
    """
    t = np.asarray(t, dtype=np.float64)
    n = len(t)
    cols = {name: np.zeros(n, dtype=np.float32) for name in OUTPUT_COLUMNS}
    info = {"version": 4, "rows": int(n)}
    if n == 0:
        info["status"] = "no rows"
        return cols, info

    data = [np.asarray(v, np.float64) for v in (ax, ay, az, gx, gy, gz)]
    amag = np.sqrt(data[0] ** 2 + data[1] ** 2 + data[2] ** 2)
    valid = np.isfinite(amag) & (amag > 1e-6)
    for g in data[3:]:
        valid &= np.isfinite(g)
    # A fresh IMU sample: any of the six values differs from the previous row.
    changed = np.zeros(n, dtype=bool)
    changed[0] = True
    for v in data:
        changed[1:] |= v[1:] != v[:-1]
    fresh = valid & changed
    if not np.any(fresh):
        cols["attitudeW"].fill(1.0)
        info["status"] = "no IMU samples"
        return cols, info

    # ---- anchor: the stillest stretch before launch ----------------------------
    first_valid = int(np.argmax(valid))
    t_limit = t[first_valid] + REST_SEARCH_S
    if flight_start is not None:
        t_limit = min(t_limit, flight_start - 0.5)
    rest_idx, rest_still = _rest_window(t, amag, data[3:], valid, t_limit)
    acc_rest = np.array([float(np.mean(v[rest_idx])) for v in data[:3]])
    g_rest = float(np.linalg.norm(acc_rest))
    gyro_rest = [float(np.mean(v[rest_idx])) for v in data[3:]]
    # Moving throughout: a mean would be motion, not bias.
    bias = gyro_rest if rest_still else [0.0, 0.0, 0.0]
    gyro_noise_var = float(sum(np.var(v[rest_idx]) for v in data[3:])) if rest_still else 0.0
    acc_noise = float(np.std(amag[rest_idx]) / g_rest) if rest_still else 0.0
    steady_var = max(GYRO_STEADY_DPS ** 2, STEADY_NOISE_FACTOR ** 2 * gyro_noise_var)
    acc_tol = max(ACC_TOL, 4.0 * acc_noise)
    q0 = initial_orientation(acc_rest)
    in_rest = rest_idx[fresh[rest_idx]]
    k0 = int(in_rest[0]) if len(in_rest) else int(np.argmax(fresh))
    info.update({
        "restWindow": [round(float(t[rest_idx[0]]), 3), round(float(t[rest_idx[-1]]), 3)],
        "restStill": rest_still,
        "restAccel": [round(float(v), 5) for v in acc_rest],
        "gyroBiasStartDps": [round(b, 4) for b in bias],
        "gyroNoiseDps": round(math.sqrt(gyro_noise_var), 3),
        "stillThresholds": {"gyroSpreadDps": round(math.sqrt(steady_var), 3),
                            "accelTol": round(acc_tol, 4)},
    })

    cfg = {
        "hi2": (HIGH_G * g_rest) ** 2,
        "lo_still2": ((1.0 - acc_tol) * g_rest) ** 2,
        "hi_still2": ((1.0 + acc_tol) * g_rest) ** 2,
        "steady_var": steady_var,
        "flight_start": math.inf if flight_start is None else float(flight_start),
        "gyro_rest": gyro_rest,
        "snap_lo2": ((1.0 - GAP_SNAP_TOL) * g_rest) ** 2,
        "snap_hi2": ((1.0 + GAP_SNAP_TOL) * g_rest) ** 2,
    }
    quats = [cols[c] for c in ATTITUDE_COLUMNS]
    fwd_share = (n - k0) / n

    def report(offset, span):
        def f(frac):
            if progress is not None:
                progress(0.85 * (offset + span * frac))
        return f

    bias_end, fwd = _propagate(t, fresh, data, k0, n, +1, q0, bias, cfg, quats,
                               report(0.0, fwd_share))
    _, bwd = _propagate(t, fresh, data, k0, -1, -1, q0, bias, cfg, quats,
                        report(fwd_share, 1.0 - fwd_share))
    _pad_acceleration(t, valid, data, quats, g_rest, [cols[c] for c in PAD_ACCEL_COLUMNS])
    if progress is not None:
        progress(1.0)

    info.update({
        "gRest": round(g_rest, 5),
        "gyroBiasEndDps": [round(b, 4) for b in bias_end],
        "flightEvents": fwd["flightEvents"] + bwd["flightEvents"],
        "correctedRows": fwd["correctedRows"] + bwd["correctedRows"],
        "biasUpdates": fwd["biasUpdates"] + bwd["biasUpdates"],
        "gapSnaps": fwd["gapSnaps"] + bwd["gapSnaps"],
        "status": "ok",
    })
    return cols, info
