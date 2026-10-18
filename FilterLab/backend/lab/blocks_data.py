"""Data and Select blocks: where a recipe starts, and which part it keeps."""
from dataclasses import replace

import numpy as np

from . import sources
from .catalog import Param, Result, block, p_signals
from .signals import BlockError


@block("source", "data", "Recording",
       "start with {recording}|session {session} · time zero at {zero}",
       """
The top of every recipe: the recording to work on, as raw as it gets.

A .bin is exactly what the payload wrote (SD card, P4 backup, or a wired
capture), converted to physical units by the same parser every other tool
uses. Every sensor keeps its own rate: IMU about 83 Hz, barometer 10-50 Hz,
GPS 1 Hz. Nothing is merged, filled in or smoothed.

Session: the payload's clock restarts when it reboots, so a recording can hold
several sessions. "longest" picks the one with the most IMU data.

Time zero: seconds since the payload booted, since the first sample, or since
launch (BOOST). The choice only shifts the clock.
""",
       [Param("recording", "recording", "", "Which recording to open."),
        Param("session", "choice", "longest",
              options=[("longest", "longest")] + [(str(i), f"#{i}") for i in range(1, 9)]),
        Param("zero", "choice", "first",
              options=[("first", "first sample"), ("launch", "launch"), ("boot", "payload boot")])],
       hat=True)
def source(ws, p, ctx):
    if not p["recording"]:
        raise BlockError("pick a recording in this block")
    arrays, meta = sources.load(p["recording"])
    session = sources.pick_session(arrays, meta, p["session"])
    sigs = sources.to_signals(arrays, meta, session)
    if not sigs:
        raise BlockError("this session has no sensor data")
    mode = next((s for s in sigs if s.name == "mode"), None)
    launch = None
    if mode is not None and mode.n:
        boost = np.nonzero(mode.v == 4)[0]
        if len(boost):
            launch = float(mode.t[boost[0]])
    firsts = [float(s.t[0]) for s in sigs if s.n and not s.name.startswith("dash.")]
    first = min(firsts) if firsts else 0.0
    no_launch = p["zero"] == "launch" and launch is None
    if p["zero"] == "launch" and launch is not None:
        offset = launch
    elif p["zero"] in ("first", "launch"):
        offset = first
    else:
        offset = 0.0
    for s in sigs:
        ws.put(replace(s, t=s.t - offset))
    ws.meta = {"recording": meta, "session": session, "offset": offset,
               "launch": None if launch is None else launch - offset}

    res = Result(changed=[s.name for s in sigs])
    if no_launch:
        res.warnings.append("this session never reached BOOST (no launch): time zero is the first sample instead")
    sessions = meta.get("sessions") or []
    res.notes.append(f"{meta.get('label')}: {meta.get('frames', 0):,} frames, "
                     f"{len(sessions)} session(s), using session {session + 1}"
                     + (f", {meta.get('crcFailures')} CRC failures" if meta.get("crcFailures") else ""))
    calib = meta.get("calib")
    if calib and calib.get("mpu_accel_fs_g"):
        res.notes.append(f"calibration frame: accel ±{calib['mpu_accel_fs_g'][0]} g "
                         f"(±{calib['mpu_accel_fs_g'][1]} g in BOOST), gyro ±{calib.get('gyro_fs_dps')} °/s")
    elif meta.get("kind") == "raw":
        res.warnings.append("no calibration frame in this session - default scaling was used")
    rows = [["signal", "samples", "rate", "from", "to"]]
    for s in sigs:
        info = s.info()
        rows.append([s.name, f"{s.n:,}", f"{info.get('rate', 0):g} /s" if info.get("rate") else "",
                     f"{info.get('x0', 0):.2f} s", f"{info.get('x1', 0):.2f} s"])
    res.table = rows
    if launch is not None:
        res.notes.append(f"launch (BOOST) at {launch - offset:.2f} s")
    if len(sessions) > 1:
        res.notes.append("other sessions: " + ", ".join(
            f"#{s['id'] + 1} {s['t_start']:.0f}-{s['t_end']:.0f} s" for s in sessions if s["id"] != session))
    return res


@block("crop", "select", "Crop",
       "keep from {start} s to {end} s {ref}",
       """
Keeps only part of the recording. Work on the interesting part first: every
later block gets faster.

"around launch" counts from the launch (BOOST), so -10 to 60 means from ten
seconds before launch to a minute after.
""",
       [Param("start", "number", 0, unit="s"),
        Param("end", "number", 60, unit="s"),
        Param("ref", "choice", "time",
              options=[("time", "of the recording"), ("launch", "around launch")])])
def crop(ws, p, ctx):
    lo, hi = p["start"], p["end"]
    if p["ref"] == "launch":
        launch = ws.meta.get("launch")
        if launch is None:
            return Result(warnings=["no launch (BOOST) in this recording, so there is nothing to crop around: "
                                    "everything was kept (crop 'of the recording' instead)"])
        lo, hi = launch + lo, launch + hi
    if hi <= lo:
        raise BlockError("'to' must be after 'from'")
    res = Result()
    for name, s in list(ws.signals.items()):
        if s.axis != "time":
            continue
        keep = (s.t >= lo) & (s.t <= hi)
        if s.step and s.n and not keep.all():
            # keep the state that was active when the window opens
            before = np.nonzero(s.t < lo)[0]
            if len(before):
                keep[before[-1]] = True
        ws.put(replace(s, t=s.t[keep], v=s.v[keep]))
        res.changed.append(name)
    empty = [n for n in res.changed if ws.signals[n].n == 0]
    if empty:
        res.warnings.append(f"nothing left of: {', '.join(empty)}")
    res.notes.append(f"kept {lo:.2f} s to {hi:.2f} s")
    return res


@block("pick", "select", "Keep signals",
       "keep only {signals}",
       """
Drops every signal except the ones named, which tidies the signal list.
Patterns work: "imu.*, baro.pressure".
""",
       [p_signals("imu.*, baro.*")])
def pick(ws, p, ctx):
    keep = set(ws.select(p["signals"], axis=None))
    if not keep:
        raise BlockError(f"nothing matches '{p['signals']}'")
    dropped = [n for n in ws.names() if n not in keep]
    for n in dropped:
        del ws.signals[n]
    return Result(notes=[f"kept {len(keep)} signal(s), dropped {len(dropped)}"])
