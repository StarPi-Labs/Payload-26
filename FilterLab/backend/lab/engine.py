"""
Running recipes.

A recipe is a list of blocks, the first one the Recording "hat". Every block
gets a copy of the workspace the block above left behind. The result of each
step is cached under a hash of everything up to and including that block, so
changing block 5 re-runs blocks 5..n only; blocks 1-4 come from the cache.
That keeps the editor interactive while someone turns a knob.
"""
import hashlib
import json
import threading
import time
import traceback
from collections import OrderedDict

import numpy as np

# Registering the blocks: importing the modules is what fills the catalog.
from . import blocks_analyze, blocks_clean, blocks_data, blocks_derive, blocks_estimate, blocks_fix  # noqa: F401
from . import sources
from .catalog import REGISTRY, Result
from .signals import BlockError, Workspace, decimate, values_at

CACHE_STEPS = 96
# Results of this many recordings stay in memory; a 20-hour recording alone holds
# several hundred MB, so older ones are dropped as a whole.
CACHE_RECORDINGS = 2
EXPORT_MAX_ROWS = 2_000_000


class Engine:
    def __init__(self):
        self._steps = OrderedDict()          # key -> (workspace, step json, recording key)
        self._recordings = OrderedDict()     # recording key -> None, least recently used first
        self._lock = threading.RLock()

    def _remember(self, key, ws, record, recording):
        with self._lock:
            self._steps[key] = (ws, record, recording)
            self._recordings[recording] = None
            self._recordings.move_to_end(recording)
            while len(self._recordings) > CACHE_RECORDINGS:
                old, _ = self._recordings.popitem(last=False)
                for k in [k for k, v in self._steps.items() if v[2] == old]:
                    del self._steps[k]
            while len(self._steps) > CACHE_STEPS:
                self._steps.popitem(last=False)

    # ---- running -------------------------------------------------------------

    def run(self, blocks):
        """Run a recipe. Returns one step record per block (results, errors,
        signal lists) - the snapshots stay cached for series().

        After a failed Recording block nothing else can run; those blocks are
        returned as "notRun". After any other failure the rest still runs on the
        unchanged data, and their own errors are marked "afterError" - they are
        most likely consequences, not separate problems."""
        if not blocks:
            raise BlockError("the recipe is empty")
        if blocks[0].get("type") != "source":
            raise BlockError("a recipe starts with the Recording block")
        steps = []
        ws = Workspace()
        key = "pilog-lab-v1"
        recording = None
        failed_before = False
        for i, raw in enumerate(blocks):
            if i > 0 and steps and steps[0].get("error"):
                steps.append({"id": raw.get("id"), "key": None, "type": raw.get("type"), "ok": True,
                              "error": None, "notRun": True, "changed": [], "warnings": [],
                              "notes": ["not run: the Recording block above failed"], "table": None,
                              "download": None, "ms": 0.0, "cached": False, "signals": []})
                continue
            btype = REGISTRY.get(raw.get("type"))
            enabled = raw.get("enabled", True) is not False
            material = {"type": raw.get("type"), "params": raw.get("params") or {}, "enabled": enabled}
            if i == 0:
                # a re-uploaded or re-exported file must not hit old results
                try:
                    rec = sources.resolve((raw.get("params") or {}).get("recording", ""))
                    material["file"] = [rec["size"], rec["modified"]]
                except BlockError:
                    pass
            key = hashlib.sha1((key + json.dumps(material, sort_keys=True, default=str))
                               .encode()).hexdigest()
            if i == 0:
                recording = key
            with self._lock:
                hit = self._steps.get(key)
                if hit:
                    self._steps.move_to_end(key)
                    self._recordings[recording] = None
                    self._recordings.move_to_end(recording)
            if hit:
                ws, record, _ = hit
                steps.append(dict(record, id=raw.get("id"), cached=True,
                                  afterError=failed_before and not record["ok"]))
                failed_before = failed_before or not record["ok"]
                continue

            t0 = time.perf_counter()
            record = {"key": key, "type": raw.get("type"), "ok": True, "error": None}
            out = ws
            if btype is None:
                record.update(ok=False, error=f"unknown block '{raw.get('type')}'")
                res = Result()
            elif not enabled:
                res = Result(notes=["switched off - the data passes through unchanged"])
            else:
                work = ws.copy()
                try:
                    params = btype.parse(raw.get("params"))
                    res = btype.fn(work, params, None) or Result()
                    out = work
                except BlockError as exc:
                    record.update(ok=False, error=str(exc))
                    res = Result()
                except Exception as exc:                     # a bug, not the user's fault
                    record.update(ok=False, error=f"{type(exc).__name__}: {exc}")
                    record["trace"] = traceback.format_exc(limit=6)
                    res = Result()
            record.update(res.to_json())
            record["ms"] = round(1000.0 * (time.perf_counter() - t0), 1)
            record["signals"] = [s.info() for s in out.signals.values()]
            self._remember(key, out, record, recording)
            ws = out
            steps.append(dict(record, id=raw.get("id"), cached=False,
                              afterError=failed_before and not record["ok"]))
            failed_before = failed_before or not record["ok"]
        return steps

    # ---- reading results -------------------------------------------------------

    def workspace(self, key):
        with self._lock:
            hit = self._steps.get(key)
        if not hit:
            raise BlockError("these results are no longer cached - run the recipe again")
        return hit[0]

    def series(self, key, names, start=None, end=None, max_points=4000):
        ws = self.workspace(key)
        out = []
        for name in names:
            s = ws.signals.get(name)
            if s is None:
                continue
            t, v = s.t, s.v
            if s.axis == "time" and (start is not None or end is not None) and len(t):
                lo = 0 if start is None else int(np.searchsorted(t, start, side="left"))
                hi = len(t) if end is None else int(np.searchsorted(t, end, side="right"))
                lo, hi = max(0, lo - 1), min(len(t), hi + 1)
                t, v = t[lo:hi], v[lo:hi]
            idx = decimate(t, v, max_points)
            vv = v[idx]
            out.append({**s.info(), "x": np.round(t[idx], 6).tolist(),
                        "y": [None if not np.isfinite(a) else float(a) for a in vv],
                        "decimated": len(idx) < len(t)})
        return out

    def export_csv(self, key, names, rate=None):
        """CSV of signals: on the first signal's samples, or a common grid."""
        ws = self.workspace(key)
        sigs = [ws.signals[n] for n in names if n in ws.signals and ws.signals[n].axis == "time"]
        if not sigs:
            raise BlockError("no time signals selected")
        if rate:
            t0 = min(s.t[0] for s in sigs if s.n)
            t1 = max(s.t[-1] for s in sigs if s.n)
            grid = np.arange(t0, t1 + 0.5 / rate, 1.0 / rate)
        else:
            grid = sigs[0].t
        if len(grid) > EXPORT_MAX_ROWS:
            raise BlockError(f"{len(grid):,} rows - crop, or export at a lower rate")
        cols = [grid] + [s.v if (s is sigs[0] and not rate) else values_at(s, grid) for s in sigs]
        header = "time [s]," + ",".join(f"{s.name} [{s.unit}]" if s.unit else s.name for s in sigs)
        lines = [header]
        for row in np.column_stack(cols).tolist():
            lines.append(",".join("" if x != x else f"{x:.9g}" for x in row))
        return "\n".join(lines) + "\n"
