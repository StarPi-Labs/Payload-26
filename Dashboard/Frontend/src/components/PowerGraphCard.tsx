import { Component, createSignal } from "solid-js";
import { PowerGraphCardProps } from "../models/ui/power-graph-props";
import BaseGraphCard from "./base/GraphCard";

const PowerGraphCard: Component<PowerGraphCardProps> = (props) => {
    const [showPower] = createSignal(true);
    return (
        <div class={`flex flex-col gap-4 ${props.class ?? ""}`}>
            <BaseGraphCard
                title="Power"
                subtitle="Real-time W"
                newPoint={props.time !== undefined && props.power !== undefined ? { time: props.time, power: props.power } : undefined}
                maxPoints={100}
                xKey="time"
                class="w-full"
                resetKey={props.resetKey}
                lines={[
                    ...(showPower() ? [{ key: "power", label: "Power", color: "#f59e0bff", legendClass: "bg-amber-500" }] : []),
                ]}
            />
        </div>
    );
};

export default PowerGraphCard;
