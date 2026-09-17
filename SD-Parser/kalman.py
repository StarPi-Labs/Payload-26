"""
kalman.py -- Linear Kalman filter and Rauch-Tung-Striebel smoother for the
Pi-LOG trajectory estimate.

Implements matlab/LKF.mlx and matlab/RTS smoother.mlx (notation of Särkkä,
"Bayesian Filtering and Smoothing", 2013):

    state      x = [x, vx, y, vy, z, vz]^T
    dynamics   x_k = A_k x_{k-1} + u_k + q_k,     q_k ~ N(0, Q_k)
    measure    y_k = H_k x_k + r_k,               r_k ~ N(0, R_k)

    A1 = [[1, dt], [0, 1]]      B1 = L1 = [dt^2/2, dt]^T    (per axis)
    A = blkdiag(A1, A1, A1),  B = L = blkdiag(B1, B1, B1)
    Q = L Qimu L^T            u = B [ax, ay, az]^T
    R = diag(sigma_gpsx^2, sigma_gpsy^2, sigma_baro^2)

    predict    m- = A m + u               P- = A P A^T + Q
    update     v = y - H m-               S = H P- H^T + R
               K = P- H^T S^-1            m = m- + K v
               P = P- - K S K^T           (then symmetrised)

    smooth     G_k  = P_k A_k^T [P-_{k+1}]^-1
               ms_k = m_k + G_k (ms_{k+1} - m-_{k+1})
               Ps_k = P_k + G_k (Ps_{k+1} - P-_{k+1}) G_k^T

Two deliberate differences from the Live Script, both about running it on real
logged data rather than a simulation:

1. dt comes from the timestamps, step by step. The script uses one fixed dt
   (and, as written, computes it as `dt = 1/Ts` while Parameters_Flight_Firmware.m
   defines `Ts = 1/fs = 0.001` -- that makes dt = 1000 s, not the 0.001 s its own
   comment states). The firmware samples at 100 Hz, not 1000 Hz, and logged
   frames jitter and occasionally drop, so per-step dt is both what the SDE
   derivation calls for (A_k = psi(t_{k+1}, t_k)) and what the data needs.

2. The update follows the script exactly, but the computation is split per
   axis. With diagonal Qimu, block-diagonal A/L/P0, diagonal R and one H row per
   axis, the covariance never couples the axes, so the 6-state filter is
   *exactly* three independent 2-state [p, v] filters. That turns 6x6 matrix
   algebra into scalar arithmetic -- necessary for a 20-hour recording of ~7
   million IMU samples. `lkf_rts_reference()` keeps the literal 6-state matrix
   form, and tests assert the two agree.

As in the script, the input u_k at step k uses the acceleration sampled at step
k, and Q = L Qimu L^T is the discrete white-acceleration model (Qimu is the
per-sample accelerometer variance), not the continuous-time integral form.
"""
from array import array
from dataclasses import dataclass, field

import numpy as np

# Initial covariance from LKF.mlx: P0 = diag([0.01, 0.001, 0.01, 0.001, 0.01, 0.001])
DEFAULT_P0 = (0.01, 0.001)


def _as_array(x):
    buf = array("d")
    buf.frombytes(np.ascontiguousarray(x, dtype=np.float64).tobytes())
    return buf


@dataclass
class AxisResult:
    """Forward and smoothed estimates for one [position, velocity] axis."""
    p_fwd: np.ndarray
    v_fwd: np.ndarray
    p: np.ndarray            # smoothed position
    v: np.ndarray            # smoothed velocity
    var_p: np.ndarray        # smoothed position variance
    var_v: np.ndarray        # smoothed velocity variance
    var_p_fwd: np.ndarray
    var_v_fwd: np.ndarray
    cov_pv_fwd: np.ndarray   # forward position-velocity covariance (to chain segments)
    innovations: np.ndarray  # innovation at each update step
    innovation_var: np.ndarray  # S at each update step
    update_steps: np.ndarray


def filter_smooth_axis(dt, acc, meas, ready, sigma_acc, sigma_meas,
                       p0=0.0, v0=0.0, P0=DEFAULT_P0, smooth=True):
    """Kalman filter + RTS smoother for one axis. Exact per-axis form of LKF/RTS.

    dt        (N,) seconds from the previous step to this one (dt[0] = the
              nominal step, used to predict from the prior m0 - as the script does)
    acc       (N,) acceleration input along this axis, m/s^2
    meas      (N,) position measurement at each step (ignored where not ready)
    ready     (N,) bool, a measurement is available at this step
    sigma_acc per-sample accelerometer noise std, m/s^2 (sqrt of Qimu diagonal)
    sigma_meas measurement noise std, m
    p0, v0    prior mean (the script's m0 = 0)
    P0        prior covariance as (var_p, var_v) or (var_p, cov_pv, var_v).
              Passing the previous segment's final forward state here continues
              the forward pass exactly, which is how long recordings are split.
    """
    dt = np.asarray(dt, dtype=np.float64)
    acc = np.asarray(acc, dtype=np.float64)
    meas = np.asarray(meas, dtype=np.float64)
    ready = np.asarray(ready, dtype=bool)
    n = len(dt)
    q = float(sigma_acc) ** 2
    r = float(sigma_meas) ** 2

    # Forward-pass storage. array('d') is compact (8 B/float) and fast to append.
    pf, vf = array("d"), array("d")
    af, bf, cf = array("d"), array("d"), array("d")          # P = [[a, b], [b, c]]
    pp, vp = array("d"), array("d")                          # predicted mean
    ap, bp, cp = array("d"), array("d"), array("d")          # predicted cov
    innov, innov_s, upd = array("d"), array("d"), array("q")

    p, v = float(p0), float(v0)
    if len(P0) == 3:
        a, b, c = float(P0[0]), float(P0[1]), float(P0[2])
    else:
        a, b, c = float(P0[0]), 0.0, float(P0[1])

    # Compact containers: indexing yields plain Python floats/ints (fast in the
    # loop) at 8 bytes per value, unlike .tolist() which costs ~32.
    dts = _as_array(dt)
    us = _as_array(acc)
    zs = _as_array(meas)
    rdy = bytearray(ready.astype(np.uint8).tobytes())

    for k in range(n):
        h = dts[k]
        u = us[k]
        h2 = h * h
        # ---- predict: m- = A m + B u,  P- = A P A^T + L Qimu L^T
        p_ = p + h * v + 0.5 * h2 * u
        v_ = v + h * u
        a_ = a + 2.0 * h * b + h2 * c + q * h2 * h2 * 0.25
        b_ = b + h * c + q * h2 * h * 0.5
        c_ = c + q * h2
        pp.append(p_); vp.append(v_)
        ap.append(a_); bp.append(b_); cp.append(c_)

        if rdy[k]:
            # ---- update with a position measurement, H = [1, 0]
            innovation = zs[k] - p_
            s = a_ + r
            k0 = a_ / s
            k1 = b_ / s
            p = p_ + k0 * innovation
            v = v_ + k1 * innovation
            # P = P- - K S K^T
            a = a_ - a_ * k0
            b = b_ - b_ * k0
            c = c_ - b_ * k1
            innov.append(innovation); innov_s.append(s); upd.append(k)
        else:
            p, v, a, b, c = p_, v_, a_, b_, c_

        pf.append(p); vf.append(v)
        af.append(a); bf.append(b); cf.append(c)

    p_fwd = np.frombuffer(pf, dtype=np.float64).copy()
    v_fwd = np.frombuffer(vf, dtype=np.float64).copy()
    var_p_fwd = np.frombuffer(af, dtype=np.float64).copy()
    var_v_fwd = np.frombuffer(cf, dtype=np.float64).copy()
    cov_pv_fwd = np.frombuffer(bf, dtype=np.float64).copy()

    if not smooth or n == 0:
        return AxisResult(p_fwd, v_fwd, p_fwd.copy(), v_fwd.copy(), var_p_fwd.copy(),
                          var_v_fwd.copy(), var_p_fwd, var_v_fwd, cov_pv_fwd,
                          np.frombuffer(innov, dtype=np.float64).copy(),
                          np.frombuffer(innov_s, dtype=np.float64).copy(),
                          np.frombuffer(upd, dtype=np.int64).copy())

    # ---- RTS backward pass --------------------------------------------------
    ps = array("d", bytes(8 * n))
    vs = array("d", bytes(8 * n))
    as_ = array("d", bytes(8 * n))
    cs_ = array("d", bytes(8 * n))
    ps[-1], vs[-1] = pf[-1], vf[-1]
    sa, sb, sc = af[-1], bf[-1], cf[-1]
    as_[-1], cs_[-1] = sa, sc

    for k in range(n - 2, -1, -1):
        h = dts[k + 1]                    # A_k maps step k -> k+1
        a, b, c = af[k], bf[k], cf[k]     # P_k (forward)
        e, f, g = ap[k + 1], bp[k + 1], cp[k + 1]   # P-_{k+1}
        det = e * g - f * f
        if det <= 0.0:
            # Degenerate prediction covariance (only possible with dt = 0 and
            # q = 0): nothing to propagate, keep the filtered estimate.
            ps[k], vs[k] = pf[k], vf[k]
            sa, sb, sc = a, b, c
            as_[k], cs_[k] = sa, sc
            continue
        # P_k A^T = [[a + b h, b], [b + c h, c]]
        m00 = a + b * h
        m10 = b + c * h
        inv = 1.0 / det
        # G = P_k A^T inv(P-_{k+1}),  inv = [[g, -f], [-f, e]] / det
        g00 = (m00 * g - b * f) * inv
        g01 = (b * e - m00 * f) * inv
        g10 = (m10 * g - c * f) * inv
        g11 = (c * e - m10 * f) * inv

        dp = ps[k + 1] - pp[k + 1]
        dv = vs[k + 1] - vp[k + 1]
        ps[k] = pf[k] + g00 * dp + g01 * dv
        vs[k] = vf[k] + g10 * dp + g11 * dv

        da = sa - e
        db = sb - f
        dc = sc - g
        # X = G dP G^T
        x00 = g00 * da + g01 * db
        x01 = g00 * db + g01 * dc
        x10 = g10 * da + g11 * db
        x11 = g10 * db + g11 * dc
        sa = a + x00 * g00 + x01 * g01
        sb = b + x00 * g10 + x01 * g11
        sc = c + x10 * g10 + x11 * g11
        as_[k], cs_[k] = sa, sc

    return AxisResult(
        p_fwd=p_fwd, v_fwd=v_fwd,
        p=np.frombuffer(ps, dtype=np.float64).copy(),
        v=np.frombuffer(vs, dtype=np.float64).copy(),
        var_p=np.frombuffer(as_, dtype=np.float64).copy(),
        var_v=np.frombuffer(cs_, dtype=np.float64).copy(),
        var_p_fwd=var_p_fwd, var_v_fwd=var_v_fwd, cov_pv_fwd=cov_pv_fwd,
        innovations=np.frombuffer(innov, dtype=np.float64).copy(),
        innovation_var=np.frombuffer(innov_s, dtype=np.float64).copy(),
        update_steps=np.frombuffer(upd, dtype=np.int64).copy(),
    )


@dataclass
class TrajectoryResult:
    x: AxisResult
    y: AxisResult
    z: AxisResult


def lkf_rts(dt, acc_xyz, gps_xy, gps_ready, baro_z, baro_ready,
            sigma_acc=(0.05, 0.05, 0.05), sigma_gps=(2.5, 2.5), sigma_baro=1.0,
            P0=DEFAULT_P0, smooth=True):
    """Full LKF + RTS on the 6-state model, computed per axis (exact).

    acc_xyz   (N, 3) acceleration in the navigation frame, gravity removed, m/s^2
    gps_xy    (N, 2) GPS east/north position relative to the origin, m
    gps_ready (N,)   bool
    baro_z    (N,)   barometric altitude above the origin, m
    baro_ready(N,)   bool
    sigma_acc (3,)   sqrt of the diagonal of Qimu
    sigma_gps (2,)   devGpsx, devGpsy       (LKF.mlx defaults: 2.5, 2.5)
    sigma_baro       devbaro                (LKF.mlx default:  1.0)
    """
    acc_xyz = np.asarray(acc_xyz, dtype=np.float64)
    gps_xy = np.asarray(gps_xy, dtype=np.float64)
    sx, sy, sz = (float(s) for s in np.broadcast_to(sigma_acc, (3,)))
    gx, gy = (float(s) for s in np.broadcast_to(sigma_gps, (2,)))
    return TrajectoryResult(
        x=filter_smooth_axis(dt, acc_xyz[:, 0], gps_xy[:, 0], gps_ready, sx, gx, P0=P0, smooth=smooth),
        y=filter_smooth_axis(dt, acc_xyz[:, 1], gps_xy[:, 1], gps_ready, sy, gy, P0=P0, smooth=smooth),
        z=filter_smooth_axis(dt, acc_xyz[:, 2], baro_z, baro_ready, sz, float(sigma_baro), P0=P0, smooth=smooth),
    )


def lkf_rts_reference(dt, acc_xyz, gps_xy, gps_ready, baro_z, baro_ready,
                      Qimu, sigma_gps=(2.5, 2.5), sigma_baro=1.0,
                      P0=(0.01, 0.001, 0.01, 0.001, 0.01, 0.001)):
    """Literal 6-state matrix transcription of LKF.mlx + RTS smoother.mlx.

    Slow (6x6 algebra per step) and memory-hungry (stores every covariance), so
    only for verification and for a non-diagonal Qimu, where the per-axis
    decomposition would not be exact. Returns (mfwd, Pfwd, smoothX, smoothP).
    """
    dt = np.asarray(dt, dtype=np.float64)
    acc_xyz = np.asarray(acc_xyz, dtype=np.float64)
    gps_xy = np.asarray(gps_xy, dtype=np.float64)
    Qimu = np.asarray(Qimu, dtype=np.float64)
    n = len(dt)
    RGpsx, RGpsy = sigma_gps[0] ** 2, sigma_gps[1] ** 2
    Rbaro = sigma_baro ** 2

    def blk(dtk):
        A1 = np.array([[1.0, dtk], [0.0, 1.0]])
        B1 = np.array([[0.5 * dtk ** 2], [dtk]])
        A = np.zeros((6, 6)); L = np.zeros((6, 3))
        for i in range(3):
            A[2 * i:2 * i + 2, 2 * i:2 * i + 2] = A1
            L[2 * i:2 * i + 2, i:i + 1] = B1
        return A, L   # B == L in the script

    mfwd = np.zeros((6, n)); Pfwd = np.zeros((6, 6, n))
    mpred = np.zeros((6, n)); Ppred = np.zeros((6, 6, n))
    As = np.zeros((6, 6, n))
    m = np.zeros(6)
    P = np.diag(P0).astype(np.float64)

    for k in range(n):
        A, L = blk(dt[k])
        As[:, :, k] = A
        Q = L @ Qimu @ L.T
        uk = L @ acc_xyz[k]
        mpred[:, k] = A @ m + uk
        Ppred[:, :, k] = A @ P @ A.T + Q

        y, H, Rd = [], [], []
        if gps_ready[k]:
            y += [gps_xy[k, 0], gps_xy[k, 1]]
            H += [[1, 0, 0, 0, 0, 0], [0, 0, 1, 0, 0, 0]]
            Rd += [RGpsx, RGpsy]
        if baro_ready[k]:
            y += [baro_z[k]]
            H += [[0, 0, 0, 0, 1, 0]]
            Rd += [Rbaro]

        if y:
            y = np.array(y); H = np.array(H, dtype=np.float64); R = np.diag(Rd)
            v = y - H @ mpred[:, k]
            S = H @ Ppred[:, :, k] @ H.T + R
            K = np.linalg.solve(S.T, (Ppred[:, :, k] @ H.T).T).T   # Ppred H' / S
            m = mpred[:, k] + K @ v
            P = Ppred[:, :, k] - K @ S @ K.T
            P = (P + P.T) / 2
        else:
            m = mpred[:, k].copy()
            P = Ppred[:, :, k].copy()
        mfwd[:, k] = m
        Pfwd[:, :, k] = P

    smoothX = np.zeros((6, n)); smoothP = np.zeros((6, 6, n))
    smoothX[:, -1] = mfwd[:, -1]
    smoothP[:, :, -1] = Pfwd[:, :, -1]
    for k in range(n - 2, -1, -1):
        A = As[:, :, k + 1]
        G = np.linalg.solve(Ppred[:, :, k + 1].T, (Pfwd[:, :, k] @ A.T).T).T
        smoothX[:, k] = mfwd[:, k] + G @ (smoothX[:, k + 1] - mpred[:, k + 1])
        smoothP[:, :, k] = Pfwd[:, :, k] + G @ (smoothP[:, :, k + 1] - Ppred[:, :, k + 1]) @ G.T
    return mfwd, Pfwd, smoothX, smoothP
