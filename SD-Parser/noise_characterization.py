#!/usr/bin/env python3
"""
noise_characterization.py -- measure the sensor noise parameters the Kalman
filter needs, from a long STATIONARY recording.

    python noise_characterization.py 20hourTest.bin --plot
    python trajectory.py flight.bin --params 20hourTest_noise.json

LKF.mlx says the process noise Q "is derived directly from the noise
characteristics of the IMU", and that R holds the variances of the GPS and the
barometer -- but it never says where those numbers come from (Qimu is not
defined anywhere in the scripts, and devGps/devbaro are placeholders). A long
recording of the payload sitting still is exactly the experiment that measures
them. This script reports:

  IMU        per-sample white noise (Qimu), Allan deviation, velocity/angle
             random walk, bias instability, gyro bias, and how the accelerometer
             bias drifts with temperature
  barometer  white noise of the altitude estimate, and the slow weather drift
  GPS        position scatter, CEP50/CEP95, and how long its errors stay
             correlated (the filter assumes they do not)
  data       sample rates, dropped samples, gaps, power draw

and writes `recommended` values that trajectory.py --params reads.

White noise is measured from differences of consecutive samples,
std(x[k+1] - x[k]) / sqrt(2): slow drift (weather, temperature) cancels in the
difference, leaving the sample-to-sample noise the filter models.
"""
import argparse
import io
import json
import math
import os
from contextlib import redirect_stdout

import numpy as np

import pilog_reader
import trajectory as tr

G0 = tr.G0


# ---------------------------------------------------------------------------
# Estimators
# ---------------------------------------------------------------------------

def white_sigma(t, x, nominal):
    """Sample-to-sample noise from consecutive pairs only (gaps skipped)."""
    dt = np.diff(t)
    ok = (dt > 0.5 * nominal) & (dt < 1.5 * nominal)
    d = np.diff(x)[ok]
    return float(np.std(d) / math.sqrt(2.0)) if d.size > 10 else float("nan")


def allan_deviation(x, fs, n_taus=48, max_fraction=0.1):
    """Overlapping Allan deviation of a uniformly sampled rate signal.

    x    samples of e.g. acceleration (m/s^2) or angular rate (deg/s)
    Averaging times run from one sample up to max_fraction of the record, where
    the estimate still has enough independent clusters to mean something.
    """
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    theta = np.concatenate([[0.0], np.cumsum(x)]) / fs
    m_max = max(1, int(n * max_fraction))
    ms = np.unique(np.logspace(0, math.log10(m_max), n_taus).astype(np.int64))
    taus = ms / fs
    adev = np.empty(len(ms))
    for i, m in enumerate(ms):
        d = theta[2 * m:] - 2.0 * theta[m:-m] + theta[:-2 * m]
        adev[i] = math.sqrt(float(np.dot(d, d)) / (2.0 * taus[i] ** 2 * len(d)))
    return taus, adev


def adev_at(taus, adev, tau):
    """Log-log interpolation of an Allan deviation curve at one averaging time."""
    if tau < taus[0] or tau > taus[-1]:
        return float("nan")
    return float(np.exp(np.interp(math.log(tau), np.log(taus), np.log(adev))))


def bias_instability(taus, adev, tau_min=1.0):
    """Flat floor of the Allan curve / 0.664 (IEEE 952), with where it occurs."""
    sel = taus >= tau_min
    if not np.any(sel):
        return float("nan"), float("nan")
    i = int(np.argmin(adev[sel]))
    return float(adev[sel][i] / 0.664), float(taus[sel][i])


def correlation_time(x, fs):
    """Lag at which the autocorrelation first drops below 1/e."""
    x = np.asarray(x, dtype=np.float64)
    x = x - x.mean()
    n = len(x)
    if n < 16 or not np.any(x):
        return float("nan")
    size = 1 << (2 * n - 1).bit_length()
    f = np.fft.rfft(x, size)
    acf = np.fft.irfft(f * np.conj(f), size)[:n]
    acf /= acf[0]
    below = np.nonzero(acf < 1.0 / math.e)[0]
    return float(below[0] / fs) if below.size else float(n / fs)


def uniform(t, x, fs):
    grid = np.arange(t[0], t[-1], 1.0 / fs)
    return grid, np.interp(grid, t, x)


def fnum(v, nd=6):
    return None if v is None or (isinstance(v, float) and not math.isfinite(v)) else round(float(v), nd)


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def characterize(rec, start=120.0, end_trim=60.0, baro_floor=0.5, block_s=300.0, progress=print):
    imu = rec.imu
    order = np.argsort(imu["t"], kind="stable")
    t_all = imu["t"][order]
    t_origin = float(t_all[0])
    t_rel = t_all - t_origin
    t_end = float(t_rel[-1]) - end_trim
    sel = (t_rel >= start) & (t_rel <= t_end)
    if np.sum(sel) < 1000:
        raise ValueError("fewer than 1000 IMU samples in the analysed segment - adjust --start/--end-trim")

    # Stamps are FreeRTOS ticks (10 ms) while the IMU samples every 12 ms: the
    # median logged step (10 ms) is NOT the sample period, and treating it as one
    # makes a steady sensor look like it loses every sixth sample. Take the real
    # period from timing_quality() and rebuild the real sample times.
    imu_timing = pilog_reader.timing_quality(t_rel[sel])
    t = pilog_reader.dequantise_times(t_rel[sel], imu_timing)
    acc_g = np.column_stack([imu["ax"][order][sel], imu["ay"][order][sel], imu["az"][order][sel]])
    gyro = np.column_stack([imu["gx"][order][sel], imu["gy"][order][sel], imu["gz"][order][sel]])
    temp = imu["temp"][order][sel]
    nominal = imu_timing["period_s"]
    fs = 1.0 / nominal
    duration = float(t[-1] - t[0])
    warnings = []

    # ---- stationarity ------------------------------------------------------------
    amag = np.linalg.norm(acc_g, axis=1)
    win = max(2, int(round(fs)))                 # 1 s windows
    nwin = len(amag) // win
    wstd = amag[:nwin * win].reshape(nwin, win).std(axis=1)
    moving = float(np.mean(wstd > 0.02)) * 100
    if moving > 0.1:
        warnings.append(f"{moving:.2f}% of 1 s windows show motion (|a| std > 0.02 g); "
                        "noise figures include it - trim with --start/--end-trim")

    quality = {"imu": imu_timing, "moving_windows_pct": fnum(moving, 3)}
    warnings.extend(pilog_reader.timing_warnings("IMU", imu_timing))

    # ---- accelerometer ---------------------------------------------------------------
    progress("    accelerometer...")
    f_mean = acc_g.mean(axis=0)
    g_measured = float(np.linalg.norm(f_mean))
    R = tr.level_rotation(f_mean)
    acc_nav = (acc_g * G0) @ R.T
    acc_nav[:, 2] -= g_measured * G0
    white_body = [white_sigma(t, acc_g[:, i] * G0, nominal) for i in range(3)]
    white_nav = [white_sigma(t, acc_nav[:, i], nominal) for i in range(3)]

    accel_allan = {"tau": None, "adev": []}
    vrw, bi, bi_tau = [], [], []
    for i in range(3):
        grid, xu = uniform(t, acc_g[:, i] * G0, fs)
        taus, adev = allan_deviation(xu, fs)
        accel_allan["tau"] = taus
        accel_allan["adev"].append(adev)
        vrw.append(adev_at(taus, adev, 1.0))
        b, bt = bias_instability(taus, adev)
        bi.append(b); bi_tau.append(bt)

    # Accelerometer bias vs temperature, over block means (slow effects only).
    nb = int(duration // block_s)
    temp_coef, temp_r2 = [None] * 3, [None] * 3
    temp_range = [fnum(np.nanmin(temp), 2), fnum(np.nanmax(temp), 2)]
    if nb >= 6 and np.all(np.isfinite(temp)):
        bidx = np.minimum(((t - t[0]) // block_s).astype(np.int64), nb - 1)
        counts = np.bincount(bidx, minlength=nb)
        tb = np.bincount(bidx, weights=temp, minlength=nb) / np.maximum(counts, 1)
        for i in range(3):
            ab = np.bincount(bidx, weights=acc_g[:, i], minlength=nb) / np.maximum(counts, 1)
            ok = counts > 0
            if np.ptp(tb[ok]) > 0.5:
                slope, icpt = np.polyfit(tb[ok], ab[ok], 1)
                pred = slope * tb[ok] + icpt
                ss_res = float(np.sum((ab[ok] - pred) ** 2))
                ss_tot = float(np.sum((ab[ok] - ab[ok].mean()) ** 2))
                temp_coef[i] = fnum(slope * 1000.0, 4)          # mg per degC
                temp_r2[i] = fnum(1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0, 3)

    # ---- gyroscope -----------------------------------------------------------------------
    progress("    gyroscope...")
    gyro_allan = {"tau": None, "adev": []}
    arw, gbi, gbi_tau = [], [], []
    for i in range(3):
        grid, xu = uniform(t, gyro[:, i], fs)
        taus, adev = allan_deviation(xu, fs)
        gyro_allan["tau"] = taus
        gyro_allan["adev"].append(adev)
        arw.append(adev_at(taus, adev, 1.0) * 60.0)            # deg/s*sqrt(s) -> deg/sqrt(h)
        b, bt = bias_instability(taus, adev)
        gbi.append(b * 3600.0); gbi_tau.append(bt)             # deg/s -> deg/h

    # ---- barometer ------------------------------------------------------------------------
    progress("    barometer...")
    baro = {}
    b = rec.baro
    if len(b.get("t", [])):
        bt_ = b["t"] - t_origin
        bsel = (bt_ >= start) & (bt_ <= t_end) & np.isfinite(b["pressure"]) & (b["pressure"] > 1)
        if np.sum(bsel) > 100:
            btt = bt_[bsel]
            bz = tr.barometric_altitude(b["pressure"][bsel], float(np.mean(b["pressure"][bsel])))
            btiming = pilog_reader.timing_quality(btt)
            warnings.extend(pilog_reader.timing_warnings("barometer", btiming))
            bnom = btiming["period_s"]
            bw = white_sigma(btt, bz, bnom)
            mins = int((btt[-1] - btt[0]) // 60)
            drift = float("nan")
            if mins >= 2:
                mi = np.minimum(((btt - btt[0]) // 60).astype(np.int64), mins - 1)
                mc = np.bincount(mi, minlength=mins)
                mm = np.bincount(mi, weights=bz, minlength=mins) / np.maximum(mc, 1)
                drift = float(np.ptp(mm[mc > 0]))
            baro = {
                "timing": btiming,
                "rate_between_pauses_hz": btiming["rate_hz"],
                "rate_effective_hz": btiming["rate_effective_hz"],
                "white_sigma_m": fnum(bw, 4),
                "drift_range_m": fnum(drift, 3),
                "about": "white_sigma is sample-to-sample noise, which the filter models. "
                         "drift_range is the spread of 1-minute means over the whole "
                         "recording: weather, not sensor noise - irrelevant for a "
                         "few-minute flight, dominant over hours.",
                "_series": (btt, bz),
            }

    # ---- GPS ----------------------------------------------------------------------------
    progress("    GPS...")
    gps = {}
    g = rec.gps
    if len(g.get("t", [])):
        gt = g["t"] - t_origin
        gsel = (gt >= start) & (gt <= t_end) & np.isfinite(g["lat"]) & np.isfinite(g["lon"])
        if np.sum(gsel) > 30:
            gtt = gt[gsel]
            quality["gps"] = pilog_reader.timing_quality(gtt)
            lat, lon, alt = g["lat"][gsel], g["lon"][gsel], g["alt"][gsel]
            e, n = tr.gps_to_enu(lat, lon, float(np.mean(lat)), float(np.mean(lon)))
            r = np.hypot(e - e.mean(), n - n.mean())
            gnom = float(np.median(np.diff(gtt)))
            _, eu = uniform(gtt, e, 1.0 / gnom)
            _, nu = uniform(gtt, n, 1.0 / gnom)
            gaps = np.diff(gtt) > 5 * gnom
            gps = {
                "fixes": int(len(gtt)),
                "rate_hz": fnum(1.0 / gnom, 3),
                "outages_over_5x_nominal": int(np.sum(gaps)),
                "sigma_east_m": fnum(np.std(e), 3),
                "sigma_north_m": fnum(np.std(n), 3),
                "sigma_up_m": fnum(np.nanstd(alt), 3),
                "cep50_m": fnum(np.percentile(r, 50), 3),
                "cep95_m": fnum(np.percentile(r, 95), 3),
                "correlation_time_east_s": fnum(correlation_time(eu, 1.0 / gnom), 1),
                "correlation_time_north_s": fnum(correlation_time(nu, 1.0 / gnom), 1),
                "mean_satellites": fnum(np.nanmean(g["sats"][gsel]), 2),
                "about": "The filter treats GPS errors as independent from fix to fix. "
                         "correlation_time shows how long they actually persist; when it "
                         "is much longer than the fix interval, consecutive fixes are not "
                         "independent evidence and the filter will trust GPS more than it "
                         "should.",
                "_series": (e, n),
            }

    # ---- power ------------------------------------------------------------------------------
    power = {}
    p = rec.ina
    if len(p.get("t", [])):
        pt = p["t"] - t_origin
        psel = (pt >= start) & (pt <= t_end)
        v, i_ma = p["bus_volts"][psel], p["current"][psel]
        valid = (v > 0.5) & (i_ma >= 0)
        if np.sum(valid) > 10:
            w = v[valid] * i_ma[valid] / 1000.0
            power = {
                "bus_volts_median": fnum(np.median(v[valid]), 4),
                "current_ma_median": fnum(np.median(i_ma[valid]), 2),
                "power_w_mean": fnum(np.mean(w), 4),
                "power_w_p95": fnum(np.percentile(w, 95), 4),
                "energy_wh": fnum(np.mean(w) * duration / 3600.0, 3),
                "note": "INA219 measures only the rail its shunt is wired in series with; "
                        "frameparser's bus-voltage scale also reads ~2.4% low.",
            }

    # ---- recommendation -----------------------------------------------------------------
    sigma_acc = [fnum(max(tr.MIN_SIGMA_ACC, s), 5) for s in white_nav]
    rec_notes = [
        "sigma_acc is the measured per-sample accelerometer noise in the navigation frame "
        "(the Qimu diagonal, as LKF.mlx describes). On the bench that is all the IMU error "
        "there is; in flight, vibration and scale error add to it - watch the NIS that "
        "trajectory.py reports and raise --q-scale until it approaches 1.",
    ]
    sigma_baro = None
    if baro:
        sigma_baro = fnum(max(baro_floor, baro["white_sigma_m"] or 0.0), 3)
        rec_notes.append(f"sigma_baro is the measured white noise, floored at {baro_floor} m: a "
                         "sensor at rest is a lower bound - in flight, airflow over the port "
                         "and heating add error the bench cannot show.")
    sigma_gps = None
    if gps:
        sigma_gps = [gps["sigma_east_m"], gps["sigma_north_m"]]
        rec_notes.append("sigma_gps is the measured position scatter; see gps.about on "
                         "correlated errors.")

    return {
        "source": os.path.abspath(rec.path),
        "sessions": rec.sessions,
        "segment": {"start_s": fnum(t[0], 3), "end_s": fnum(t[-1], 3),
                    "duration_h": fnum(duration / 3600.0, 3)},
        "data_quality": quality,
        "imu": {
            "accel": {
                "mean_g": [fnum(v, 6) for v in f_mean],
                "g_measured": fnum(g_measured, 6),
                "scale_error_pct_if_level": fnum((g_measured - 1.0) * 100.0, 3),
                "white_sigma_body_mps2": [fnum(v) for v in white_body],
                "white_sigma_nav_mps2": [fnum(v) for v in white_nav],
                "velocity_random_walk_mps_per_sqrt_s": [fnum(v) for v in vrw],
                "bias_instability_mps2": [fnum(v, 7) for v in bi],
                "bias_instability_at_tau_s": [fnum(v, 1) for v in bi_tau],
                "temperature_range_c": temp_range,
                "bias_temperature_coefficient_mg_per_c": temp_coef,
                "bias_temperature_fit_r2": temp_r2,
            },
            "gyro": {
                "bias_dps": [fnum(v, 5) for v in gyro.mean(axis=0)],
                "white_sigma_dps": [fnum(white_sigma(t, gyro[:, i], nominal), 5) for i in range(3)],
                "angle_random_walk_deg_per_sqrt_h": [fnum(v, 4) for v in arw],
                "bias_instability_deg_per_h": [fnum(v, 3) for v in gbi],
                "bias_instability_at_tau_s": [fnum(v, 1) for v in gbi_tau],
            },
        },
        "barometer": {k: v for k, v in baro.items() if not k.startswith("_")},
        "gps": {k: v for k, v in gps.items() if not k.startswith("_")},
        "power": power,
        "recommended": {"sigma_acc": sigma_acc, "sigma_gps": sigma_gps, "sigma_baro": sigma_baro,
                        "notes": rec_notes},
        "warnings": warnings,
        "_plot": {"accel": accel_allan, "gyro": gyro_allan,
                  "baro": baro.get("_series"), "gps": gps.get("_series"), "gps_info": gps},
    }


def plot(path, result, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    pd = result["_plot"]
    fig, axs = plt.subplots(2, 2, figsize=(13, 10))
    fig.suptitle(title, fontsize=11)
    labels = ["X", "Y", "Z"]

    for ax, key, unit, name in ((axs[0, 0], "accel", "m/s²", "Accelerometer"),
                                (axs[0, 1], "gyro", "deg/s", "Gyroscope")):
        taus = pd[key]["tau"]
        for i, adev in enumerate(pd[key]["adev"]):
            ax.loglog(taus, adev, label=labels[i])
        if taus is not None and len(taus):
            ref = pd[key]["adev"][0][0]
            ax.loglog(taus, ref * (taus / taus[0]) ** -0.5, "k:", lw=0.8, label="slope -1/2 (white noise)")
        ax.set_title(f"{name} Allan deviation")
        ax.set_xlabel("averaging time τ [s]"); ax.set_ylabel(f"σ(τ) [{unit}]")
        ax.grid(which="both", alpha=0.3); ax.legend(fontsize=8)

    if pd["baro"] is not None:
        bt, bz = pd["baro"]
        stride = max(1, len(bt) // 20000)
        axs[1, 0].plot(bt[::stride] / 3600.0, bz[::stride], lw=0.5)
        axs[1, 0].set_title("Barometric altitude (drift = weather)")
        axs[1, 0].set_xlabel("time [h]"); axs[1, 0].set_ylabel("altitude vs mean [m]")
        axs[1, 0].grid(alpha=0.3)

    if pd["gps"] is not None:
        e, n = pd["gps"]
        stride = max(1, len(e) // 20000)
        ax = axs[1, 1]
        ax.plot(e[::stride] - e.mean(), n[::stride] - n.mean(), ".", ms=1.5, alpha=0.4)
        info = pd["gps_info"]
        for key, style in (("cep50_m", "-"), ("cep95_m", "--")):
            rr = info.get(key)
            if rr:
                a = np.linspace(0, 2 * np.pi, 200)
                ax.plot(rr * np.cos(a), rr * np.sin(a), "r" + style, lw=1,
                        label=f"{key.split('_')[0].upper()} {rr:.1f} m")
        ax.set_aspect("equal", adjustable="datalim")
        ax.set_title("GPS scatter"); ax.set_xlabel("east [m]"); ax.set_ylabel("north [m]")
        ax.grid(alpha=0.3); ax.legend(fontsize=8)

    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="stationary recording: raw .bin or bin2json .json")
    ap.add_argument("--output", help="output path prefix (default: next to the source)")
    ap.add_argument("--start", type=float, default=120.0,
                    help="seconds skipped at the start, while it was still being handled (default 120)")
    ap.add_argument("--end-trim", type=float, default=60.0,
                    help="seconds skipped at the end (default 60)")
    ap.add_argument("--session", type=int, help="session id (default: the longest)")
    ap.add_argument("--baro-floor", type=float, default=0.5,
                    help="lower bound for the recommended sigma_baro, metres (default 0.5)")
    ap.add_argument("--plot", action="store_true")
    args = ap.parse_args()

    prefix = args.output or os.path.splitext(args.source)[0] + "_noise"
    print(f"[+] reading {args.source}")
    with redirect_stdout(io.StringIO()):
        rec = pilog_reader.load_recording(args.source)
    print(f"    {rec.frames:,} frames, {len(rec.sessions)} session(s)")
    rec = pilog_reader.select_session(rec, args.session)

    print("[+] analysing")
    result = characterize(rec, start=args.start, end_trim=args.end_trim, baro_floor=args.baro_floor)

    out = {k: v for k, v in result.items() if k != "_plot"}
    with open(prefix + ".json", "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    a, gy = result["imu"]["accel"], result["imu"]["gyro"]
    print(f"[+] {result['segment']['duration_h']} h analysed")
    # Console output stays ASCII: Windows consoles fall back to cp1252 when output
    # is piped, which cannot encode characters such as the square-root sign.
    print(f"    accel white noise (nav)  {a['white_sigma_nav_mps2']} m/s^2")
    print(f"    accel |g| at rest        {a['g_measured']} g  ({a['scale_error_pct_if_level']:+}%)")
    print(f"    accel bias instability   {a['bias_instability_mps2']} m/s^2")
    print(f"    gyro bias                {gy['bias_dps']} deg/s")
    print(f"    gyro ARW                 {gy['angle_random_walk_deg_per_sqrt_h']} deg/sqrt(h)")
    if result["barometer"]:
        bb = result["barometer"]
        print(f"    baro white noise         {bb['white_sigma_m']} m   (weather drift {bb['drift_range_m']} m)")
    if result["gps"]:
        gg = result["gps"]
        print(f"    GPS scatter E/N          {gg['sigma_east_m']} / {gg['sigma_north_m']} m   "
              f"CEP50 {gg['cep50_m']} m   correlation {gg['correlation_time_east_s']} s")
    if result["power"]:
        pw = result["power"]
        print(f"    power                    {pw['power_w_mean']} W mean  ({pw['energy_wh']} Wh)")
    q = result["data_quality"]
    qi = q["imu"]
    stamps = " (timestamps tick-quantised)" if qi.get("timestamps_quantised") else ""
    print(f"    IMU timing               {qi['rate_hz']} Hz{stamps}, "
          f"{qi['samples_missing_pct']}% genuinely missing")
    print(f"[+] recommended: {result['recommended']['sigma_acc']=}, "
          f"{result['recommended']['sigma_gps']=}, {result['recommended']['sigma_baro']=}")
    for w in result["warnings"]:
        print(f"[!] {w}")
    if args.plot:
        plot(prefix + ".png", result, os.path.basename(args.source))
        print(f"[+] plot -> {prefix}.png")
    print(f"[+] parameters -> {prefix}.json   (use with trajectory.py --params)")


if __name__ == "__main__":
    main()
