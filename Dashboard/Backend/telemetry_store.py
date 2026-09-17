"""
telemetry_store.py -- scalable telemetry access for the Pi-LOG Dashboard.

Why this exists
---------------
The original server parsed every telemetry file completely -- readlines() plus
one dict per row -- on every request, including /api/flights, which only
needed each flight's *duration*. A 20-hour recording is ~9 million rows / 3 GB,
so listing flights ran out of memory, and because the failure escaped the
per-flight loop it took the whole flight list down with it.

This module replaces that with three ideas:

1. Cheap metadata. A flight's header metadata comes from the top of the file
   and its duration from the *last line* (a seek to the end), so listing flights
   costs a few kilobytes of I/O per file no matter how large the recording is.

2. A column cache. Each telemetry file is converted once into one raw binary
   file per column (float64 for time and GPS, float32 for everything else),
   read back through numpy memmaps. Building it is streamed chunk by chunk, so
   memory stays bounded; it runs in a background thread with progress reporting
   and is invalidated automatically when the source file changes.

3. Range queries with extreme-preserving decimation. A client asks for a time
   window and a point budget. Each bucket keeps its first row plus the rows
   holding the minimum and maximum of the key channels (altitude, acceleration,
   velocity), so apogee, peak-g and velocity extremes survive decimation no
   matter how far a 20-hour recording is zoomed out. Returned rows are real
   rows, never averages, so every channel in a row stays mutually consistent.

The module has no Flask dependency so it can be tested and used from the
command line on its own.
"""
import json
import math
import os
import re
import shutil
import threading
import time
from collections import OrderedDict
from datetime import datetime

import numpy as np
import pandas as pd

# Column order written by SD-Parser/json2telemetry.py. Files WITHOUT a header
# row are interpreted positionally against this list; files WITH a header are
# read by column name, so older or newer layouts (e.g. the legacy sample flight,
# which has no altitudeMSL/gpsAlt) are handled correctly.
SCHEMA = [
    "time", "altitude", "altitudeMSL", "velocity", "horizontalVelocity",
    "acceleration", "accelerationX", "accelerationY", "accelerationZ",
    "temperature", "pressure", "humidity",
    "gpsLat", "gpsLon", "gpsAlt", "pitch", "roll", "yaw",
    "gasResistance", "busVoltage", "current", "power",
]

# Values used when a column is absent from a file -- same defaults the original
# positional parser used, so old files render exactly as before.
COLUMN_DEFAULTS = {"temperature": 20.0, "pressure": 101.3, "humidity": 50.0}

# float32 halves the cache size, but it cannot hold these precisely enough:
# time needs millisecond resolution at 72 000 s, GPS needs ~1e-7 degrees.
FLOAT64_COLUMNS = {"time", "gpsLat", "gpsLon"}

# Channels whose extremes must survive decimation.
KEY_COLUMNS = ("altitude", "acceleration", "velocity")

# Decimal places in JSON output -- keeps responses small without visible loss.
DECIMALS = {"time": 3, "gpsLat": 7, "gpsLon": 7, "gasResistance": 1}
DEFAULT_DECIMALS = 4

ALLOWED_DATA_EXTENSIONS = {"txt", "csv", "log"}

CACHE_VERSION = 1
CACHE_DIRNAME = ".cache"

# Files smaller than this are cached synchronously inside the request, so small
# flights load immediately instead of going through a "processing" round trip.
SYNC_BUILD_BYTES = 32 * 1024 * 1024

CHUNK_ROWS = 500_000


# ---------------------------------------------------------------------------
# Header / metadata
# ---------------------------------------------------------------------------

def _empty_meta():
    return {
        "targetAltitude": None,
        "departureAltitude": None,
        "t0": None,
        "t0EpochMs": None,
        "modeTransitions": [],
    }


def _apply_meta_line(meta, line):
    """Parse one '# key=value' metadata line into meta (in place)."""
    m = re.match(r"^#\s*([A-Za-z0-9_]+)\s*=\s*([^#]+)$", line)
    if not m:
        return
    key, value = m.group(1), m.group(2).strip()

    if key in ("targetAltitude", "departureAltitude"):
        try:
            meta[key] = float(value)
        except ValueError:
            pass
    elif key in ("t0", "startTime"):
        meta["t0"] = value
        try:
            dt = datetime.fromisoformat(value.replace("Z", "+00:00"))
            meta["t0EpochMs"] = int(dt.timestamp() * 1000)
        except ValueError:
            pass
    elif key == "t0EpochMs":
        try:
            epoch_ms = int(float(value))
            meta["t0EpochMs"] = epoch_ms
            if meta["t0"] is None:
                meta["t0"] = datetime.fromtimestamp(epoch_ms / 1000).isoformat()
        except ValueError:
            pass
    elif key == "modeTransitions":
        transitions = []
        for part in value.split(","):
            part = part.strip()
            if ":" not in part:
                continue
            time_str, mode_str = part.split(":", 1)
            try:
                transitions.append({"time": float(time_str), "mode": mode_str.strip()})
            except ValueError:
                pass
        meta["modeTransitions"] = transitions


def _detect_delimiter(line):
    if "," in line:
        return ","
    if ";" in line:
        return ";"
    if "\t" in line:
        return "\t"
    return None  # whitespace


def _split(line, delimiter):
    return line.split(delimiter) if delimiter else line.split()


def parse_header(path, max_lines=500):
    """Read only the top of a telemetry file.

    Returns a dict with the metadata, column names, delimiter and whether a
    header row is present. Never reads past the first data line.
    """
    meta = _empty_meta()
    columns = None
    delimiter = ","
    has_header = False

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for _ in range(max_lines):
            raw = f.readline()
            if not raw:
                break
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                _apply_meta_line(meta, line)
                continue

            delimiter = _detect_delimiter(line)
            parts = [p.strip() for p in _split(line, delimiter)]
            try:
                float(parts[0])
                has_header = False
                columns = SCHEMA[:len(parts)] if len(parts) <= len(SCHEMA) else (
                    SCHEMA + [f"col{i}" for i in range(len(SCHEMA), len(parts))])
            except ValueError:
                has_header = True
                columns = parts
            break

    return {
        "meta": meta,
        "columns": columns or [],
        "delimiter": delimiter,
        "hasHeader": has_header,
    }


def read_last_time(path, delimiter):
    """Time value of the last data row, by reading only the file's tail."""
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        block = 65536
        while True:
            start = max(0, size - block)
            f.seek(start)
            data = f.read(size - start)
            lines = data.splitlines()
            # If we did not start at byte 0, the first line may be partial.
            if start > 0 and lines:
                lines = lines[1:]
            for raw in reversed(lines):
                s = raw.strip()
                if not s or s.startswith(b"#"):
                    continue
                text = s.decode("utf-8", errors="replace")
                try:
                    return float(_split(text, delimiter)[0])
                except (ValueError, IndexError):
                    continue  # header row in a tiny file -- keep looking
            if start == 0:
                return None
            block *= 4


def list_telemetry_files(flight_dir):
    telemetry_dir = os.path.join(flight_dir, "telemetry")
    if not os.path.isdir(telemetry_dir):
        return []
    files = []
    for name in sorted(os.listdir(telemetry_dir)):
        if name.startswith("."):
            continue
        full = os.path.join(telemetry_dir, name)
        if not os.path.isfile(full):
            continue
        if "." in name and name.rsplit(".", 1)[1].lower() in ALLOWED_DATA_EXTENSIONS:
            files.append(full)
    return files


# ---------------------------------------------------------------------------
# Decimation
# ---------------------------------------------------------------------------

def decimate_indices(i0, i1, max_points, key_arrays):
    """Row indices in [i0, i1) that fit max_points while keeping extremes.

    key_arrays: sequence of array-likes covering the same rows (memmaps are
    fine; only the [i0, i1) slice is read). Each bucket contributes its first
    row plus the argmin and argmax row of every key array.
    """
    n = i1 - i0
    if n <= 0:
        return np.empty(0, dtype=np.int64)
    if max_points <= 0 or n <= max_points:
        return np.arange(i0, i1, dtype=np.int64)

    # Read the key channels once; every refinement pass reuses them.
    keys = [np.asarray(arr[i0:i1]) for arr in key_arrays]

    per_bucket = 1 + 2 * len(keys)
    nb = max(1, (max_points - 2) // per_bucket)
    best = None
    # Where key channels are flat, argmin/argmax land on the bucket's first row
    # and dedupe away, wasting most of the budget. Refine: grow the bucket count
    # in proportion to the unused budget until the result fills it.
    for _ in range(4):
        nb = min(nb, n)
        idx = _bucket_picks(i0, n, nb, keys)
        if len(idx) <= max_points:
            best = idx
        else:
            break
        if len(idx) >= 0.8 * max_points or nb == n:
            break
        nb = int(nb * max(1.25, 0.95 * max_points / len(idx)))
    if best is None:
        best = _bucket_picks(i0, n, max(1, (max_points - 2) // per_bucket), keys)
    return best


def _bucket_picks(i0, n, nb, keys):
    bsize = n // nb
    usable = nb * bsize
    offsets = np.arange(nb, dtype=np.int64) * bsize
    picks = [offsets]
    for arr in keys:
        seg = arr[:usable].reshape(nb, bsize)
        picks.append(offsets + seg.argmax(axis=1))
        picks.append(offsets + seg.argmin(axis=1))
    # Rows beyond the last full bucket, plus the very last row, so the returned
    # range always reaches the end of the requested window.
    if usable < n:
        picks.append(np.array([usable], dtype=np.int64))
    picks.append(np.array([n - 1], dtype=np.int64))
    return i0 + np.unique(np.concatenate(picks))


def columns_to_json(cols):
    """Round and convert numpy columns to JSON-safe lists (NaN -> None)."""
    out = {}
    for name, arr in cols.items():
        a = np.round(np.asarray(arr, dtype=np.float64), DECIMALS.get(name, DEFAULT_DECIMALS))
        if np.all(np.isfinite(a)):
            out[name] = a.tolist()
        else:
            out[name] = [v if math.isfinite(v) else None for v in a.tolist()]
    return out


# ---------------------------------------------------------------------------
# The store
# ---------------------------------------------------------------------------

class _BuildState:
    def __init__(self, source_mtime):
        self.progress = 0.0
        self.error = None
        self.started = time.time()
        self.thread = None
        # Set before the work starts and cleared when it ends, whether the build
        # runs in a background thread or synchronously in the caller - status()
        # must report "building" in both cases.
        self.running = True
        self.source_mtime = source_mtime


class TelemetryStore:
    def __init__(self, cache_root=None):
        """cache_root: optional directory for caches. Default keeps each cache
        next to its source, in telemetry/.cache/<file>/ -- which means it lives
        on the same Docker volume as the data and survives restarts."""
        self.cache_root = cache_root
        self._lock = threading.RLock()
        self._info = {}        # path -> (size, mtime, info)
        self._builds = {}      # path -> _BuildState
        self._mmaps = {}       # path -> (manifest_key, {col: memmap})
        self._results = OrderedDict()   # query LRU

    # ---- paths ------------------------------------------------------------

    def cache_dir(self, path):
        if self.cache_root:
            safe = re.sub(r"[^A-Za-z0-9_.-]", "_", os.path.abspath(path))
            return os.path.join(self.cache_root, safe)
        return os.path.join(os.path.dirname(path), CACHE_DIRNAME, os.path.basename(path))

    # ---- metadata -----------------------------------------------------------

    def file_info(self, path):
        """Fast, cached: header metadata + duration from the tail."""
        st = os.stat(path)
        with self._lock:
            hit = self._info.get(path)
            if hit and hit[0] == st.st_size and hit[1] == st.st_mtime:
                return hit[2]

        header = parse_header(path)
        last = read_last_time(path, header["delimiter"])
        info = {
            "path": path,
            "size": st.st_size,
            "mtime": st.st_mtime,
            "meta": header["meta"],
            "columns": header["columns"],
            "delimiter": header["delimiter"],
            "hasHeader": header["hasHeader"],
            "lastTime": last,
        }
        with self._lock:
            self._info[path] = (st.st_size, st.st_mtime, info)
        return info

    # ---- cache status -----------------------------------------------------

    def _manifest(self, path):
        manifest_path = os.path.join(self.cache_dir(path), "manifest.json")
        try:
            with open(manifest_path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
        except (OSError, ValueError):
            return None
        st = os.stat(path)
        src = manifest.get("source", {})
        if (manifest.get("version") != CACHE_VERSION
                or src.get("size") != st.st_size
                or abs(src.get("mtime", 0) - st.st_mtime) > 1e-3):
            return None
        return manifest

    def status(self, path):
        with self._lock:
            build = self._builds.get(path)
            if build is not None and build.running:
                return {"state": "building", "progress": round(build.progress, 4),
                        "elapsed": round(time.time() - build.started, 1)}
            errored = build is not None and build.error
        if self._manifest(path) is not None:
            return {"state": "ready", "progress": 1.0}
        if errored:
            return {"state": "error", "error": build.error}
        return {"state": "missing", "progress": 0.0}

    def ensure(self, path, background=True):
        """Make sure a cache exists or is being built. Returns status().

        Safe to call concurrently: the check and the registration of a new build
        happen under one lock, so simultaneous requests for the same uncached
        file start exactly one build.
        """
        if self._manifest(path) is not None:
            return {"state": "ready", "progress": 1.0}

        mtime = os.stat(path).st_mtime
        with self._lock:
            build = self._builds.get(path)
            if build is not None and build.running:
                return self.status(path)
            # A failed build is not retried in a loop on every poll - only once
            # the source file has actually changed (e.g. it was re-exported).
            if build is not None and build.error and build.source_mtime == mtime:
                return self.status(path)
            state = _BuildState(mtime)
            self._builds[path] = state

        if not background or os.path.getsize(path) <= SYNC_BUILD_BYTES:
            self._build(path, state)
            return self.status(path)

        state.thread = threading.Thread(target=self._build, args=(path, state),
                                        name=f"cache-build:{os.path.basename(path)}",
                                        daemon=True)
        state.thread.start()
        return self.status(path)

    # ---- building -----------------------------------------------------------

    def _build(self, path, state):
        try:
            self._build_inner(path, state)
            state.progress = 1.0
            state.error = None
        except Exception as exc:  # surfaced through status(), never raised
            state.error = f"{type(exc).__name__}: {exc}"
        finally:
            state.running = False

    def _build_inner(self, path, state):
        info = self.file_info(path)
        final_dir = self.cache_dir(path)
        tmp_dir = f"{final_dir}.tmp-{os.getpid()}-{threading.get_ident()}"
        if os.path.exists(tmp_dir):
            shutil.rmtree(tmp_dir, ignore_errors=True)
        os.makedirs(tmp_dir, exist_ok=True)

        file_columns = list(info["columns"])
        # Union of the standard schema and whatever else the file carries, so
        # columns added later (append-only) flow through without code changes.
        all_columns = list(SCHEMA) + [c for c in file_columns if c not in SCHEMA]
        dtypes = {c: (np.float64 if c in FLOAT64_COLUMNS else np.float32) for c in all_columns}

        try:
            self._write_columns(path, info, state, tmp_dir, final_dir, file_columns,
                                all_columns, dtypes)
        except BaseException:
            # Never leave a half-written temp cache behind to confuse a retry.
            shutil.rmtree(tmp_dir, ignore_errors=True)
            raise

    def _write_columns(self, path, info, state, tmp_dir, final_dir, file_columns,
                       all_columns, dtypes):
        handles = {c: open(os.path.join(tmp_dir, f"{c}.bin"), "wb") for c in all_columns}
        rows = 0
        last_time = None
        monotonic = True
        total = max(1, info["size"])

        read_kwargs = dict(
            comment="#",
            skip_blank_lines=True,
            chunksize=CHUNK_ROWS,
            engine="c",
            on_bad_lines="skip",
        )
        if info["delimiter"] is None:
            read_kwargs["sep"] = r"\s+"
        else:
            read_kwargs["sep"] = info["delimiter"]
        if info["hasHeader"]:
            read_kwargs["header"] = 0
        else:
            read_kwargs["header"] = None
            read_kwargs["names"] = file_columns

        try:
            with open(path, "rb") as fh:
                for chunk in pd.read_csv(fh, **read_kwargs):
                    chunk.columns = [str(c).strip() for c in chunk.columns]
                    n = len(chunk)
                    if n == 0:
                        continue
                    for col in all_columns:
                        if col in chunk.columns:
                            values = pd.to_numeric(chunk[col], errors="coerce").to_numpy(
                                dtype=np.float64, na_value=np.nan)
                        else:
                            values = np.full(n, COLUMN_DEFAULTS.get(col, 0.0), dtype=np.float64)
                        values.astype(dtypes[col]).tofile(handles[col])

                    t = pd.to_numeric(chunk["time"], errors="coerce").to_numpy(
                        dtype=np.float64, na_value=np.nan) if "time" in chunk.columns else None
                    if t is not None and len(t):
                        if last_time is not None and t[0] < last_time:
                            monotonic = False
                        if np.any(np.diff(t) < 0):
                            monotonic = False
                        last_time = float(t[-1])

                    rows += n
                    state.progress = min(0.97, fh.tell() / total)
        finally:
            for h in handles.values():
                h.close()

        if rows > 0 and not monotonic:
            self._sort_by_time(tmp_dir, all_columns, dtypes, rows)
            state.progress = 0.99

        time_first = time_last = None
        if rows > 0:
            tm = np.memmap(os.path.join(tmp_dir, "time.bin"), dtype=np.float64, mode="r")
            time_first, time_last = float(tm[0]), float(tm[-1])
            del tm

        st = os.stat(path)
        manifest = {
            "version": CACHE_VERSION,
            "source": {"size": st.st_size, "mtime": st.st_mtime,
                       "name": os.path.basename(path)},
            "rows": rows,
            "columns": {c: np.dtype(dtypes[c]).name for c in all_columns},
            "fileColumns": file_columns,
            "sortedOnBuild": not monotonic,
            "timeFirst": time_first,
            "timeLast": time_last,
            "meta": info["meta"],
            "builtAt": datetime.now().isoformat(),
        }
        with open(os.path.join(tmp_dir, "manifest.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)

        # Swap in atomically-enough: drop any stale cache (and its open maps),
        # then move the fresh one into place.
        with self._lock:
            self._mmaps.pop(path, None)
            self._results.clear()
        if os.path.exists(final_dir):
            shutil.rmtree(final_dir, ignore_errors=True)
        os.makedirs(os.path.dirname(final_dir), exist_ok=True)
        os.replace(tmp_dir, final_dir)

    @staticmethod
    def _sort_by_time(cache_dir, columns, dtypes, rows):
        """Reorder every column by time. Rare: json2telemetry already sorts."""
        tm = np.fromfile(os.path.join(cache_dir, "time.bin"), dtype=np.float64, count=rows)
        order = np.argsort(tm, kind="stable")
        del tm
        for col in columns:
            p = os.path.join(cache_dir, f"{col}.bin")
            arr = np.fromfile(p, dtype=dtypes[col], count=rows)
            arr[order].tofile(p)

    # ---- querying -----------------------------------------------------------

    def _open(self, path):
        manifest = self._manifest(path)
        if manifest is None:
            return None, None
        key = (manifest["source"]["size"], manifest["source"]["mtime"], manifest["rows"])
        with self._lock:
            hit = self._mmaps.get(path)
            if hit and hit[0] == key:
                return manifest, hit[1]
        cdir = self.cache_dir(path)
        maps = {}
        if manifest["rows"] > 0:
            for col, dtype in manifest["columns"].items():
                maps[col] = np.memmap(os.path.join(cdir, f"{col}.bin"),
                                      dtype=np.dtype(dtype), mode="r",
                                      shape=(manifest["rows"],))
        with self._lock:
            self._mmaps[path] = (key, maps)
        return manifest, maps

    def query(self, paths, start=None, end=None, max_points=20000):
        """Decimated rows for [start, end] across one or more cached files.

        All paths must be cached (status 'ready'). Returns (columns, info) where
        columns maps name -> numpy array.
        """
        opened = []
        for p in paths:
            manifest, maps = self._open(p)
            if manifest is None:
                raise RuntimeError(f"cache not ready for {os.path.basename(p)}")
            opened.append((p, manifest, maps))

        cache_key = (tuple((p, m["source"]["mtime"], m["rows"]) for p, m, _ in opened),
                     None if start is None else round(float(start), 3),
                     None if end is None else round(float(end), 3),
                     int(max_points))
        with self._lock:
            if cache_key in self._results:
                self._results.move_to_end(cache_key)
                return self._results[cache_key]

        total_rows = sum(m["rows"] for _, m, _ in opened)
        parts = []
        for p, manifest, maps in opened:
            rows = manifest["rows"]
            if rows == 0:
                continue
            t = maps["time"]
            i0 = 0 if start is None else int(np.searchsorted(t, start, side="left"))
            i1 = rows if end is None else int(np.searchsorted(t, end, side="right"))
            if i1 <= i0:
                continue
            keys = [maps[k] for k in KEY_COLUMNS if k in maps]
            idx = decimate_indices(i0, i1, max_points, keys)
            parts.append({col: np.asarray(mm[idx]) for col, mm in maps.items()})

        columns = {}
        if parts:
            names = list(parts[0].keys())
            for part in parts[1:]:
                names += [n for n in part if n not in names]
            for name in names:
                columns[name] = np.concatenate([
                    part[name].astype(np.float64) if name in part
                    else np.full(len(part["time"]), COLUMN_DEFAULTS.get(name, 0.0))
                    for part in parts])
            if len(parts) > 1:
                order = np.argsort(columns["time"], kind="stable")
                columns = {k: v[order] for k, v in columns.items()}
                n = len(columns["time"])
                if n > max_points > 0:
                    keys = [columns[k] for k in KEY_COLUMNS if k in columns]
                    idx = decimate_indices(0, n, max_points, keys)
                    columns = {k: v[idx] for k, v in columns.items()}

        info = {
            "totalRows": total_rows,
            "returnedRows": int(len(columns.get("time", []))),
            "decimated": int(len(columns.get("time", []))) < self._rows_in_range(opened, start, end),
        }
        result = (columns, info)
        with self._lock:
            self._results[cache_key] = result
            while len(self._results) > 16:
                self._results.popitem(last=False)
        return result

    @staticmethod
    def _rows_in_range(opened, start, end):
        n = 0
        for _, manifest, maps in opened:
            if manifest["rows"] == 0:
                continue
            t = maps["time"]
            i0 = 0 if start is None else int(np.searchsorted(t, start, side="left"))
            i1 = manifest["rows"] if end is None else int(np.searchsorted(t, end, side="right"))
            n += max(0, i1 - i0)
        return n


def merge_meta(infos):
    """Combine header metadata from several telemetry files of one flight."""
    meta = _empty_meta()
    transitions = []
    for info in infos:
        m = info["meta"]
        for key in ("targetAltitude", "departureAltitude", "t0", "t0EpochMs"):
            if meta[key] is None and m.get(key) is not None:
                meta[key] = m[key]
        transitions.extend(m.get("modeTransitions") or [])
    transitions.sort(key=lambda x: x.get("time", 0))
    meta["modeTransitions"] = transitions
    return meta
