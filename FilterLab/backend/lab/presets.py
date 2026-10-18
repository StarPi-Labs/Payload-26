"""Built-in recipes: starting points that show how the blocks fit together.
The recording is left empty; the editor fills in the one that is open."""


def b(type, **params):
    return {"type": type, "params": params, "enabled": True}


SOURCE = b("source", recording="", session="longest", zero="first")
SOURCE_LAUNCH = b("source", recording="", session="longest", zero="launch")

PRESETS = [
    {
        "id": "raw",
        "name": "Just the raw data",
        "description": "Only the Recording block: every sensor as the payload logged it. "
                       "Click it and pick signals to look at.",
        "blocks": [SOURCE],
    },
    {
        "id": "website",
        "name": "Website trajectory (what the Dashboard shows)",
        "description": "The Dashboard's Kalman/RTS altitude and speeds in one block - "
                       "the same code as json2telemetry.py --kalman.",
        "blocks": [SOURCE, b("website_trajectory", attitude="static", q_scale=1.0, heading=0.0)],
    },
    {
        "id": "euroc",
        "name": "EuRoC estimator, step by step (MATLAB)",
        "description": "The team's MATLAB chain as separate blocks: pad calibration (calib), "
                       "attitude (Mahony), rotation to the Earth frame, barometric altitude, GPS "
                       "to metres, then the linear Kalman filter and the RTS smoother. "
                       "Needs a flight (a BOOST in the recording).",
        "blocks": [SOURCE_LAUNCH,
                   b("crop", start=-10, end=180, ref="launch"),
                   b("fix_timestamps"),
                   b("pad_calibration", what="both", n=500, where="first", check=True),
                   b("attitude", method="mahony", kp=0.5, ki=0.0, beta=0.1, alpha=0.98),
                   b("earth_frame", g="standard", heading=0.0),
                   b("baro_altitude", name="baro.altitude", pressure="baro.pressure", seconds=5.0),
                   b("gps_local", lat="gps.lat", lon="gps.lon", fixes=10),
                   b("lkf_rts", altitude="baro.altitude", east="gps.east", north="gps.north",
                     sigma_acc=0.0, q_scale=1.0, sigma_gps=2.5, sigma_baro=1.0, smooth=True)],
    },
    {
        "id": "baro",
        "name": "Clean up the barometer",
        "description": "Barometric altitude, then three ways to smooth it (low-pass, Savitzky-Golay, "
                       "Kalman) and the vertical speed from the smoothed one. Switch blocks off "
                       "to compare.",
        "blocks": [SOURCE,
                   b("baro_altitude", name="baro.altitude", pressure="baro.pressure", seconds=5.0),
                   b("lowpass", signals="baro.altitude", cutoff=2.0, order="4", zero_phase=True, keep=True),
                   b("savgol", signals="baro.altitude", window=1.0, order=3, keep=True),
                   b("kalman_smooth", signals="baro.altitude", model="trend", noise=0.5, change=2.0,
                     rts=True, keep=True),
                   b("derivative", name="baro.vspeed", signal="baro.altitude.kf")],
    },
    {
        "id": "noise",
        "name": "Noise check (still recording)",
        "description": "Allan deviation and spectrum of the IMU, and the spread of every sensor - "
                       "the numbers behind the Kalman filter's σ. Use a long recording of the "
                       "payload sitting still.",
        "blocks": [SOURCE,
                   b("stats", signals="imu.*, baro.pressure, power.*", start=0.0, end=0.0),
                   b("spectrum", signal="imu.az", segment=5.0),
                   b("allan", signal="imu.gz"),
                   b("allan", signal="imu.az")],
    },
    {
        "id": "six",
        "name": "Calibrate the accelerometer (6 positions)",
        "description": "For a recording of the payload still in six poses (see "
                       "Documentation/CALIBRATION.md): finds offset and scale, applies them and "
                       "checks that |a| now reads 1 g.",
        "blocks": [SOURCE,
                   b("six_position", min_still=3.0),
                   b("apply_calibration", source="block", ox=0, oy=0, oz=0, sx=1, sy=1, sz=1),
                   b("magnitude", name="imu.amag", x="imu.ax", y="imu.ay", z="imu.az"),
                   b("stats", signals="imu.amag", start=0.0, end=0.0)],
    },
]
