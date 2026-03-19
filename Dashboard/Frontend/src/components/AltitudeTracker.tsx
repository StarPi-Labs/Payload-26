import { Component, createMemo, mergeProps } from "solid-js";
import { AltitudeTrackerProps } from "../models/ui/altitude-tracker-props";

const AltitudeTracker: Component<AltitudeTrackerProps> = (rawProps) => {
    // Se non viene dato un maxAltitude, diamo un 15% di "spazio vuoto" sopra il target
    const props = mergeProps({
        maxAltitude: rawProps.targetAltitude * 1.15 || 100
    }, rawProps);

    const rocketPercent = createMemo(() => {
        const clamped = Math.max(0, Math.min(props.maxAltitude, props.currentAltitude));
        return (clamped / props.maxAltitude) * 100;
    });
    const targetPercent = createMemo(() => {
        const clamped = Math.max(0, Math.min(props.maxAltitude, props.targetAltitude));
        return (clamped / props.maxAltitude) * 100;
    });

    return (
        <div class={`flex flex-col items-center justify-between bg-base-300/30 border border-base-content/10 rounded-2xl p-6 ${props.class ?? ""}`}>

            {/* Header / Dati testuali */}
            <div class="w-full flex justify-between items-center mb-6 text-sm font-medium">
                <div class="flex flex-col">
                    <span class="text-base-content/50 uppercase text-xs">Current</span>
                    <span class="text-lg text-primary">{props.currentAltitude.toFixed(0)}m</span>
                </div>
            </div>

            {/* BARRA VERTICALE */}
            <div class="relative flex-1 w-16 flex justify-center mt-2 mb-4">
                <div class="absolute inset-y-0 w-2 bg-base-content/10 rounded-full"></div>
                <div
                    class="absolute bottom-0 w-2 bg-gradient-to-t from-orange-500 via-primary to-primary rounded-full transition-all duration-500 ease-out"
                    style={{ height: `${rocketPercent()}%` }}
                ></div>

                {/*LINEA DEL TARGET */}
                <div
                    class="absolute w-full flex items-center justify-center pointer-events-none"
                    style={{ bottom: `${targetPercent()}%` }}
                >
                    <div class="w-full border-b-2 border-dashed border-emerald-500 relative">
                        {/* Etichetta target */}
                        <span class="absolute left-full ml-2 -translate-y-1/2 text-[10px] font-bold text-emerald-500 tracking-wider">
                            TARGET
                        </span>
                    </div>
                </div>

                <div
                    class="absolute w-12 h-12 -ml-6 left-1/2 transition-all duration-500 ease-out flex items-center justify-center drop-shadow-[0_0_10px_rgba(var(--p),0.8)]"
                    style={{ bottom: `calc(${rocketPercent()}% - 24px)` }}
                >
                    {/* SVG Razzo */}
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" class="w-full h-full text-primary -rotate-45">
                        <path d="M4.5 16.5c-1.5 1.26-2 5-2 5s3.74-.5 5-2c.71-.84.7-2.13-.09-2.91a2.18 2.18 0 0 0-2.91-.09z" />
                        <path d="m12 15-3-3a22 22 0 0 1 2-3.95A12.88 12.88 0 0 1 22 2c0 2.72-.78 7.5-6 11a22.35 22.35 0 0 1-4 2z" />
                        <path d="M9 12H4s.55-3.03 2-4c1.62-1.08 5 0 5 0" />
                        <path d="M12 15v5s3.03-.55 4-2c1.08-1.62 0-5 0-5" />
                    </svg>
                </div>

            </div>
        </div>
    );
};

export default AltitudeTracker;