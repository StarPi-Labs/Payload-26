import { Component, createSignal, onCleanup, onMount } from "solid-js";

import { AtmosphericSample } from "../models/atmospheric-sample";

import AttitudeCard from "../components/AttitudeCard";
import AtmosphereCard from "../components/AtmosphereCard";
import NavigationCard from "../components/base/NavigationCard";
import VelocityGraphCard from "../components/VelocityGraphCard";
import AltitudeGraphCard from "../components/AltitudeGraphCard";
import AccellerationGraphCard from "../components/AccellerationGraphCard";
import AltitudeTracker from "../components/AltitudeTracker";
import VideoPlayer from "../components/VideoPlayer";
import RocketMapCard from "../components/RocketMapCard"
import { VideoSource } from "../models/videp-source";

const Dashboard: Component = () => {

    // Use only server telemetry (polling). Map server telemetry points to AtmosphericSample.
    const emptySample: AtmosphericSample = {
        ts: Date.now(),
        roll: 0,
        pitch: 0,
        yaw: 0,
        status: false,

        alt: 0,
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
    };

    const [sample, setSample] = createSignal<AtmosphericSample>(emptySample);
    const [targetAltitude, setTargetAltitude] = createSignal(1200);
    const [videoSources, setVideoSources] = createSignal<VideoSource[]>([]);
    const [t0EpochMs, setT0EpochMs] = createSignal<number | null>(null);

    let playbackRaf: number | undefined = undefined;
    let activeFlightId: string | null = null;
    let telemetryFrames: any[] = [];

    const numericFields: Array<keyof AtmosphericSample> = [
        "roll", "pitch", "yaw",
        "alt", "vvel", "hvel",
        "lat", "long",
        "temp", "pres", "rh",
        "accelX", "accelY", "accelZ",
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
        };
    }

    function catmullRom(p0: number, p1: number, p2: number, p3: number, t: number): number {
        const t2 = t * t;
        const t3 = t2 * t;
        return 0.5 * (
            (2 * p1) +
            (-p0 + p2) * t +
            (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
            (-p0 + 3 * p1 - 3 * p2 + p3) * t3
        );
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
            // Keep non-numeric states as step values.
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

    async function loadTelemetryForActiveFlight() {
        if (!activeFlightId) return;
        try {
            const res = await fetch(`/api/flights/${activeFlightId}/telemetry`);
            if (!res.ok) return;
            const json = await res.json();
            const data = json.data || [];
            const targetFromMeta = Number(json?.meta?.targetAltitude);
            const t0EpochFromMeta = Number(json?.meta?.t0EpochMs);
            const t0IsoFromMeta = typeof json?.meta?.t0 === "string" ? json.meta.t0 : null;

            if (Number.isFinite(t0EpochFromMeta) && t0EpochFromMeta > 0) {
                setT0EpochMs(t0EpochFromMeta);
            } else if (t0IsoFromMeta) {
                const parsed = Date.parse(t0IsoFromMeta);
                if (Number.isFinite(parsed)) {
                    setT0EpochMs(parsed);
                }
            }

            if (Number.isFinite(targetFromMeta) && targetFromMeta > 0) {
                setTargetAltitude(targetFromMeta);
            }
            if (data.length) {
                telemetryFrames = data;
            }
        } catch (e) {
            // ignore network errors silently for now
        }
    }

    async function loadVideosForActiveFlight() {
        if (!activeFlightId) return;
        try {
            const res = await fetch(`/api/flights/${activeFlightId}/videos`);
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

    function getPointTimeSeconds(pt: any, fallbackIndex: number): number {
        const t = Number(pt?.time);
        if (Number.isFinite(t) && t >= 0) return t;
        return fallbackIndex * 0.1;
    }

    function clearPlaybackTimers() {
        if (playbackRaf) {
            cancelAnimationFrame(playbackRaf);
            playbackRaf = undefined;
        }
    }

    function startPlaybackOnce() {
        if (telemetryFrames.length < 2) {
            if (telemetryFrames.length === 1) {
                setSample(mapTelemetryPointToSample(telemetryFrames[0]));
            }
            return;
        }

        clearPlaybackTimers();

        const frameSamples = telemetryFrames.map((pt: any) => mapTelemetryPointToSample(pt));
        const frameTimes = telemetryFrames.map((pt: any, i: number) => getPointTimeSeconds(pt, i));
        const firstTime = frameTimes[0];
        const lastTime = frameTimes[frameTimes.length - 1];
        const totalDurationMs = Math.max(1, (lastTime - firstTime) * 1000);
        const playbackEpoch = t0EpochMs() ?? Date.now();
        const startPerf = performance.now();

        const findSegmentIndex = (tSec: number): number => {
            let i = 0;
            while (i < frameTimes.length - 2 && tSec >= frameTimes[i + 1]) {
                i++;
            }
            return i;
        };

        const animate = (now: number) => {
            const elapsedMs = now - startPerf;
            const elapsedSec = elapsedMs / 1000;
            const tSec = firstTime + elapsedSec;

            if (elapsedMs >= totalDurationMs) {
                const last = frameSamples[frameSamples.length - 1];
                setSample({ ...last, ts: playbackEpoch + totalDurationMs });
                playbackRaf = undefined;
                return;
            }

            const i = findSegmentIndex(tSec);
            const t0 = frameTimes[i];
            const t1 = frameTimes[i + 1];
            const denom = Math.max(0.000001, t1 - t0);
            const u = (tSec - t0) / denom;

            const prev = frameSamples[Math.max(0, i - 1)];
            const current = frameSamples[i];
            const next = frameSamples[i + 1];
            const next2 = frameSamples[Math.min(frameSamples.length - 1, i + 2)];

            setSample(interpolateSampleCurve(prev, current, next, next2, u, playbackEpoch + elapsedMs));
            playbackRaf = requestAnimationFrame(animate);
        };

        // exact first point
        setSample({ ...frameSamples[0], ts: playbackEpoch });
        playbackRaf = requestAnimationFrame(animate);
    }

    onMount(async () => {
        try {
            const res = await fetch('/api/flights');
            if (!res.ok) return;
            const json = await res.json();
            const flights = json.flights || [];
            if (flights.length === 0) return;
            activeFlightId = flights[0].id;

            // initial load, then replay once using telemetry time deltas
            await loadVideosForActiveFlight();
            await loadTelemetryForActiveFlight();
            startPlaybackOnce();
        } catch (e) {
            // nothing
        }
    });

    onCleanup(() => {
        clearPlaybackTimers();
    });

    const timestampLabel = () => {
        const value = sample().ts;
        return new Date(value).toLocaleTimeString();
    };

    return (
        <div class="space-y-6">
            <div class="flex flex-col gap-2">
                <h1 class="text-2xl font-semibold">Dashboard</h1>
            </div>

            {/* DATI METRICI */}
            <div class="grid grid-cols-1 xl:grid-cols-3 gap-4">
                <AttitudeCard
                    roll={sample().roll}
                    pitch={sample().pitch}
                    yaw={sample().yaw}
                    status={sample().status}
                    timestampLabel={timestampLabel()}
                />
                <AtmosphereCard
                    temperature={sample().temp}
                    pressure={sample().pres}
                    humidity={sample().rh}
                />
                <NavigationCard
                    altitude={sample().alt}
                    verticalVelocity={sample().vvel}
                    horizontalVelocity={sample().hvel}
                    latitude={sample().lat}
                    longitude={sample().long}
                    gpsFix={sample().gps}
                />
            </div>

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
                        />

                        <AltitudeGraphCard
                            time={sample().ts}
                            altitude={sample().alt}
                            class="w-full"
                        />
                    </div>
                    
                    {/* MINI COLONNA TRACKER ALTITUDINE */}
                    <AltitudeTracker
                        currentAltitude={sample().alt}
                        targetAltitude={targetAltitude()}
                        maxAltitude={Math.max(targetAltitude(), 100)}
                        class="w-full sm:w-28 shrink-0 h-[350px] sm:h-auto"
                    />

                </div>

            </div>

            {/* GRAFICO */}
            <div class="grid gap-4">
                <AccellerationGraphCard
                    time={sample().ts}
                    accelX={sample().accelX}
                    accelY={sample().accelY}
                    accelZ={sample().accelZ}
                    class="w-full"
                />
            </div>
            
            {/* MAPPA */}
            <RocketMapCard
                latitude={sample().lat}
                longitude={sample().long}
                gpsFix={sample().gps}
            />
        </div>

    );
};

export default Dashboard;
