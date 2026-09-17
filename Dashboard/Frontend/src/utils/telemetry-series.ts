/**
 * Helpers for playing back telemetry that may be far too long to hold at full
 * resolution in the browser (a 20-hour recording is ~9 million rows).
 *
 * The dashboard keeps an OVERVIEW of the whole flight (a few thousand rows the
 * server picked so that extremes survive) plus a DETAIL window of higher
 * resolution around the playhead, fetched on demand. These helpers are the
 * pure, stateless parts of that scheme.
 */

export interface SeriesWindow<T> {
    /** Seconds relative to the start of the flight, ascending. */
    times: number[];
    samples: T[];
    /** Coverage of this window, same time base as `times`. */
    start: number;
    end: number;
    /** Typical spacing between points, seconds. */
    spacing: number;
}

/** Largest index i with times[i] <= t, clamped so i + 1 is always valid. */
export function segmentIndex(times: number[], t: number): number {
    const n = times.length;
    if (n < 2) return 0;
    let lo = 0;
    let hi = n - 1;
    while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if (times[mid] <= t) lo = mid;
        else hi = mid - 1;
    }
    return Math.min(Math.max(0, lo), n - 2);
}

export function catmullRom(p0: number, p1: number, p2: number, p3: number, t: number): number {
    const t2 = t * t;
    const t3 = t2 * t;
    return 0.5 * (
        (2 * p1) +
        (-p0 + p2) * t +
        (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
        (-p0 + 3 * p1 - 3 * p2 + p3) * t3
    );
}

/** Median spacing of a time array - robust to the odd gap or duplicate. */
export function medianSpacing(times: number[]): number {
    const n = times.length;
    if (n < 2) return Infinity;
    const step = Math.max(1, Math.floor(n / 2000));   // sample, don't sort 1e6 values
    const gaps: number[] = [];
    for (let i = step; i < n; i += step) {
        gaps.push((times[i] - times[i - step]) / step);
    }
    gaps.sort((a, b) => a - b);
    return gaps[Math.floor(gaps.length / 2)] ?? Infinity;
}

/**
 * Time resolution worth fetching at a given playback speed.
 *
 * At 60 fps every rendered frame advances `speed / 60` seconds of flight, so
 * detail finer than that can never be seen - but never ask for finer than the
 * recording's own native rate either.
 */
export function targetResolution(speed: number, nativeSpacing: number): number {
    return Math.max(nativeSpacing, speed / 60);
}

/** Playback speeds that make sense for a recording of this length. */
export function speedOptionsFor(durationSec: number): number[] {
    const all = [0.25, 0.5, 1, 2, 4, 10, 30, 60, 300, 1000, 3600];
    // Keep speeds that still take at least ~5 s to play the whole flight.
    const usable = all.filter((s) => s <= 1 || durationSec / s >= 5);
    return usable.length ? usable : [1];
}

/** A "nice" skip step: about 2% of the flight, never below 10 s. */
export function skipStepFor(durationSec: number): number {
    const nice = [10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600];
    const want = durationSec / 50;
    let step = nice[0];
    for (const n of nice) {
        if (n <= want) step = n;
    }
    return step;
}

export function formatBytes(bytes: number | undefined): string {
    if (!bytes || bytes <= 0) return "";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let v = bytes;
    let u = 0;
    while (v >= 1024 && u < units.length - 1) {
        v /= 1024;
        u++;
    }
    return `${v.toFixed(v >= 10 || u === 0 ? 0 : 1)} ${units[u]}`;
}
