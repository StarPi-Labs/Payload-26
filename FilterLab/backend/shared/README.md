# Shared modules (copies, do not edit here)

Byte-identical copies of the team's Python code, so the FilterLab folder runs on
its own:

| File | Original |
|---|---|
| `frameparser.py`, `pilog_reader.py`, `kalman.py`, `trajectory.py`, `noise_characterization.py`, `imu_calibration.py` | `SD-Parser/` |
| `attitude.py` | `Dashboard/Backend/` |

Change the originals, then refresh these from the repository root:

```bash
python FilterLab/sync_shared.py
```

`python FilterLab/sync_shared.py --check` only reports copies that are out of date.
