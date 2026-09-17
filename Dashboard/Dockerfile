# syntax=docker/dockerfile:1
#
# Pi-LOG Dashboard - frontend + API in one image.
#
#   docker compose up -d --build        (see DOCKER.md)
#
# Flight data is NOT baked in: it is mounted at runtime (a 20-hour recording is
# gigabytes, and flights keep being added). .dockerignore keeps Backend/data out
# of the build context for the same reason.

# ---- 1. build the frontend ---------------------------------------------------
FROM node:22-alpine AS frontend

WORKDIR /build/Frontend

# Dependencies first, so editing source does not re-download node_modules.
#
# Deliberately `npm install`, not `npm ci`, and no lockfile (.dockerignore drops
# it): the lockfile is generated on Windows and git-ignored in this repo. npm
# lockfiles made on one OS can omit the native binaries other platforms need -
# lightningcss, @tailwindcss/oxide, rollup and esbuild all ship per-platform
# builds - so `npm ci` in this Linux/musl container would fail to find them.
COPY Frontend/package.json ./
RUN npm install --no-audit --no-fund

COPY Frontend/ ./
RUN npm run build


# ---- 2. runtime ----------------------------------------------------------------
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PILOG_DATA_DIR=/data \
    PILOG_CACHE_DIR=/cache \
    PILOG_STATIC_DIR=/app/Frontend/dist

WORKDIR /app/Backend

COPY Backend/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY Backend/server.py Backend/telemetry_store.py ./
COPY --from=frontend /build/Frontend/dist /app/Frontend/dist

# Run unprivileged. /cache is created here so a fresh named volume inherits the
# ownership; /data is expected to be mounted read-only (the server never writes
# to it - column caches go to /cache).
RUN useradd --create-home --uid 1000 pilog \
    && mkdir -p /data /cache \
    && chown pilog:pilog /cache
USER pilog

EXPOSE 5000

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD python -c "import sys, urllib.request; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:5000/api/health', timeout=4).status == 200 else 1)"

# ONE worker with several threads, deliberately: cache-build progress and the
# query result cache live in process memory, and memmapped column files are
# shared per process. More workers would each build their own cache in parallel.
CMD ["gunicorn", "--bind", "0.0.0.0:5000", \
     "--workers", "1", "--threads", "8", \
     "--timeout", "120", \
     "--access-logfile", "-", \
     "server:app"]
