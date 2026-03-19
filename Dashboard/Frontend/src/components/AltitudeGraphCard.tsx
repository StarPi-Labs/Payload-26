import { Component, createSignal } from "solid-js";
import { AltitudeGraphCardProps } from "../models/ui/altitude-graph-props";
import BaseGraphCard from "./GraphCard";

const AltitudeGraphCard: Component<AltitudeGraphCardProps> = (props) => {
    const [showAltitude] = createSignal(true);
    return (
        <div class={`flex flex-col gap-4 ${props.class ?? ""}`}>
            <BaseGraphCard
                title="Altitude"
                subtitle="Real-time m"
                newPoint={props.time !== undefined && props.altitude !== undefined ? { time: props.time, altitude: props.altitude } : undefined}
                maxPoints={100}
                xKey="time"
                class="w-full"
                lines={[
                    ...(showAltitude() ? [{ key: "altitude", label: "Altitude", color: "#ab2b8dff", legendClass: "bg-blue-500" }] : []),
                ]}
            />
        </div>
    );
};

export default AltitudeGraphCard;
