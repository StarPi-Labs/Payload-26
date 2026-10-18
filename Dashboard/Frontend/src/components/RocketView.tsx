import { Component, createEffect, createSignal, on, onCleanup, onMount } from "solid-js";
import { RocketModelConfig, RocketViewProps } from "../models/ui/rocket-view-props";
import type { RocketRenderer } from "../utils/rocket-renderer";

/**
 * Fixed front view of the rocket on black: it turns with the payload's
 * measured orientation and is pushed off centre by its acceleration. The
 * numbers themselves are in the cards above - this window shows no text.
 */
const RocketView: Component<RocketViewProps> = (props) => {
    let container: HTMLDivElement | undefined;
    let canvas: HTMLCanvasElement | undefined;
    let view: RocketRenderer | null = null;
    let resizeObserver: ResizeObserver | undefined;
    let disposed = false;
    const [ready, setReady] = createSignal(false);

    const hasAttitude = () => props.qw !== 0 || props.qx !== 0 || props.qy !== 0 || props.qz !== 0;

    onMount(async () => {
        // three.js only loads with this window, never with the rest of the dashboard.
        const { RocketRenderer } = await import("../utils/rocket-renderer");
        if (disposed || !canvas || !container) return;
        try {
            view = new RocketRenderer(canvas);
        } catch (e) {
            console.warn("rocket view: WebGL is not available", e);
            return;
        }
        const el = container;
        resizeObserver = new ResizeObserver(() => view?.resize(el.clientWidth, el.clientHeight));
        resizeObserver.observe(el);
        view.resize(el.clientWidth, el.clientHeight);
        setReady(true);
    });

    // The flight's own model if it has one, else the default one, else a generic rocket.
    createEffect(on([ready, () => props.flightId], async ([isReady, flightId]) => {
        if (!isReady || !view) return;
        let config: RocketModelConfig | null = null;
        if (flightId) {
            try {
                const res = await fetch(`/api/flights/${flightId}/model`);
                if (res.ok) config = await res.json();
            } catch (e) {
                // no model service: the generic rocket will do
            }
        }
        if (!disposed && view && flightId === props.flightId) await view.setModel(config);
    }));

    createEffect(() => {
        const q: [number, number, number, number] = [props.qw, props.qx, props.qy, props.qz];
        const a = [props.accelX, props.accelY, props.accelZ];
        if (!ready() || !view) return;
        view.setAttitude(hasAttitude() ? q : null);
        view.setAcceleration(a[0], a[1], a[2]);
    });

    onCleanup(() => {
        disposed = true;
        resizeObserver?.disconnect();
        view?.dispose();
        view = null;
    });

    return (
        <div
            ref={container}
            class={`relative rounded-2xl overflow-hidden bg-black border border-base-content/10 ${props.class ?? ""}`}
            title={hasAttitude() ? undefined : "No attitude data for this flight"}
        >
            <canvas ref={canvas} class="absolute inset-0 w-full h-full block" />
        </div>
    );
};

export default RocketView;
