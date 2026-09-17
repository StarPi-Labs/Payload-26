# Running the Dashboard in Docker

One container serves both the frontend and the API. Flight data is mounted,
never baked into the image.

## Quick start (on any machine with Docker)

```bash
cd Dashboard
docker compose up -d --build
```

Open `http://<host>:5000`. First start builds the frontend inside the image
(a few minutes); later rebuilds reuse the cached layers.

## Deploying to your Docker server

### 1. Get the code there

Either clone the repo on the server, or copy just the `Dashboard/` folder — it
is self-contained. The frontend lockfile is not needed; the image installs
dependencies itself (see the comment in the `Dockerfile` for why).

### 2. Get the flights there

The container expects the usual layout: `<data>/<YYYY-MM-DD>/telemetry/…` and
`…/videos/…`. Copy your data folder to the server, skipping local caches:

```bash
rsync -av --progress --exclude '.cache/' Dashboard/Backend/data/ user@server:/srv/pilog/data/
```

Then point the container at it with a `.env` file next to `docker-compose.yml`:

```ini
PILOG_DATA_PATH=/srv/pilog/data
PILOG_PORT=5000
```

The data is mounted **read-only**. The server never writes to your recordings;
its column caches live in the `pilog-cache` named volume instead.

### 3. Start it

```bash
docker compose up -d --build
docker compose logs -f
```

### Building elsewhere instead

If the server is slow or has no internet access, build on your PC and ship the
image:

```bash
docker build -t pilog-dashboard:latest Dashboard
docker save pilog-dashboard:latest | ssh user@server docker load
```

On the server, remove the `build:` line from `docker-compose.yml` and run
`docker compose up -d`.

**Check the CPU architecture first.** An image built on an x86 PC will not run
on an ARM server (Raspberry Pi, many NAS boxes). Either build on the server, or
cross-build:

```bash
docker buildx build --platform linux/arm64 -t pilog-dashboard:latest --load Dashboard
```

## Large recordings

A multi-hour recording is converted once into a column cache (~30 % of the
telemetry size). With `PILOG_PREBUILD=1` — the default in `docker-compose.yml`
— that happens for every flight at container start, so the browser never waits.

For reference, the 3.1 GB / 20-hour recording took **26 s** on a desktop PC.
Expect several times that on a NAS or a Raspberry Pi. Until a cache is ready the
dashboard shows a progress bar for that flight; every other flight stays usable.

To (re)build caches manually, e.g. right after copying in a new flight:

```bash
docker compose exec dashboard python server.py --build-cache             # all flights
docker compose exec dashboard python server.py --build-cache 2026-09-16  # one flight
```

Caches are invalidated automatically when a telemetry file changes. To discard
them all:

```bash
docker compose down
docker volume rm dashboard_pilog-cache
```

## Updating

```bash
git pull
docker compose up -d --build
```

## Security

**The dashboard has no authentication.** Anyone who can reach the port can view
every flight. That is fine on a private LAN or behind a VPN (e.g. Tailscale).
Do not publish the port to the internet directly — put a reverse proxy with
authentication in front of it instead.

## Configuration reference

| Variable | Default in image | Meaning |
|---|---|---|
| `PILOG_DATA_DIR` | `/data` | Flight data root inside the container |
| `PILOG_CACHE_DIR` | `/cache` | Where column caches are written |
| `PILOG_STATIC_DIR` | `/app/Frontend/dist` | Built frontend |
| `PILOG_PREBUILD` | unset (`1` in compose) | Build all caches at startup |
| `PILOG_MAX_POINTS` | `200000` | Upper bound a client may request per query |

Compose-level (`.env`): `PILOG_DATA_PATH` (host data folder), `PILOG_PORT` (host port).
