import { Component, createSignal } from "solid-js";
import { PowerGraphCardProps } from "../models/ui/power-graph-props";
import BaseGraphCard from "./base/GraphCard";

const PowerGraphCard: Component<PowerGraphCardProps> = (props) => {
    const [showPower, setShowPower] = createSignal(true);
    const [showVoltage, setShowVoltage] = createSignal(true);
    const [showCurrent, setShowCurrent] = createSignal(true);

    // Current is carried in mA everywhere else in the app; convert to A here
    // so its magnitude sits closer to power/voltage on the shared axis.
    const currentA = () => props.current !== undefined ? props.current / 1000 : undefined;

    return (
        <div class={`flex flex-col gap-4 ${props.class ?? ""}`}>
            <BaseGraphCard
                title="Power"
                subtitle="Real-time W / V / A"
                newPoint={props.time !== undefined && props.power !== undefined
                    ? { time: props.time, power: props.power, voltage: props.voltage ?? 0, current: currentA() ?? 0 }
                    : undefined}
                maxPoints={100}
                xKey="time"
                class="w-full"
                resetKey={props.resetKey}

                controls={
                    <>
                        <button
                            class={`btn btn-xs ${showPower() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowPower(!showPower())}
                        >
                            {showPower() ? 'Hide' : 'Show'} Power
                        </button>
                        <button
                            class={`btn btn-xs ${showVoltage() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowVoltage(!showVoltage())}
                        >
                            {showVoltage() ? 'Hide' : 'Show'} Voltage
                        </button>
                        <button
                            class={`btn btn-xs ${showCurrent() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowCurrent(!showCurrent())}
                        >
                            {showCurrent() ? 'Hide' : 'Show'} Current
                        </button>
                    </>
                }

                lines={[
                    ...(showPower() ? [{ key: "power", label: "Power (W)", color: "#f59e0bff", legendClass: "bg-amber-500" }] : []),
                    ...(showVoltage() ? [{ key: "voltage", label: "Voltage (V)", color: "#22d3eeff", legendClass: "bg-cyan-500" }] : []),
                    ...(showCurrent() ? [{ key: "current", label: "Current (A)", color: "#a3e635ff", legendClass: "bg-lime-500" }] : []),
                ]}
            />
        </div>
    );
};

export default PowerGraphCard;
