#!/usr/bin/env python3
"""
imu_calibration.py -- calibrate the MPU's accelerometer (and the gyro's scale)
from one recording of the payload held still in known poses.

    python imu_calibration.py six_position.bin
    python imu_calibration.py six_position.bin --output calibration.json
    python imu_calibration.py turns.bin --gyro-turns 10

Accelerometer, six-position method
    Record the payload lying still for ~10 s in each of six poses: +X up, -X up,
    +Y up, -Y up, +Z up (standing as in the rocket), -Z up. The recording is
    split into still stretches, each stretch is assigned to the pose whose axis
    points up, and per axis

        offset = (up + down) / 2        scale = (up - down) / 2

    so that the corrected reading (raw - offset) / scale is +1 g pointing up and
    -1 g pointing down. With extra still poses (any orientation) an ellipsoid
    fit uses them all: minimise sum (|(a - offset) / scale| - 1)^2.

Gyroscope
    Every still stretch also measures the gyro's zero-rate offset; their spread
    shows how much it moves (it drifts with temperature, so the payload measures
    it again on the pad before every flight - see Documentation/CALIBRATION.md).
    For the scale factor, turn the payload a counted number of full turns about
    one axis between two still stretches (e.g. 10 turns against a stop on a
    table) and pass --gyro-turns: the integrated angle over N x 360 deg is the
    scale.

Units: accelerations in g, rates in deg/s, as frameparser converts them.
"""
import argparse
import contextlib
import io
import json
import math
import sys

import numpy as np

POSES = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]


def still_segments(t, a, g=None, window_s=1.0, min_duration_s=3.0, accel_rel_std=0.005,
                   gyro_std_dps=1.5, trim_s=0.3):
    """Index ranges [i0, i1) where the payload is still: over a sliding window
    |a| varies less than accel_rel_std and (if given) every gyro axis less than
    gyro_std_dps. Edges are trimmed by trim_s, where it was being put down."""
    t = np.asarray(t, dtype=np.float64)
    a = np.asarray(a, dtype=np.float64)
    n = len(t)
    if n < 10:
        return []
    amag = np.linalg.norm(a, axis=1)
    j0 = np.searchsorted(t, t - window_s / 2)
    j1 = np.searchsorted(t, t + window_s / 2)
    cnt = np.maximum(j1 - j0, 1)

    def wstd(x):
        c1 = np.concatenate([[0.0], np.cumsum(x)])
        c2 = np.concatenate([[0.0], np.cumsum(x * x)])
        m = (c1[j1] - c1[j0]) / cnt
        return np.sqrt(np.maximum((c2[j1] - c2[j0]) / cnt - m * m, 0.0))

    still = (wstd(amag) / max(float(np.median(amag)), 1e-9) < accel_rel_std) & (cnt >= 5)
    if g is not None:
        g = np.asarray(g, dtype=np.float64)
        for k in range(3):
            still &= wstd(g[:, k]) < gyro_std_dps
    segs = []
    i = 0
    while i < n:
        if not still[i]:
            i += 1
            continue
        k = i
        while k + 1 < n and still[k + 1] and t[k + 1] - t[k] < 1.0:
            k += 1
        lo = np.searchsorted(t, t[i] + trim_s)
        hi = np.searchsorted(t, t[k] - trim_s, side="right")
        if hi > lo and t[hi - 1] - t[lo] >= min_duration_s - 2 * trim_s:
            segs.append((int(lo), int(hi)))
        i = k + 1
    return segs


def pose_of(mean):
    """Which axis points up ("+X" ... "-Z") for a mean accelerometer reading,
    and how cleanly (cosine of the angle to that axis)."""
    k = int(np.argmax(np.abs(mean)))
    sign = "+" if mean[k] > 0 else "-"
    return sign + "XYZ"[k], float(abs(mean[k]) / max(np.linalg.norm(mean), 1e-12))


def fit_ellipsoid(means, offset0=None, scale0=None, iterations=50):
    """Offset and per-axis scale with |(m - offset) / scale| = 1 for every mean
    (Gauss-Newton). Needs at least six well-spread poses."""
    m = np.asarray(means, dtype=np.float64)
    b = np.zeros(3) if offset0 is None else np.array(offset0, dtype=np.float64)
    s = np.ones(3) if scale0 is None else np.array(scale0, dtype=np.float64)
    for _ in range(iterations):
        u = (m - b) / s
        r = np.linalg.norm(u, axis=1)
        res = r - 1.0
        # d r / d b_k = -u_k / (r s_k),  d r / d s_k = -u_k^2 / (r s_k)
        jb = -u / (r[:, None] * s[None, :])
        js = -(u * u) / (r[:, None] * s[None, :])
        J = np.hstack([jb, js])
        step, *_ = np.linalg.lstsq(J, -res, rcond=None)
        b += step[:3]
        s += step[3:]
        if np.max(np.abs(step)) < 1e-10:
            break
    u = (m - b) / s
    return b, s, np.linalg.norm(u, axis=1) - 1.0


def calibrate_accel(t, a, g=None, **still_kw):
    """Accelerometer offset/scale from still stretches. Returns a dict."""
    a = np.asarray(a, dtype=np.float64)
    segs = still_segments(t, a, g, **still_kw)
    poses = {}
    stretches = []
    for i0, i1 in segs:
        mean = a[i0:i1].mean(axis=0)
        pose, clean = pose_of(mean)
        entry = {"t_start": round(float(t[i0]), 2), "t_end": round(float(t[i1 - 1]), 2),
                 "samples": int(i1 - i0), "mean_g": [round(float(v), 5) for v in mean],
                 "pose": pose, "alignment": round(clean, 4)}
        if g is not None:
            entry["gyro_mean_dps"] = [round(float(v), 4) for v in np.asarray(g)[i0:i1].mean(axis=0)]
        stretches.append(entry)
        if clean > 0.98:                      # within ~11 deg of the axis
            poses.setdefault(pose, []).append((i1 - i0, mean))
    pose_means = {p: sum(w * m for w, m in v) / sum(w for w, _ in v) for p, v in poses.items()}
    warnings = []
    missing = [p for p in POSES if p not in pose_means]
    result = {"stretches": stretches, "poses_found": sorted(pose_means), "warnings": warnings}

    offset = np.zeros(3)
    scale = np.ones(3)
    method = None
    if not missing:
        for k, axis in enumerate("XYZ"):
            up, down = pose_means["+" + axis][k], pose_means["-" + axis][k]
            offset[k] = (up + down) / 2.0
            scale[k] = (up - down) / 2.0
        method = "six-position"
    elif len(stretches) >= 6:
        warnings.append(f"poses missing for the six-position method: {', '.join(missing)} - "
                        "using an ellipsoid fit over all still stretches instead")
        method = "ellipsoid"
    else:
        warnings.append(f"need the payload still in all six poses; missing: {', '.join(missing)}")
        result.update({"method": None})
        return result

    means = [np.array(s["mean_g"]) for s in stretches]
    if len(means) >= 6:
        fb, fs, fres = fit_ellipsoid(means, offset, scale)
        if method == "ellipsoid":
            offset, scale = fb, fs
        result["ellipsoid"] = {"offset_g": [round(float(v), 5) for v in fb],
                               "scale": [round(float(v), 5) for v in fs]}
    corrected = [(m - offset) / scale for m in means]
    resid = [float(np.linalg.norm(c) - 1.0) for c in corrected]
    for s, r in zip(stretches, resid):
        s["corrected_norm_error_mg"] = round(1000.0 * r, 2)
    result.update({
        "method": method,
        "offset_g": [round(float(v), 5) for v in offset],
        "scale": [round(float(v), 5) for v in scale],
        "residual_rms_mg": round(1000.0 * math.sqrt(float(np.mean(np.square(resid)))), 2),
    })
    if result["residual_rms_mg"] > 10:
        warnings.append(f"corrected stretches still miss 1 g by {result['residual_rms_mg']} mg rms - "
                        "the poses were not still or not level, or the axes are cross-coupled")
    for k, axis in enumerate("XYZ"):
        if abs(scale[k] - 1.0) > 0.06:
            warnings.append(f"{axis} scale {scale[k]:.3f} is outside the datasheet's +-3 % - check the data")
    if g is not None and stretches:
        gm = np.array([s["gyro_mean_dps"] for s in stretches])
        result["gyro_offset_dps"] = {"mean": [round(float(v), 4) for v in gm.mean(axis=0)],
                                     "spread": [round(float(v), 4) for v in gm.std(axis=0)]}
    return result


def gyro_turns(t, g, segs, turns):
    """Scale factor of the gyro axis turned between consecutive still stretches,
    from the integrated angle over turns x 360 deg."""
    t = np.asarray(t, dtype=np.float64)
    g = np.asarray(g, dtype=np.float64)
    out = []
    for (a0, a1), (b0, b1) in zip(segs, segs[1:]):
        bias = (g[a0:a1].mean(axis=0) + g[b0:b1].mean(axis=0)) / 2.0
        sl = slice(a1 - 1, b0 + 1)
        dt = np.diff(t[sl])
        angle = ((g[sl][:-1] - bias) * dt[:, None]).sum(axis=0)
        k = int(np.argmax(np.abs(angle)))
        if abs(angle[k]) < 180.0:
            continue                                   # no real rotation in between
        out.append({"axis": "XYZ"[k], "t_start": round(float(t[a1 - 1]), 2),
                    "t_end": round(float(t[b0]), 2), "angle_deg": round(float(angle[k]), 2),
                    "expected_deg": 360.0 * turns,
                    "scale": round(abs(float(angle[k])) / (360.0 * turns), 5)})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="recording: .bin or bin2json .json")
    ap.add_argument("--output", help="write the result as JSON here")
    ap.add_argument("--gyro-turns", type=float, help="full turns made between still stretches")
    ap.add_argument("--min-still", type=float, default=3.0, help="shortest still stretch, s (default 3)")
    args = ap.parse_args()

    import pilog_reader
    with contextlib.redirect_stdout(io.StringIO()):
        rec = pilog_reader.select_session(pilog_reader.load_recording(args.source))
    imu = rec.imu
    if len(imu.get("t", [])) < 100:
        sys.exit("[!] not enough IMU samples in this recording")
    t = imu["t"]
    a = np.column_stack([imu["ax"], imu["ay"], imu["az"]])
    g = np.column_stack([imu["gx"], imu["gy"], imu["gz"]])
    res = calibrate_accel(t, a, g, min_duration_s=args.min_still)
    if args.gyro_turns:
        res["gyro_turns"] = gyro_turns(t, g, still_segments(t, a, g, min_duration_s=args.min_still),
                                       args.gyro_turns)

    print(f"[+] {len(res['stretches'])} still stretches, poses {', '.join(res['poses_found']) or '-'}")
    for s in res["stretches"]:
        extra = f"  error after correction {s['corrected_norm_error_mg']:+.1f} mg" \
            if "corrected_norm_error_mg" in s else ""
        print(f"    {s['t_start']:8.2f}-{s['t_end']:8.2f} s  {s['pose']}  mean {s['mean_g']}{extra}")
    if res.get("method"):
        print(f"[+] {res['method']}: offset {res['offset_g']} g, scale {res['scale']}, "
              f"residual {res['residual_rms_mg']} mg rms")
    if "gyro_offset_dps" in res:
        print(f"[+] gyro zero-rate offset {res['gyro_offset_dps']['mean']} deg/s "
              f"(spread {res['gyro_offset_dps']['spread']})")
    for r in res.get("gyro_turns", []):
        print(f"[+] gyro {r['axis']}: {r['angle_deg']} deg for {r['expected_deg']} -> scale {r['scale']}")
    for w in res["warnings"]:
        print(f"[!] {w}")
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=2)
        print(f"[+] written to {args.output}")


if __name__ == "__main__":
    main()
