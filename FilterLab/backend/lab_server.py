"""
Pi-LOG Filter Lab server: the block catalog, recordings, recipe runs and
plot data for the editor in FilterLab/frontend.

    python lab_server.py                  development server on :5050
    gunicorn lab_server:app               production (the Docker image)

Configuration (environment, all optional):
    PILOG_RECORDINGS_DIR      .bin/.json recordings to offer      (default: ../../SD-Parser)
    PILOG_DASHBOARD_DATA_DIR  the Dashboard's flight folders      (default: ../../Dashboard/Backend/data)
    PILOG_LAB_DATA_DIR        writable: uploads, saved recipes, caches  (default: ../lab-data)
    PILOG_LAB_STATIC_DIR      built editor                        (default: ../frontend/dist)
    PILOG_SHARED_PATH         where SD-Parser's modules and attitude.py are
"""
import argparse
import json
import os
import re

from flask import Flask, Response, abort, jsonify, request, send_from_directory

from lab import sources
from lab.catalog import catalog_json
from lab.engine import Engine
from lab.presets import PRESETS
from lab.signals import BlockError

BASE = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.abspath(os.environ.get("PILOG_LAB_STATIC_DIR",
                                            os.path.join(BASE, "..", "frontend", "dist")))
RECIPE_DIR = os.path.join(sources.LAB_DATA_DIR, "recipes")
RECIPE_NAME_RE = re.compile(r"^[A-Za-z0-9 _.-]{1,80}$")
MAX_UPLOAD_BYTES = 1024 * 1024 * 1024

app = Flask(__name__, static_folder=None)
app.config["MAX_CONTENT_LENGTH"] = MAX_UPLOAD_BYTES
engine = Engine()


@app.errorhandler(BlockError)
def block_error(exc):
    return jsonify({"error": str(exc)}), 400


def body():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        abort(400, description="expected a JSON object")
    return data


# ---- editor ------------------------------------------------------------------

@app.route("/", defaults={"path": ""})
@app.route("/<path:path>")
def static_files(path):
    if path.startswith("api/"):
        abort(404)
    if not os.path.isdir(STATIC_DIR):
        return (f"Editor not built: {STATIC_DIR} does not exist. "
                "Run 'npm install' and 'npm run build' in FilterLab/frontend.", 503)
    if path and os.path.isfile(os.path.join(STATIC_DIR, path)):
        return send_from_directory(STATIC_DIR, path)
    return send_from_directory(STATIC_DIR, "index.html")


# ---- catalog, presets, recordings -------------------------------------------------

@app.get("/api/health")
def health():
    return jsonify({"status": "ok", "recordings": sources.RECORDINGS_DIR,
                    "dashboard": sources.DASHBOARD_DATA_DIR, "labData": sources.LAB_DATA_DIR})


@app.get("/api/catalog")
def catalog():
    return jsonify(catalog_json())


@app.get("/api/presets")
def presets():
    return jsonify({"presets": PRESETS})


@app.get("/api/recordings")
def recordings():
    items = [{k: v for k, v in r.items() if k != "path"} for r in sources.list_recordings()]
    return jsonify({"recordings": items})


@app.post("/api/recordings")
def upload():
    f = request.files.get("file")
    if f is None or not f.filename:
        abort(400, description="no file")
    try:
        name = sources.safe_upload_name(f.filename)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    os.makedirs(sources.UPLOAD_DIR, exist_ok=True)
    stem, ext = os.path.splitext(name)
    target, n = os.path.join(sources.UPLOAD_DIR, name), 2
    while os.path.exists(target):
        target = os.path.join(sources.UPLOAD_DIR, f"{stem}_{n}{ext}")
        n += 1
    f.save(target)
    return jsonify({"id": "upload/" + os.path.basename(target)})


# ---- recipes -------------------------------------------------------------------

def recipe_path(name):
    if not RECIPE_NAME_RE.match(name or "") or name.startswith("."):
        abort(400, description="recipe names may use letters, digits, spaces, _ . -")
    return os.path.join(RECIPE_DIR, name + ".json")


@app.get("/api/recipes")
def list_recipes():
    out = []
    if os.path.isdir(RECIPE_DIR):
        for f in sorted(os.listdir(RECIPE_DIR)):
            if f.endswith(".json"):
                p = os.path.join(RECIPE_DIR, f)
                out.append({"name": f[:-5], "modified": os.path.getmtime(p)})
    return jsonify({"recipes": out})


@app.get("/api/recipes/<name>")
def get_recipe(name):
    p = recipe_path(name)
    if not os.path.isfile(p):
        abort(404)
    with open(p, "r", encoding="utf-8") as f:
        return jsonify(json.load(f))


@app.put("/api/recipes/<name>")
def save_recipe(name):
    p = recipe_path(name)
    data = body()
    os.makedirs(RECIPE_DIR, exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return jsonify({"saved": name})


@app.delete("/api/recipes/<name>")
def delete_recipe(name):
    p = recipe_path(name)
    if os.path.isfile(p):
        os.remove(p)
    return jsonify({"deleted": name})


# ---- running ---------------------------------------------------------------------

@app.post("/api/run")
def run():
    blocks = body().get("blocks")
    if not isinstance(blocks, list):
        abort(400, description="expected {blocks: [...]}")
    return jsonify({"steps": engine.run(blocks)})


@app.post("/api/series")
def series():
    data = body()
    names = data.get("names") or []
    max_points = max(100, min(int(data.get("maxPoints", 4000)), 50000))
    out = {"series": engine.series(data.get("key", ""), names, data.get("start"), data.get("end"),
                                   max_points)}
    if data.get("beforeKey"):
        out["before"] = engine.series(data["beforeKey"], names, data.get("start"), data.get("end"),
                                      max_points)
    return jsonify(out)


@app.post("/api/export")
def export():
    data = body()
    rate = data.get("rate")
    csv = engine.export_csv(data.get("key", ""), data.get("names") or [],
                            float(rate) if rate else None)
    return Response(csv, mimetype="text/csv",
                    headers={"Content-Disposition": "attachment; filename=pilog-lab.csv"})


def main():
    ap = argparse.ArgumentParser(description="Pi-LOG Filter Lab server")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5050)
    args = ap.parse_args()
    print(f"Recordings:   {sources.RECORDINGS_DIR}")
    print(f"Dashboard:    {sources.DASHBOARD_DATA_DIR}")
    print(f"Lab data:     {sources.LAB_DATA_DIR}")
    print(f"Editor:       {STATIC_DIR}")
    app.run(host=args.host, port=args.port, threaded=True, use_reloader=False)


if __name__ == "__main__":
    main()
