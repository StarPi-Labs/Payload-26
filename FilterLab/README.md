# Pi-LOG Filter Lab

A second website, next to the Dashboard, where anyone on the team can build a
filter for the payload's data by **stacking blocks**, like a Scratch script, and
see at every step what each block does to the data.

```
┌ start with  fly.bin  · session longest · time zero at launch ┐   ← the raw recording
├ keep from -10 s to 180 s around launch                       │
├ remove the gyro bias using 500 samples of the start          │   ← the MATLAB calib
├ estimate the orientation with Mahony                         │
├ turn the accelerometer into east/north/up, remove 1 g        │
├ make baro.altitude = height above the pad from baro.pressure │
├ keep only changes slower than 2 Hz in baro.altitude          │   ← add / remove / reorder
└ Kalman filter on nav.a* with altitude baro.altitude …        ┘
```

## The idea

- **Start as raw as it gets.** The Recording block opens the `.bin` the payload
  wrote (SD card, P4 SD backup, or a wired capture), or a `bin2json` `.json`.
  Every sensor keeps its own rate, converted by the same `frameparser.py` all
  tools use: nothing merged, filled in or smoothed. A Dashboard flight can be
  opened too (it is de-duplicated back to per-sensor samples), plus the
  Dashboard's own derived columns as `dash.*` for comparison.
- **One block, one idea.** Crop, calibrate, filter, derive, estimate, analyse.
  Every block says in a sentence what it does, and the panel underneath explains
  it in plain language, with the trade-offs.
- **See every step.** Click any block: the chart shows its output (solid) over
  its input (dashed), and the panel lists what it found (biases, apogee, NIS,
  warnings).
- **The website's pipeline is a block.** *Website trajectory* runs exactly
  what `json2telemetry.py --kalman` does for the Dashboard. The preset *EuRoC
  estimator, step by step* lays the team's MATLAB chain (calib → imufilter
  Mahony → rotatepoint → LKF → RTS) out as separate blocks to play with.
- **Same code as everywhere else.** The engine calls SD-Parser's `kalman.py`,
  `trajectory.py`, `pilog_reader.py`, `noise_characterization.py` and
  `imu_calibration.py`, and the Dashboard's `attitude.py`. A recipe therefore
  reproduces the other tools' numbers instead of re-implementing them.

## Using it

1. Pick a recording in the bar (or **Upload…** a `.bin`).
2. Drag blocks from the categories on the left and snap them under the stack.
   Loose blocks are greyed out and do not run. Right-click a block to switch it
   off (the data passes through), duplicate or delete it.
3. Every change re-runs the recipe; unchanged blocks above come from a cache, so
   turning a knob is quick. ⚠ on a block = an error or a warning (click it).
4. Click a block and tick signals on the right to plot them. Drag on the chart
   to zoom (double-click resets).
5. **Save** stores the recipe on the server for the team; *More → Download
   recipe* gives a `.json` file. *More → Export* writes the plotted signals as
   CSV.

Presets to start from: raw data, the website trajectory, the EuRoC estimator,
barometer clean-up, a noise check (Allan deviation, spectrum), and the six-position
accelerometer calibration ([Documentation/CALIBRATION.md](../Documentation/CALIBRATION.md)).

### Blocks

| Category | Blocks |
|---|---|
| Select | Crop, Keep signals |
| Fix & calibrate | Pad calibration (MATLAB `calib`), Remap axes, Apply IMU calibration, Scale & offset, Fix IMU timestamps |
| Clean | Moving average, Median, Low-pass, High-pass, Notch, Savitzky-Golay, Remove outliers, Resample, Kalman smoother |
| Derive | Math, Magnitude, Derivative, Integral, Altitude from pressure, GPS to metres |
| Estimate | Attitude (Dashboard / Mahony / Madgwick / complementary / gyro), Rotate to Earth frame, Kalman filter + RTS, Website trajectory |
| Analyze | Statistics, Spectrum, Allan deviation, Compare, Six-position calibration |

Adding a block: write a function with `@block(...)` in one of
`backend/lab/blocks_*.py`. Its sentence, fields and help text come from that one
place, and the editor builds itself from `GET /api/catalog`.

## Running it

### Docker (on the server, like the Dashboard)

```bash
cd FilterLab
docker compose up -d --build
```

Then open `http://<server>:5050`. The build context is the repository root,
because the image copies SD-Parser's modules and the Dashboard's `attitude.py`
in; `Dockerfile.dockerignore` keeps everything else, recordings above all, out
of the build.

Paths go in `FilterLab/.env`:

```ini
LAB_PORT=5050
LAB_RECORDINGS_PATH=/srv/pilog/recordings
PILOG_DATA_PATH=/srv/pilog/data
```

Recordings and Dashboard data are mounted **read-only**. Uploads, saved recipes
and parse caches live in the `lab-data` volume.

**No authentication**, like the Dashboard: keep it on the LAN or behind a VPN.

### Locally, for development

```bash
cd FilterLab/backend
pip install -r requirements.txt
python lab_server.py
```

```bash
cd FilterLab/frontend
npm install
npm run dev
```

`npm run dev` serves the editor on http://localhost:5173 and forwards `/api` to
the server on port 5050; `npm run build` produces `frontend/dist`, which
`lab_server.py` serves on its own.

## Limits

- Blocks that run sample by sample (attitude, Kalman) refuse more than 5
  million samples: crop first. A whole 20-hour recording is better handled by
  `trajectory.py` / `noise_characterization.py` on the command line.
- ARMED-mode recordings keep only a 0.4 s burst every 10 s, so filters that
  assume even spacing warn about the gaps.
- One server process holds the step cache, so it is meant for a team, not for the
  public internet.
