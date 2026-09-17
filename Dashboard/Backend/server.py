"""
Pi-LOG Dashboard backend.

Serves the built frontend plus a small REST API over the flight folders in
DATA_DIR:

    data/<YYYY-MM-DD>/telemetry/*.txt|csv|log
    data/<YYYY-MM-DD>/videos/*.mp4|avi|mov|mkv

Large recordings are handled by telemetry_store.py: flights are listed from
header/tail reads only, telemetry is served from a per-file column cache, and
clients request a time window with a point budget instead of the whole file.
A single oversized or corrupt flight can no longer take the flight list down --
each flight is isolated and reported with its own status.

Configuration (environment variables, all optional):
    PILOG_DATA_DIR      flight data root              (default: ./data)
    PILOG_STATIC_DIR    built frontend                (default: ../Frontend/dist)
    PILOG_CACHE_DIR     put column caches here instead of next to the data
    PILOG_PREBUILD      "1" = start building caches for every flight at startup
    PILOG_MAX_POINTS    upper bound a client may request per query (default 200000)

Command line:
    python server.py                         run the development server
    python server.py --build-cache           build all caches now, then exit
    python server.py --build-cache 2026-09-16
"""
import argparse
import os
import re
import sys
from datetime import datetime

from flask import Flask, abort, jsonify, request, send_from_directory
from flask_cors import CORS

from telemetry_store import (TelemetryStore, columns_to_json, list_telemetry_files,
                             merge_meta)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.abspath(os.environ.get("PILOG_DATA_DIR", os.path.join(BASE_DIR, "data")))
STATIC_DIR = os.path.abspath(os.environ.get(
    "PILOG_STATIC_DIR", os.path.join(BASE_DIR, "..", "Frontend", "dist")))
MAX_POINTS_LIMIT = int(os.environ.get("PILOG_MAX_POINTS", "200000"))
DEFAULT_MAX_POINTS = 20000

ALLOWED_VIDEO_EXTENSIONS = {"mp4", "avi", "mov", "mkv"}
RAW_TELEMETRY_LIMIT_BYTES = 20 * 1024 * 1024
FLIGHT_ID_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

app = Flask(__name__, static_folder=STATIC_DIR)
CORS(app)

os.makedirs(DATA_DIR, exist_ok=True)
store = TelemetryStore(cache_root=os.environ.get("PILOG_CACHE_DIR") or None)


# ============================================================================
# Helpers
# ============================================================================

def allowed_file(filename, allowed_extensions):
    return "." in filename and filename.rsplit(".", 1)[1].lower() in allowed_extensions


def flight_dir_or_404(flight_id):
    """Resolve a flight id to its folder. Ids are dates, which also rules out
    path traversal ('..' etc.) - this server may be exposed on a network."""
    if not FLIGHT_ID_RE.match(flight_id or ""):
        abort(404)
    path = os.path.join(DATA_DIR, flight_id)
    if not os.path.isdir(path):
        abort(404)
    return path


def aggregate_status(statuses):
    """Combine per-file cache statuses into one flight-level status."""
    if not statuses:
        return {"state": "none"}
    states = [s["state"] for s in statuses]
    if "error" in states:
        return next(s for s in statuses if s["state"] == "error")
    if "building" in states:
        progress = sum(s.get("progress", 0.0) for s in statuses) / len(statuses)
        return {"state": "building", "progress": round(progress, 4)}
    if all(s == "ready" for s in states):
        return {"state": "ready", "progress": 1.0}
    return {"state": "missing", "progress": 0.0}


def get_flight_info(flight_folder):
    """Cheap per-flight summary: header metadata and tail reads only."""
    folder_name = os.path.basename(flight_folder)

    videos_dir = os.path.join(flight_folder, "videos")
    videos = []
    if os.path.isdir(videos_dir):
        videos = sorted(f for f in os.listdir(videos_dir)
                        if allowed_file(f, ALLOWED_VIDEO_EXTENSIONS))

    telemetry_paths = list_telemetry_files(flight_folder)
    infos = [store.file_info(p) for p in telemetry_paths]

    duration = 0.0
    for info in infos:
        if info["lastTime"] is not None:
            duration = max(duration, info["lastTime"])

    has_telemetry = bool(telemetry_paths)
    cache = aggregate_status([store.status(p) for p in telemetry_paths])

    return {
        "id": folder_name,
        "date": folder_name,
        "duration": duration,
        "status": ("success" if has_telemetry and videos
                   else "partial" if has_telemetry or videos else "pending"),
        "cameras": len(videos),
        "videos": videos,
        "telemetryFiles": [os.path.basename(p) for p in telemetry_paths],
        "telemetryBytes": sum(i["size"] for i in infos),
        "hasTelemetry": has_telemetry,
        "cache": cache,
    }


# ============================================================================
# Frontend
# ============================================================================

@app.route("/", defaults={"path": ""})
@app.route("/<path:path>")
def serve(path):
    if path.startswith("api/") or path.startswith("videos/"):
        abort(404)
    if not os.path.isdir(STATIC_DIR):
        return (f"Frontend not built: {STATIC_DIR} does not exist. "
                "Run 'npm run build' in Dashboard/Frontend.", 503)
    if path and os.path.exists(os.path.join(STATIC_DIR, path)):
        return send_from_directory(STATIC_DIR, path)
    return send_from_directory(STATIC_DIR, "index.html")


# ============================================================================
# Flights
# ============================================================================

@app.route("/api/flights", methods=["GET"])
def get_flights():
    """List every flight. Each flight is isolated: a broken one is reported
    with status 'error' and the rest are listed normally."""
    flights = []
    if os.path.isdir(DATA_DIR):
        for folder_name in sorted(os.listdir(DATA_DIR), reverse=True):
            folder_path = os.path.join(DATA_DIR, folder_name)
            if not os.path.isdir(folder_path) or not FLIGHT_ID_RE.match(folder_name):
                continue
            try:
                flights.append(get_flight_info(folder_path))
            except Exception as exc:
                app.logger.exception("flight %s could not be summarised", folder_name)
                flights.append({
                    "id": folder_name, "date": folder_name, "duration": 0,
                    "status": "error", "error": f"{type(exc).__name__}: {exc}",
                    "cameras": 0, "videos": [], "telemetryFiles": [],
                    "hasTelemetry": False, "cache": {"state": "error"},
                })
    return jsonify({"flights": flights})


@app.route("/api/flights/<flight_id>", methods=["GET"])
def get_flight(flight_id):
    return jsonify(get_flight_info(flight_dir_or_404(flight_id)))


# ============================================================================
# Videos
# ============================================================================

@app.route("/api/flights/<flight_id>/videos", methods=["GET"])
def list_videos(flight_id):
    videos_dir = os.path.join(flight_dir_or_404(flight_id), "videos")
    if not os.path.isdir(videos_dir):
        return jsonify({"videos": [], "urls": []})
    videos = sorted(f for f in os.listdir(videos_dir)
                    if allowed_file(f, ALLOWED_VIDEO_EXTENSIONS))
    return jsonify({
        "videos": videos,
        "urls": [f"/api/flights/{flight_id}/videos/{v}" for v in videos],
    })


@app.route("/api/flights/<flight_id>/videos/<filename>", methods=["GET"])
def serve_video(flight_id, filename):
    videos_dir = os.path.join(flight_dir_or_404(flight_id), "videos")
    return send_from_directory(videos_dir, filename)   # traversal-safe


# ============================================================================
# Telemetry
# ============================================================================

def _float_arg(name):
    value = request.args.get(name)
    if value is None or value == "":
        return None
    try:
        return float(value)
    except ValueError:
        abort(400, description=f"'{name}' must be a number")


@app.route("/api/flights/<flight_id>/telemetry/status", methods=["GET"])
def telemetry_status(flight_id):
    paths = list_telemetry_files(flight_dir_or_404(flight_id))
    return jsonify({"flight_id": flight_id,
                    "cache": aggregate_status([store.status(p) for p in paths])})


@app.route("/api/flights/<flight_id>/telemetry", methods=["GET"])
def get_telemetry(flight_id):
    """Telemetry for a time window, decimated to a point budget.

    Query parameters (all optional):
        start, end   seconds, in the file's own time base (not normalised)
        maxPoints    point budget, default 20000
        format       'columns' (default, compact) or 'rows' (legacy list of dicts)

    Returns 202 with progress while the column cache for a large file is still
    being built; the client should poll until it gets 200.
    """
    flight_dir = flight_dir_or_404(flight_id)
    paths = list_telemetry_files(flight_dir)
    if not paths:
        return jsonify({"flight_id": flight_id, "status": "ready", "columns": {},
                        "data": [], "meta": merge_meta([])})

    statuses = [store.ensure(p, background=True) for p in paths]
    overall = aggregate_status(statuses)
    if overall["state"] == "building":
        return jsonify({"flight_id": flight_id, "status": "processing",
                        "progress": overall.get("progress", 0.0)}), 202
    if overall["state"] == "error":
        return jsonify({"flight_id": flight_id, "status": "error",
                        "error": overall.get("error")}), 500

    start = _float_arg("start")
    end = _float_arg("end")
    try:
        max_points = int(request.args.get("maxPoints", DEFAULT_MAX_POINTS))
    except ValueError:
        abort(400, description="'maxPoints' must be an integer")
    max_points = max(100, min(max_points, MAX_POINTS_LIMIT))

    columns, qinfo = store.query(paths, start=start, end=end, max_points=max_points)

    infos = [store.file_info(p) for p in paths]
    meta = merge_meta(infos)
    manifests = [store._manifest(p) for p in paths]
    firsts = [m["timeFirst"] for m in manifests if m and m.get("timeFirst") is not None]
    lasts = [m["timeLast"] for m in manifests if m and m.get("timeLast") is not None]
    meta.update({
        "timeStart": min(firsts) if firsts else 0.0,
        "timeEnd": max(lasts) if lasts else 0.0,
        "totalRows": qinfo["totalRows"],
        "returnedRows": qinfo["returnedRows"],
        "decimated": qinfo["decimated"],
        "range": {"start": start, "end": end},
    })

    payload = {"flight_id": flight_id, "status": "ready", "meta": meta}
    json_cols = columns_to_json(columns)
    if request.args.get("format") == "rows":
        names = list(json_cols.keys())
        n = len(json_cols.get("time", []))
        payload["data"] = [{k: json_cols[k][i] for k in names} for i in range(n)]
    else:
        payload["columns"] = json_cols
    return jsonify(payload)


@app.route("/api/flights/<flight_id>/telemetry/raw", methods=["GET"])
def get_raw_telemetry(flight_id):
    """Raw file contents - small files only. Reading a multi-GB recording into
    one JSON response is exactly the failure this server was rebuilt to avoid."""
    files_data = []
    for path in list_telemetry_files(flight_dir_or_404(flight_id)):
        size = os.path.getsize(path)
        if size > RAW_TELEMETRY_LIMIT_BYTES:
            files_data.append({"filename": os.path.basename(path), "content": None,
                               "size": size, "error": "file too large for raw view"})
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            files_data.append({"filename": os.path.basename(path), "content": f.read(),
                               "size": size})
    return jsonify({"files": files_data})


# ============================================================================
# Health
# ============================================================================

@app.route("/api/health", methods=["GET"])
def health_check():
    return jsonify({
        "status": "healthy",
        "data_directory": DATA_DIR,
        "static_directory": STATIC_DIR,
        "frontend_built": os.path.isdir(STATIC_DIR),
        "timestamp": datetime.now().isoformat(),
    })


# ============================================================================
# Startup
# ============================================================================

def all_flight_dirs():
    if not os.path.isdir(DATA_DIR):
        return []
    return [os.path.join(DATA_DIR, d) for d in sorted(os.listdir(DATA_DIR))
            if FLIGHT_ID_RE.match(d) and os.path.isdir(os.path.join(DATA_DIR, d))]


def prebuild_in_background():
    """Kick off cache builds for every flight so the first click is instant."""
    for flight in all_flight_dirs():
        for path in list_telemetry_files(flight):
            try:
                store.ensure(path, background=True)
            except Exception:
                app.logger.exception("prebuild failed for %s", path)


def build_caches_cli(flight_ids):
    """Build caches synchronously with progress output (for --build-cache)."""
    import threading
    import time

    targets = []
    for flight in all_flight_dirs():
        if flight_ids and os.path.basename(flight) not in flight_ids:
            continue
        targets.extend(list_telemetry_files(flight))
    if not targets:
        print("No telemetry files found.")
        return 0

    failures = 0
    for path in targets:
        rel = os.path.relpath(path, DATA_DIR)
        size_mb = os.path.getsize(path) / 1e6
        if store.status(path)["state"] == "ready":
            print(f"[=] {rel} ({size_mb:.0f} MB) already cached")
            continue
        print(f"[+] {rel} ({size_mb:.0f} MB) building...")
        t0 = time.time()
        worker = threading.Thread(target=store.ensure, args=(path,), kwargs={"background": False})
        worker.start()
        while worker.is_alive():
            worker.join(2.0)
            st = store.status(path)
            if st["state"] == "building":
                print(f"\r    {st['progress'] * 100:5.1f}%", end="", flush=True)
        st = store.status(path)
        if st["state"] == "ready":
            print(f"\r    done in {time.time() - t0:.1f}s")
        else:
            failures += 1
            print(f"\r    FAILED: {st.get('error')}")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description="Pi-LOG Dashboard server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--build-cache", nargs="*", metavar="FLIGHT_ID",
                        help="build column caches (all flights, or the given ids) and exit")
    args = parser.parse_args()

    if args.build_cache is not None:
        sys.exit(build_caches_cli(set(args.build_cache)))

    print(f"Data directory:   {DATA_DIR}")
    print(f"Static directory: {STATIC_DIR}")
    if os.environ.get("PILOG_PREBUILD") == "1":
        prebuild_in_background()
    # The reloader would start a second process and a second set of cache
    # builds; keep it off unless explicitly debugging.
    app.run(debug=args.debug, use_reloader=False, host=args.host, port=args.port,
            threaded=True)


if os.environ.get("PILOG_PREBUILD") == "1" and __name__ != "__main__":
    # Under gunicorn the module is imported rather than run.
    prebuild_in_background()

if __name__ == "__main__":
    main()
