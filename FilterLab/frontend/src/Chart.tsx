import { Component, createEffect, onCleanup, onMount } from "solid-js";
import uPlot from "uplot";
import { Series } from "./types";

export const PALETTE = ["#60a5fa", "#f97316", "#34d399", "#f472b6", "#facc15", "#a78bfa", "#22d3ee", "#fb7185"];

interface ChartProps {
    series: Series[];
    before: Series[];
    /** called with the visible x range after a drag-zoom, null after a reset */
    onZoom: (range: [number, number] | null) => void;
}

/** Union of all x values, each series' y placed at its own x (null elsewhere). */
function align(all: Series[]): uPlot.AlignedData {
    const set = new Set<number>();
    for (const s of all) for (const x of s.x) set.add(x);
    const xs = Float64Array.from([...set].sort((a, b) => a - b));
    const index = new Map<number, number>();
    xs.forEach((x, i) => index.set(x, i));
    const cols = all.map((s) => {
        const col: (number | null)[] = new Array(xs.length).fill(null);
        s.x.forEach((x, i) => {
            col[index.get(x)!] = s.y[i];
        });
        return col;
    });
    return [xs, ...cols] as uPlot.AlignedData;
}

function withAlpha(hex: string, alpha: number) {
    const a = Math.round(alpha * 255).toString(16).padStart(2, "0");
    return hex + a;
}

const Chart: Component<ChartProps> = (props) => {
    let host: HTMLDivElement | undefined;
    let plot: uPlot | null = null;
    let observer: ResizeObserver | undefined;

    const size = () => ({
        width: Math.max(200, host?.clientWidth ?? 600),
        height: Math.max(160, (host?.clientHeight ?? 300) - 34),     // legend below
    });

    function build() {
        plot?.destroy();
        plot = null;
        if (!host) return;
        const after = props.series;
        if (!after.length) return;
        const kind = after[0].axis;
        const shown = after.filter((s) => s.axis === kind);
        const names = new Set(shown.map((s) => s.name));
        const before = props.before.filter((s) => s.axis === kind && names.has(s.name));
        const all = [...before, ...shown];

        const units = [...new Set(shown.map((s) => s.unit))];
        const scaleOf = (unit: string) => (units.indexOf(unit) === 1 ? "y1" : "y0");
        const colour = new Map(shown.map((s, i) => [s.name, PALETTE[i % PALETTE.length]]));
        const log = kind !== "time";
        const stepped = uPlot.paths.stepped ? uPlot.paths.stepped({ align: 1 }) : undefined;

        const series: uPlot.Series[] = [
            { label: kind === "time" ? "time [s]" : kind === "frequency" ? "frequency [Hz]" : "τ [s]" },
            ...all.map((s, i) => {
                const isBefore = i < before.length;
                const c = colour.get(s.name) ?? "#999999";
                return {
                    label: `${s.name}${isBefore ? " (before)" : ""}${s.unit ? ` [${s.unit}]` : ""}`,
                    scale: scaleOf(s.unit),
                    stroke: isBefore ? withAlpha(c, 0.45) : c,
                    width: isBefore ? 1 : 1.5,
                    dash: isBefore ? [5, 4] : undefined,
                    spanGaps: true,
                    paths: s.step ? stepped : undefined,
                    points: { show: false },
                    value: (_u: uPlot, v: number | null) => (v == null ? "–" : Math.abs(v) >= 1e5 || (Math.abs(v) < 1e-3 && v !== 0) ? v.toExponential(3) : v.toPrecision(6)),
                } as uPlot.Series;
            }),
        ];
        const axisStyle = { stroke: "#9ca3af", grid: { stroke: "#ffffff14", width: 1 }, ticks: { stroke: "#ffffff22" } };
        const axes: uPlot.Axis[] = [
            { ...axisStyle },
            { ...axisStyle, scale: "y0", label: units[0] || "", labelSize: 16, size: 64 },
        ];
        if (units.length > 1) {
            axes.push({ ...axisStyle, scale: "y1", side: 1, grid: { show: false }, label: units[1] || "", labelSize: 16, size: 64 });
        }
        const opts: uPlot.Options = {
            ...size(),
            series,
            axes,
            scales: {
                x: { time: false, distr: log ? 3 : 1 },
                y0: { distr: log ? 3 : 1 },
                y1: { distr: log ? 3 : 1 },
            },
            cursor: { drag: { x: true, y: false, setScale: true } },
            legend: { live: true },
            hooks: {
                setSelect: [
                    (u) => {
                        if (u.select.width > 4) {
                            const a = u.posToVal(u.select.left, "x");
                            const b = u.posToVal(u.select.left + u.select.width, "x");
                            if (kind === "time") props.onZoom([a, b]);
                        }
                    },
                ],
            },
        };
        plot = new uPlot(opts, align(all), host);
        plot.over.addEventListener("dblclick", () => props.onZoom(null));
    }

    onMount(() => {
        observer = new ResizeObserver(() => plot?.setSize(size()));
        if (host) observer.observe(host);
    });

    createEffect(() => {
        // rebuild whenever the data changes
        void props.series;
        void props.before;
        build();
    });

    onCleanup(() => {
        observer?.disconnect();
        plot?.destroy();
    });

    return <div ref={host} class="absolute inset-0 overflow-hidden" />;
};

export default Chart;
