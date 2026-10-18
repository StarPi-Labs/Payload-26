# syntax=docker/dockerfile:1
#
# Pi-LOG Filter Lab - block editor + Python engine in one image.
#
#   cd FilterLab && docker compose up -d --build        (see README.md)
#
# The build context is the REPOSITORY ROOT, not this folder: the engine runs the
# team's own SD-Parser modules and the Dashboard's attitude.py, so they are copied
# in rather than duplicated. Dockerfile.dockerignore (next to this file) sends
# only those files - never the recordings.

# ---- 1. build the editor ----------------------------------------------------
FROM node:22-alpine AS frontend

WORKDIR /build

# As in Dashboard/Dockerfile: `npm install` without the (git-ignored, Windows-
# generated) lockfile, so Linux gets its own native binaries. The direct
# dependencies are pinned exactly in package.json.
COPY FilterLab/frontend/package.json ./
RUN npm install --no-audit --no-fund

COPY FilterLab/frontend/ ./
RUN npm run build


# ---- 2. runtime ---------------------------------------------------------------
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PILOG_RECORDINGS_DIR=/recordings \
    PILOG_DASHBOARD_DATA_DIR=/dashboard-data \
    PILOG_LAB_DATA_DIR=/lab-data \
    PILOG_LAB_STATIC_DIR=/app/frontend/dist \
    PILOG_SHARED_PATH=/app/shared

WORKDIR /app/backend

COPY FilterLab/backend/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY SD-Parser/frameparser.py SD-Parser/pilog_reader.py SD-Parser/kalman.py \
     SD-Parser/trajectory.py SD-Parser/noise_characterization.py \
     SD-Parser/imu_calibration.py Dashboard/Backend/attitude.py /app/shared/
COPY FilterLab/backend/ ./
COPY --from=frontend /build/dist /app/frontend/dist

# Unprivileged. Recordings and Dashboard data are mounted read-only; uploads,
# saved recipes and parse caches go to the /lab-data volume.
RUN useradd --create-home --uid 1000 pilog \
    && mkdir -p /recordings /dashboard-data /lab-data \
    && chown pilog:pilog /lab-data
USER pilog

EXPOSE 5050

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD python -c "import sys, urllib.request; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:5050/api/health', timeout=4).status == 200 else 1)"

# One worker: the step cache that makes the editor interactive lives in process
# memory. Long timeout: a full 20-hour recording can take minutes per block.
CMD ["gunicorn", "--bind", "0.0.0.0:5050", \
     "--workers", "1", "--threads", "4", \
     "--timeout", "600", \
     "--access-logfile", "-", \
     "lab_server:app"]
