import { Component, createSignal, createEffect, createMemo, Show, For, on } from "solid-js";
import { GraphCardProps } from "../models/ui/graph-card-props";
import TelemetryCard from "./TelemetryCard";

const GraphCard: Component<GraphCardProps> = (props) => {

    //---- Costanti per le linee del grafico ----
    const SVG_WIDTH = 600, SVG_HEIGHT = 200;

    const PADDING_TOP = 12;
    const PADDING_RIGHT = 20;
    const PADDING_BOTTOM = 35;
    const PADDING_LEFT = 40;

    const drawWidth = SVG_WIDTH - PADDING_LEFT - PADDING_RIGHT;
    const drawHeight = SVG_HEIGHT - PADDING_TOP - PADDING_BOTTOM;

    //---- Buffer circolare ----
    let buffer = new CircularBuffer<Record<string, number | string>>(props.maxPoints || 100);
    const [tick, setTick] = createSignal(0);

    createEffect(on(() => props.newPoint, (pt) => {
        if (!pt) return;
        // se per qualche motivo cambia la capacità del buffer
        if (buffer.capacity !== (props.maxPoints || 100)) {
            buffer = new CircularBuffer(props.maxPoints || 100);
        }

        buffer.push(pt);
        setTick(t => t + 1);
    }, { defer: true }));

    const stats = createMemo(() => {
        tick();
        if (buffer.length === 0 || !props.lines.length) return null;

        let minT = Number(buffer.first![props.xKey]), maxT = minT;
        let minV = Infinity, maxV = -Infinity;

        for (const pt of buffer) {
            const t = Number(pt[props.xKey]);
            if (t < minT) minT = t; else if (t > maxT) maxT = t;

            for (const line of props.lines) {
                const val = Number(pt[line.key]);
                if (val < minV) minV = val;
                if (val > maxV) maxV = val;
            }
        }

        const xRange = (maxT - minT) || 1;
        const yRange = (maxV - minV) || 1;

        return { minT, maxT, minV, maxV, xRange, yRange };
    });

    //---- Linee asse Y ----
    const yTicks = createMemo(() => {
        tick();
        const s = stats();
        if (!s) return [];
        const numTicks = 4;
        return Array.from({ length: numTicks + 1 }).map((_, i) => {
            const fraction = i / numTicks;
            const val = s.minV + (s.yRange * fraction);
            const y = SVG_HEIGHT - PADDING_BOTTOM - (fraction * drawHeight);
            return { val: val.toFixed(1), y };
        });
    });

    //---- Linee asse X ----
    const xTicks = createMemo(() => {
        tick();
        const s = stats();
        if (!s) return [];
        const numTicks = 4;
        return Array.from({ length: numTicks + 1 }).map((_, i) => {
            const fraction = i / numTicks;
            const val = s.minT + (s.xRange * fraction);
            const x = PADDING_LEFT + (fraction * drawWidth);

            const timeString = new Date(val).toLocaleTimeString(undefined, {
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit'
            });

            return { val: timeString, x };
        });
    });

    //---- disegnare le linee ----
    const paths = createMemo(() => {
        tick();
        const s = stats();
        if (!s || buffer.length === 0) return [];

        return props.lines.map(line => {
            let d = "";
            let i = 0;
            for (const pt of buffer) {
                const x = PADDING_LEFT + ((Number(pt[props.xKey]) - s.minT) / s.xRange) * drawWidth;
                const y = SVG_HEIGHT - PADDING_BOTTOM - ((Number(pt[line.key]) - s.minV) / s.yRange) * drawHeight;
                d += `${i === 0 ? 'M' : ' L'} ${x.toFixed(1)},${y.toFixed(1)}`;
                i++;
            }
            return { ...line, d };
        });
    });

    const zeroY = createMemo(() => {
        tick();
        const s = stats();
        if (!s || s.minV >= 0 || s.maxV <= 0) return null;
        return SVG_HEIGHT - PADDING_BOTTOM - ((0 - s.minV) / s.yRange) * drawHeight;
    });

    return (
        <TelemetryCard title={props.title} subtitle={props.subtitle} class={props.class}>
            <div class="flex flex-col gap-4">
                <div class="flex flex-wrap items-center gap-8">
                    {/* LEGENDA */}
                    <div class="flex flex-wrap gap-4 text-sm font-medium">
                        <For each={props.lines}>
                            {(line) => (
                                <div class="flex items-center gap-2">
                                    <span class={`w-3 h-3 rounded-full ${line.legendClass}`} style={{ "background-color": line.color }}></span>
                                    <span>{line.label}</span>
                                </div>
                            )}
                        </For>
                    </div>

                    {/* PULSANTI */}
                    <Show when={props.controls}>
                        <div class="flex items-center gap-2">
                            {props.controls}
                        </div>
                    </Show>
                </div>

                <div class="w-full h-48 bg-base-300/30 rounded-lg overflow-hidden relative">
                    <Show when={(tick(), buffer.length > 1)} fallback={
                        <div class="flex h-full items-center justify-center text-sm text-base-content/50">
                            Waiting for telemetry...
                        </div>
                    }>
                        <svg viewBox={`0 0 ${SVG_WIDTH} ${SVG_HEIGHT}`} preserveAspectRatio="none" class="w-full h-full overflow-visible">

                            {/* Griglia orizzontale */}
                            <g class="text-base-content text-[11px] font-sans" fill="currentColor">
                                <For each={yTicks()}>
                                    {(t) => (
                                        <g>
                                            <line x1={PADDING_LEFT} x2={SVG_WIDTH - PADDING_RIGHT} y1={t.y} y2={t.y}
                                                stroke="currentColor" class="opacity-20" vector-effect="non-scaling-stroke" />
                                            <text x={PADDING_LEFT - 8} y={t.y + 3} text-anchor="end">{t.val}</text>
                                        </g>
                                    )}
                                </For>
                            </g>
                            {/* Griglia verticale */}
                            <g class="text-base-content text-[11px] font-sans" fill="currentColor">
                                <For each={xTicks()}>
                                    {(t) => (
                                        <g>
                                            <line
                                                x1={t.x} x2={t.x}
                                                y1={PADDING_TOP} y2={SVG_HEIGHT - PADDING_BOTTOM}
                                                stroke="#ef4444"
                                                stroke-dasharray="4 4"
                                                class="opacity-40"
                                                vector-effect="non-scaling-stroke"
                                            />
                                            {/* Testo asse X */}
                                            <text x={t.x} y={SVG_HEIGHT - PADDING_BOTTOM + 18} text-anchor="middle">{t.val}</text>
                                        </g>
                                    )}
                                </For>
                            </g>

                            {/* Linea dello Zero */}
                            <Show when={zeroY()}>
                                <line x1={PADDING_LEFT} x2={SVG_WIDTH - PADDING_RIGHT} y1={zeroY()!} y2={zeroY()!}
                                    stroke="currentColor" class="text-base-content/50" stroke-dasharray="4"
                                    vector-effect="non-scaling-stroke" />
                            </Show>

                            <For each={paths()}>
                                {(p) => (
                                    <path
                                        d={p.d}
                                        fill="none"
                                        stroke={p.color}
                                        stroke-width="3"
                                        stroke-linejoin="round"
                                        vector-effect="non-scaling-stroke"
                                    />
                                )}
                            </For>
                        </svg>
                    </Show>
                </div>
            </div>
        </TelemetryCard>
    );
}

export default GraphCard;


export class CircularBuffer<T> {
    private buffer: T[];
    private head: number = 0;
    private size: number = 0;

    constructor(public capacity: number) {
        this.buffer = new Array(capacity);
    }

    push(item: T) {
        this.buffer[(this.head + this.size) % this.capacity] = item;
        if (this.size < this.capacity) {
            this.size++;
        } else {
            this.head = (this.head + 1) % this.capacity;
        }
    }

    get length() { return this.size; }
    get first(): T | undefined { return this.size === 0 ? undefined : this.buffer[this.head]; }

    *[Symbol.iterator]() {
        for (let i = 0; i < this.size; i++) {
            yield this.buffer[(this.head + i) % this.capacity];
        }
    }
}