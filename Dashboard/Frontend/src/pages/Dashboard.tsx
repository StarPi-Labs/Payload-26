import { Component, createMemo, createSignal, onCleanup, onMount, Show } from "solid-js";

import { AtmosphericSample } from "../models/atmospheric-sample";
import { FlightSummary } from "../models/ui/flight-selector-props";
import { ModeTransition } from "../models/ui/timeline-scrubber-props";
import {
    SeriesWindow, catmullRom, segmentIndex, skipStepFor, speedOptionsFor, targetResolution,
} from "../utils/telemetry-series";

import AttitudeCard from "../components/AttitudeCard";
import AtmosphereCard from "../components/AtmosphereCard";
import NavigationCard from "../components/base/NavigationCard";
import VelocityGraphCard from "../components/VelocityGraphCard";
import AltitudeGraphCard from "../components/AltitudeGraphCard";
import AccellerationGraphCard from "../components/AccellerationGraphCard";
import AltitudeTracker from "../components/AltitudeTracker";
import VideoPlayer from "../components/VideoPlayer";
import RocketMapCard from "../components/RocketMapCard"
import PowerGraphCard from "../components/PowerGraphCard"
import FlightSelector from "../components/base/FlightSelector";
import TimelineScrubber from "../components/base/TimelineScrubber";
import { VideoSource } from "../models/videp-source";

/*
 * Telemetry loading scheme
 * ------------------------
 * A recording can be far too long to hold at full resolution in the browser
 * (20 hours at ~120 rows/s is ~9 million rows). So the dashboard keeps:
 *
 *   overview  the whole flight at up to OVERVIEW_POINTS rows. The server picks
 *             those rows so each bucket's altitude/acceleration/velocity
 *             extremes survive - apogee and peak-g are never decimated away.
 *
 *   detail    a higher-resolution window around the playhead, fetched on
 *             demand. How fine it needs to be depends on playback speed: at
 *             60 fps each frame advances speed/60 s of flight, so detail finer
 *             than that can never be seen.
 *
 * Short flights fit entirely in the overview, so they never fetch detail.
 */
const OVERVIEW_POINTS = 20000;
const DETAIL_POINTS = 20000;
/** Overview/detail counts as fine enough within this factor of what playback needs. */
const DETAIL_SLACK = 1.5;
const PROCESSING_POLL_MS = 1500;
const DETAIL_CHECK_MS = 200;
const DETAIL_RETRY_MS = 2000;

const delay = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

const Dashboard: Component = () => {

    const emptySample: AtmosphericSample = {
        ts: Date.now(),
        roll: 0,
        pitch: 0,
        yaw: 0,
        status: false,

        alt: 0,
        altMsl: 0,
        galt: 0,
        vvel: 0,
        hvel: 0,
        lat: 0,
        long: 0,
        gps: false,
        temp: 0,
        pres: 0,
        rh: 0,
        accelX: 0,
        accelY: 0,
        accelZ: 0,
        gasRes: 0,
        busVolt: 0,
        current: 0,
        power: 0,
    };

    const [sample, setSample] = createSignal<AtmosphericSample>(emptySample);
    const [targetAltitude, setTargetAltitude] = createSignal(1200);
    const [departureAltitude, setDepartureAltitude] = createSignal(0);
    const [videoSources, setVideoSources] = createSignal<VideoSource[]>([]);
    const [t0EpochMs, setT0EpochMs] = createSignal<number | null>(null);

    const [flights, setFlights] = createSignal<FlightSummary[]>([]);
    const [activeFlightId, setActiveFlightId] = createSignal<string | null>(null);

    const [elapsedSec, setElapsedSec] = createSignal(0);
    const [durationSec, setDurationSec] = createSignal(0);
    const [isPlaying, setIsPlaying] = createSignal(false);
    const [playbackSpeed, setPlaybackSpeed] = createSignal(1);
    const [modeTransitions, setModeTransitions] = createSignal<ModeTransition[]>([]);
    // The series live in plain variables (not signals) so JSX can't react to
    // them -- this signal is the reactive proxy for "is there enough data to
    // scrub/play", kept in sync wherever the overview is replaced.
    const [canPlay, setCanPlay] = createSignal(false);
    // Bumped on every seek/flight switch so the rolling graphs drop their
    // buffered points instead of drawing a line back across the jump.
    const [graphResetKey, setGraphResetKey] = createSignal(0);
    /** Non-null while the server is still building the cache for a large recording. */
    const [processing, setProcessing] = createSignal<{ progress: number } | null>(null);
    const [loadError, setLoadError] = createSignal<string | null>(null);

    const speedOptions = createMemo(() => speedOptionsFor(durationSec()));
    const skipStep = createMemo(() => skipStepFor(durationSec()));

    let playbackRaf: number | undefined = undefined;
    let playAnchorPerf = 0;   // performance.now() when playback last (re)started
    let playAnchorSec = 0;    // elapsedSec() value at that anchor

    let overview: SeriesWindow<AtmosphericSample> | null = null;
    let detail: SeriesWindow<AtmosphericSample> | null = null;
    /** Raw file time of the flight's first row; every window is normalised by it. */
    let flightTimeStart = 0;
    /** Finest spacing worth fetching: the recording's own row density. */
    let nativeSpacing = 0.01;

    /** Bumped on every flight switch; responses carrying an older token are dropped. */
    let loadToken = 0;
    let detailAbort: AbortController | null = null;
    let detailInFlight: { start: number; end: number; token: number } | null = null;
    let lastDetailCheck = 0;
    let lastDetailFailure = 0;

    const numericFields: Array<keyof AtmosphericSample> = [
        "roll", "pitch", "yaw",
        "alt", "altMsl", "galt", "vvel", "hvel",
        "lat", "long",
        "temp", "pres", "rh",
        "accelX", "accelY", "accelZ",
        "gasRes", "busVolt", "current", "power",
    ];

    function mapTelemetryPointToSample(pt: any): AtmosphericSample {
        const gpsFromPoint = typeof pt.gps === "boolean"
            ? pt.gps
            : Boolean(pt.gpsLat ?? pt.lat ?? 0) || Boolean(pt.gpsLon ?? pt.long ?? 0);

        return {
            ts: Date.now(),
            roll: pt.roll ?? pt.pitch ?? 0,
            pitch: pt.pitch ?? 0,
            yaw: pt.yaw ?? 0,
            status: pt.status ?? true,

            alt: pt.altitude ?? pt.alt ?? 0,
            altMsl: pt.altitudeMSL ?? pt.altMsl ?? 0,
            galt: pt.gpsAlt ?? pt.galt ?? 0,
            vvel: pt.velocity ?? pt.vvel ?? 0,
            hvel: pt.horizontalVelocity ?? pt.hvel ?? 0,
            lat: pt.gpsLat ?? pt.lat ?? 0,
            long: pt.gpsLon ?? pt.long ?? 0,
            gps: gpsFromPoint,
            temp: pt.temperature ?? pt.temp ?? 0,
            pres: pt.pressure ?? pt.pres ?? 0,
            rh: pt.humidity ?? pt.rh ?? 0,
            accelX: pt.accelerationX ?? pt.accelX ?? 0,
            accelY: pt.accelerationY ?? pt.accelY ?? 0,
            accelZ: pt.accelerationZ ?? pt.accelZ ?? 0,
            gasRes: pt.gasResistance ?? pt.gasRes ?? 0,
            busVolt: pt.busVoltage ?? pt.busVolt ?? 0,
            current: pt.current ?? 0,
            power: pt.power ?? 0,
        };
    }

    function interpolateSampleCurve(
        prev: AtmosphericSample,
        current: AtmosphericSample,
        next: AtmosphericSample,
        next2: AtmosphericSample,
        progress: number,
        playbackTs: number,
    ): AtmosphericSample {
        const p = Math.min(1, Math.max(0, progress));
        const out: AtmosphericSample = {
            ...current,
            ts: playbackTs,
            gps: current.gps,
            status: current.status,
        };

        for (const key of numericFields) {
            const p0 = Number(prev[key] ?? 0);
            const p1 = Number(current[key] ?? 0);
            const p2 = Number(next[key] ?? 0);
            const p3 = Number(next2[key] ?? 0);
            (out[key] as number) = catmullRom(p0, p1, p2, p3, p);
        }

        return out;
    }

    function getPointTimeSeconds(pt: any, fallbackIndex: number): number {
        const t = Number(pt?.time);
        if (Number.isFinite(t) && t >= 0) return t;
        return fallbackIndex * 0.1;
    }

    // ---- series construction ------------------------------------------------

    /** First raw timestamp in a telemetry response, for servers without meta.timeStart. */
    function firstRawTime(body: any): number {
        if (Array.isArray(body?.columns?.time) && body.columns.time.length) {
            return Number(body.columns.time[0]) || 0;
        }
        if (Array.isArray(body?.data) && body.data.length) {
            return getPointTimeSeconds(body.data[0], 0);
        }
        return 0;
    }

    /** Turn a telemetry response (columns, or legacy rows) into a window. */
    function buildWindow(body: any): SeriesWindow<AtmosphericSample> {
        const cols = body?.columns;
        let rawTimes: number[];
        let samples: AtmosphericSample[];

        if (cols && Array.isArray(cols.time)) {
            const names = Object.keys(cols);
            const n = cols.time.length;
            rawTimes = new Array(n);
            samples = new Array(n);
            for (let i = 0; i < n; i++) {
                const pt: Record<string, any> = {};
                for (const k of names) pt[k] = cols[k][i];
                samples[i] = mapTelemetryPointToSample(pt);
                rawTimes[i] = Number(cols.time[i]);
            }
        } else {
            const data: any[] = Array.isArray(body?.data) ? body.data : [];
            samples = data.map((pt) => mapTelemetryPointToSample(pt));
            rawTimes = data.map((pt, i) => getPointTimeSeconds(pt, i));
        }

        const times = rawTimes.map((t) => t - flightTimeStart);
        const n = times.length;
        const start = n ? times[0] : 0;
        const end = n ? times[n - 1] : 0;
        // Average spacing, not median: the server's extreme-preserving picks
        // cluster, so a median would overstate a decimated window's resolution.
        const spacing = n > 1 ? (end - start) / (n - 1) : Infinity;
        return { times, samples, start, end, spacing };
    }

    function applyMeta(meta: any) {
        const targetFromMeta = Number(meta?.targetAltitude);
        const departureFromMeta = Number(meta?.departureAltitude);
        const t0EpochFromMeta = Number(meta?.t0EpochMs);
        const t0IsoFromMeta = typeof meta?.t0 === "string" ? meta.t0 : null;

        if (Number.isFinite(t0EpochFromMeta) && t0EpochFromMeta > 0) {
            setT0EpochMs(t0EpochFromMeta);
        } else if (t0IsoFromMeta) {
            const parsed = Date.parse(t0IsoFromMeta);
            setT0EpochMs(Number.isFinite(parsed) ? parsed : null);
        } else {
            setT0EpochMs(null);
        }

        if (Number.isFinite(targetFromMeta) && targetFromMeta > 0) {
            setTargetAltitude(targetFromMeta);
        }
        // 0 is a valid departure altitude (sea-level launch), unlike target.
        setDepartureAltitude(Number.isFinite(departureFromMeta) ? departureFromMeta : 0);

        const transitions: ModeTransition[] = Array.isArray(meta?.modeTransitions)
            ? meta.modeTransitions
                .map((t: any) => ({ time: Number(t.time) - flightTimeStart, mode: String(t.mode) }))
                .filter((t: ModeTransition) => Number.isFinite(t.time))
            : [];
        setModeTransitions(transitions);
    }

    // ---- sampling -----------------------------------------------------------

    /** Detail when it covers t, otherwise the overview. */
    function windowFor(t: number): SeriesWindow<AtmosphericSample> | null {
        if (detail && detail.samples.length >= 2 && t >= detail.start && t <= detail.end) {
            return detail;
        }
        return overview;
    }

    function computeSampleAtTime(tSec: number): AtmosphericSample {
        const win = windowFor(tSec);
        if (!win || win.samples.length === 0) return emptySample;
        if (win.samples.length === 1) return win.samples[0];

        const times = win.times;
        const clamped = Math.min(times[times.length - 1], Math.max(times[0], tSec));
        const i = segmentIndex(times, clamped);
        const t0 = times[i];
        const t1 = times[i + 1];
        const denom = Math.max(0.000001, t1 - t0);
        const u = (clamped - t0) / denom;

        const prev = win.samples[Math.max(0, i - 1)];
        const current = win.samples[i];
        const next = win.samples[i + 1];
        const next2 = win.samples[Math.min(win.samples.length - 1, i + 2)];

        const epoch = t0EpochMs() ?? Date.now();
        return interpolateSampleCurve(prev, current, next, next2, u, epoch + clamped * 1000);
    }

    // ---- detail windows -------------------------------------------------------

    /** Fetch a detail window if the playhead needs finer data than is loaded. */
    function maybeFetchDetail(t: number) {
        const flightId = activeFlightId();
        if (!overview || !flightId) return;

        const need = targetResolution(playbackSpeed(), nativeSpacing);
        if (overview.spacing <= need * DETAIL_SLACK) return;          // overview suffices

        const covered = !!detail && t >= detail.start && t <= detail.end
            && detail.spacing <= need * DETAIL_SLACK;
        // While playing, fetch the next window before running off this one.
        const nearEnd = covered && isPlaying() && !!detail
            && t > detail.start + 0.75 * (detail.end - detail.start);
        if (covered && !nearEnd) return;

        if (detailInFlight && detailInFlight.token === loadToken
            && t >= detailInFlight.start && t <= detailInFlight.end) {
            return;                                                    // one is already coming
        }
        if (performance.now() - lastDetailFailure < DETAIL_RETRY_MS) return;

        const dur = durationSec();
        const span = Math.min(dur, Math.max(need * DETAIL_POINTS, 10));
        // Mostly ahead of the playhead when playing, centred when paused.
        const behind = isPlaying() ? 0.05 : 0.5;
        let start = t - span * behind;
        let end = start + span;
        if (start < 0) { end -= start; start = 0; }
        if (end > dur) { start = Math.max(0, start - (end - dur)); end = dur; }

        void fetchDetail(flightId, start, end);
    }

    async function fetchDetail(flightId: string, start: number, end: number) {
        const token = loadToken;
        detailAbort?.abort();
        const ctrl = new AbortController();
        detailAbort = ctrl;
        detailInFlight = { start, end, token };

        const rawStart = (start + flightTimeStart).toFixed(3);
        const rawEnd = (end + flightTimeStart).toFixed(3);
        try {
            const res = await fetch(
                `/api/flights/${flightId}/telemetry?start=${rawStart}&end=${rawEnd}&maxPoints=${DETAIL_POINTS}`,
                { signal: ctrl.signal },
            );
            if (token !== loadToken || ctrl.signal.aborted) return;
            if (res.status !== 200) {
                lastDetailFailure = performance.now();
                return;                           // overview keeps playback going
            }
            const win = buildWindow(await res.json());
            if (token !== loadToken || ctrl.signal.aborted) return;
            if (win.samples.length >= 2) {
                detail = win;
                // Paused on a spot that just got sharper: show the better value.
                if (!isPlaying()) setSample(computeSampleAtTime(elapsedSec()));
            }
        } catch (e) {
            if (!ctrl.signal.aborted) lastDetailFailure = performance.now();
        } finally {
            if (detailInFlight && detailInFlight.token === token
                && detailInFlight.start === start && detailInFlight.end === end) {
                detailInFlight = null;
            }
        }
    }

    // ---- playback -------------------------------------------------------------

    function clearPlaybackTimers() {
        if (playbackRaf) {
            cancelAnimationFrame(playbackRaf);
            playbackRaf = undefined;
        }
    }

    function tick(now: number) {
        if (!isPlaying()) {
            playbackRaf = undefined;
            return;
        }
        const t = playAnchorSec + ((now - playAnchorPerf) / 1000) * playbackSpeed();
        const end = durationSec();

        if (t >= end) {
            setElapsedSec(end);
            setSample(computeSampleAtTime(end));
            setIsPlaying(false);
            playbackRaf = undefined;
            return;
        }

        setElapsedSec(t);
        setSample(computeSampleAtTime(t));
        if (now - lastDetailCheck > DETAIL_CHECK_MS) {
            lastDetailCheck = now;
            maybeFetchDetail(t);
        }
        playbackRaf = requestAnimationFrame(tick);
    }

    function play() {
        if (!canPlay()) return;
        // Restart from the beginning if playback had already reached the end.
        if (elapsedSec() >= durationSec()) {
            setElapsedSec(0);
        }
        playAnchorPerf = performance.now();
        playAnchorSec = elapsedSec();
        setIsPlaying(true);
        clearPlaybackTimers();
        maybeFetchDetail(elapsedSec());
        playbackRaf = requestAnimationFrame(tick);
    }

    function pause() {
        setIsPlaying(false);
        clearPlaybackTimers();
    }

    function togglePlayPause() {
        if (isPlaying()) pause(); else play();
    }

    function seek(tSec: number) {
        const clamped = Math.min(durationSec(), Math.max(0, tSec));
        setElapsedSec(clamped);
        setSample(computeSampleAtTime(clamped));
        setGraphResetKey(k => k + 1);
        if (isPlaying()) {
            // Keep playing, just from the new position.
            playAnchorPerf = performance.now();
            playAnchorSec = clamped;
        }
        maybeFetchDetail(clamped);
    }

    function skip(deltaSec: number) {
        seek(elapsedSec() + deltaSec);
    }

    function changeSpeed(newSpeed: number) {
        // Re-anchor from the current position so the rate change takes
        // effect immediately instead of retroactively over the old anchor.
        if (isPlaying()) {
            playAnchorPerf = performance.now();
            playAnchorSec = elapsedSec();
        }
        setPlaybackSpeed(newSpeed);
        // Slower playback may need finer data than is currently loaded.
        maybeFetchDetail(elapsedSec());
    }

    // ---- loading ----------------------------------------------------------------

    async function refreshFlights(): Promise<FlightSummary[]> {
        try {
            const res = await fetch('/api/flights');
            if (!res.ok) return flights();
            const json = await res.json();
            const list: FlightSummary[] = json.flights || [];
            setFlights(list);
            return list;
        } catch (e) {
            return flights();
        }
    }

    async function loadTelemetryForFlight(flightId: string, token: number) {
        let waited = false;
        while (token === loadToken) {
            let res: Response | null = null;
            let body: any = null;
            try {
                res = await fetch(`/api/flights/${flightId}/telemetry?maxPoints=${OVERVIEW_POINTS}`);
                try { body = await res.json(); } catch (e) { body = null; }
            } catch (e) {
                res = null;
            }
            if (token !== loadToken) return;
            if (!res) {
                setLoadError("Could not reach the server.");
                return;
            }

            if (res.status === 202) {
                // Large recording: the server is building its cache. Poll.
                waited = true;
                setProcessing({ progress: Number(body?.progress) || 0 });
                await delay(PROCESSING_POLL_MS);
                continue;
            }
            setProcessing(null);

            if (res.status !== 200 || !body) {
                setLoadError(body?.error
                    ? `Telemetry unavailable: ${body.error}`
                    : `Telemetry unavailable (HTTP ${res.status}).`);
                return;
            }

            const meta = body.meta ?? {};
            flightTimeStart = Number.isFinite(Number(meta.timeStart))
                ? Number(meta.timeStart) : firstRawTime(body);
            applyMeta(meta);

            overview = buildWindow(body);

            const timeEnd = Number(meta.timeEnd);
            const duration = Number.isFinite(timeEnd)
                ? Math.max(0, timeEnd - flightTimeStart)
                : (overview.times.length ? overview.times[overview.times.length - 1] : 0);
            setDurationSec(duration);

            const totalRows = Number(meta.totalRows);
            nativeSpacing = Number.isFinite(totalRows) && totalRows > 1 && duration > 0
                ? Math.max(0.001, duration / totalRows)
                : Math.max(0.001, overview.spacing);

            setCanPlay(overview.samples.length >= 2);

            // The selector showed "preparing"; refresh it now that it is ready.
            if (waited) void refreshFlights();
            return;
        }
    }

    async function loadVideosForFlight(flightId: string) {
        try {
            const res = await fetch(`/api/flights/${flightId}/videos`);
            if (!res.ok) return;
            const json = await res.json();
            const urls: string[] = json.urls || [];
            const videos: string[] = json.videos || [];

            const mapped: VideoSource[] = urls.map((url, i) => ({
                id: `cam${i + 1}`,
                label: videos[i] || `Camera ${i + 1}`,
                src: url,
            }));

            setVideoSources(mapped);
        } catch (e) {
            // ignore network errors silently for now
        }
    }

    async function selectFlight(flightId: string) {
        if (!flightId || flightId === activeFlightId()) return;
        pause();

        loadToken++;
        const token = loadToken;
        detailAbort?.abort();
        detailAbort = null;
        detailInFlight = null;
        overview = null;
        detail = null;

        setActiveFlightId(flightId);
        setVideoSources([]);
        setElapsedSec(0);
        setDurationSec(0);
        setModeTransitions([]);
        setDepartureAltitude(0);
        setCanPlay(false);
        setProcessing(null);
        setLoadError(null);
        // Speed options depend on flight length; 1000x on a 2-minute flight is nonsense.
        setPlaybackSpeed(1);
        setSample(emptySample);
        setGraphResetKey(k => k + 1);

        await Promise.all([
            loadVideosForFlight(flightId),
            loadTelemetryForFlight(flightId, token),
        ]);
        if (token !== loadToken) return;

        // Land on the first frame, paused -- let the user press Play or drag
        // the scrubber rather than immediately replaying the whole flight.
        if (overview && overview.samples.length) {
            setSample(computeSampleAtTime(0));
            maybeFetchDetail(0);
        }
    }

    onMount(async () => {
        const list = await refreshFlights();
        // Newest first; skip anything the server could not read at all.
        const first = list.find((f) => f.status !== "error");
        if (first) await selectFlight(first.id);
    });

    onCleanup(() => {
        clearPlaybackTimers();
        loadToken++;            // stops any processing poll loop
        detailAbort?.abort();
    });

    const timestampLabel = () => {
        const value = sample().ts;
        return new Date(value).toLocaleTimeString();
    };

    return (
        <div class="space-y-6">
            <div class="flex flex-col gap-4">
                <div class="flex flex-wrap items-center justify-between gap-4">
                    <h1 class="text-2xl font-semibold">Dashboard</h1>
                    <FlightSelector
                        flights={flights()}
                        selectedId={activeFlightId()}
                        onSelect={selectFlight}
                    />
                </div>

                <div class="card bg-base-200/70 border border-base-300 shadow-sm">
                    <div class="card-body py-3 gap-3">
                        <Show when={processing()}>
                            {(p) => (
                                <div class="flex items-center gap-3 text-sm">
                                    <span class="loading loading-spinner loading-sm" />
                                    <span class="whitespace-nowrap">
                                        Preparing recording for playback… {Math.round(p().progress * 100)}%
                                    </span>
                                    <progress
                                        class="progress progress-primary flex-1"
                                        value={p().progress * 100}
                                        max="100"
                                    />
                                </div>
                            )}
                        </Show>
                        <Show when={loadError()}>
                            {(msg) => <div class="alert alert-error py-2 text-sm">{msg()}</div>}
                        </Show>
                        <TimelineScrubber
                            elapsedSeconds={elapsedSec()}
                            durationSeconds={durationSec()}
                            isPlaying={isPlaying()}
                            disabled={!canPlay()}
                            onPlayPause={togglePlayPause}
                            onSeek={seek}
                            onSkip={skip}
                            speed={playbackSpeed()}
                            onSpeedChange={changeSpeed}
                            speedOptions={speedOptions()}
                            skipSeconds={skipStep()}
                            modeTransitions={modeTransitions()}
                        />
                    </div>
                </div>
            </div>

            {/* DATI METRICI */}
            <AttitudeCard
                roll={sample().roll}
                pitch={sample().pitch}
                yaw={sample().yaw}
                accelX={sample().accelX}
                accelY={sample().accelY}
                accelZ={sample().accelZ}
                status={sample().status}
                timestampLabel={timestampLabel()}
            />

            <AtmosphereCard
                temperature={sample().temp}
                pressure={sample().pres}
                humidity={sample().rh}
                gasResistance={sample().gasRes}
                power={sample().power}
            />

            <NavigationCard
                altitude={sample().alt}
                altitudeMSL={sample().altMsl}
                gpsAltitude={sample().galt}
                verticalVelocity={sample().vvel}
                horizontalVelocity={sample().hvel}
                latitude={sample().lat}
                longitude={sample().long}
                gpsFix={sample().gps}
            />

            <div class="grid grid-cols-1 lg:grid-cols-4 gap-4">
                {/* VIDEO PLAYER */}
                <div class="lg:col-span-2 h-full">
                    <VideoPlayer
                        sources={videoSources()}
                        objectFit="cover"
                        loop={false}
                    />
                </div>

                <div class="flex flex-col sm:flex-row gap-4 lg:col-span-2">
                    {/* SOTTO-COLONNA GRAFICI */}
                    <div class="flex flex-col gap-4 flex-1 w-full sm:w-0">
                        <VelocityGraphCard
                            time={sample().ts}
                            verticalVelocity={sample().vvel}
                            horizontalVelocity={sample().hvel}
                            class="w-full"
                            resetKey={graphResetKey()}
                        />

                        <AltitudeGraphCard
                            time={sample().ts}
                            altitude={sample().alt}
                            class="w-full"
                            resetKey={graphResetKey()}
                        />
                    </div>

                    {/* MINI COLONNA TRACKER ALTITUDINE */}
                    <AltitudeTracker
                        currentAltitude={sample().alt}
                        targetAltitude={targetAltitude()}
                        maxAltitude={Math.max(targetAltitude(), 100)}
                        gpsAltitude={sample().gps ? sample().galt - departureAltitude() : undefined}
                        class="w-full sm:w-28 shrink-0 h-[350px] sm:h-auto"
                    />

                </div>

            </div>

            {/* GRAFICO */}
            <div class="grid grid-cols-1 lg:grid-cols-2 gap-4">
                <AccellerationGraphCard
                    time={sample().ts}
                    accelX={sample().accelX}
                    accelY={sample().accelY}
                    accelZ={sample().accelZ}
                    class="w-full"
                    resetKey={graphResetKey()}
                />
                <PowerGraphCard
                    time={sample().ts}
                    power={sample().power}
                    voltage={sample().busVolt}
                    current={sample().current}
                    class="w-full"
                    resetKey={graphResetKey()}
                />
            </div>

            {/* MAPPA */}
            <RocketMapCard
                latitude={sample().lat}
                longitude={sample().long}
                gpsFix={sample().gps}
                resetKey={graphResetKey()}
            />
        </div>

    );
};

export default Dashboard;
