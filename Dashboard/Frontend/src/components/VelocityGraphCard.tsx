import { Component, createSignal } from "solid-js"
import BaseGraphCard from "./GraphCard"
import { VelocityGraphCardProps } from "../models/ui/velocity-graph-props";

const VelocityGraphCard: Component<VelocityGraphCardProps> = (props) => {
    const [showVertical, setShowVertical] = createSignal(true);
    const [showHorizontal, setShowHorizontal] = createSignal(true);

    return (
        <div class={`flex flex-col w-full ${props.class ?? ""}`}>
            <BaseGraphCard
                title="Velocities"
                subtitle="Real-time m/s"
                newPoint={props.time !== undefined && props.verticalVelocity !== undefined && props.horizontalVelocity !== undefined ? { time: props.time, verticalVelocity: props.verticalVelocity, horizontalVelocity: props.horizontalVelocity } : undefined}
                maxPoints={100}
                xKey="time"
                class="w-full"

                controls={
                    <>
                        <button
                            class={`btn btn-xs ${showHorizontal() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowHorizontal(!showHorizontal())}
                        >
                            {showHorizontal() ? 'Hide' : 'Show'} Horizontal
                        </button>
                        <button
                            class={`btn btn-xs ${showVertical() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowVertical(!showVertical())}
                        >
                            {showVertical() ? 'Hide' : 'Show'} Vertical
                        </button>
                    </>
                }

                lines={[
                    ...(showVertical() ? [{ key: "verticalVelocity", label: "Vertical", color: "#2b5aab", legendClass: "bg-blue-500" }] : []),
                    ...(showHorizontal() ? [{ key: "horizontalVelocity", label: "Horizontal", color: "#37cb9a", legendClass: "bg-emerald-500" }] : [])
                ]}
            />
        </div>
    )
}

export default VelocityGraphCard;