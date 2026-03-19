import { Component, createSignal } from "solid-js"
import BaseGraphCard from "./GraphCard"
import { AccellerationGraphCardProps } from "../models/ui/accelleration-graph-props";

const AccellerationGraphCard: Component<AccellerationGraphCardProps> = (props) => {
    const [showX, setShowX] = createSignal(true);
    const [showY, setShowY] = createSignal(true);
    const [showZ, setShowZ] = createSignal(true);

    return (
        <div class={`flex flex-col w-full ${props.class ?? ""}`}>
            <BaseGraphCard
                title="Accelleration"
                subtitle="Real-time m/s^2"
                newPoint={props.time !== undefined && props.accelX !== undefined && props.accelY !== undefined && props.accelZ !== undefined ? { time: props.time, accelX: props.accelX, accelY: props.accelY, accelZ: props.accelZ } : undefined}
                maxPoints={100}
                xKey="time"
                class="w-full"

                controls={
                    <>
                        <button
                            class={`btn btn-xs ${showX() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowX(!showX())}
                        >
                            {showX() ? 'Hide' : 'Show'} X
                        </button>
                        <button
                            class={`btn btn-xs ${showY() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowY(!showY())}
                        >
                            {showY() ? 'Hide' : 'Show'} Y
                        </button>
                        <button
                            class={`btn btn-xs ${showZ() ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setShowZ(!showZ())}
                        >
                            {showZ() ? 'Hide' : 'Show'} Z
                        </button>
                    </>
                }

                lines={[
                    ...(showX() ? [{ key: "accelX", label: "X", color: "#ddf600ff", legendClass: "bg-blue-500" }] : []),
                    ...(showY() ? [{ key: "accelY", label: "Y", color: "#00ff11ff", legendClass: "bg-emerald-500" }] : []),
                    ...(showZ() ? [{ key: "accelZ", label: "Z", color: "#00fffbff", legendClass: "bg-emerald-500" }] : [])
                ]}
            />
        </div>
    )
}

export default AccellerationGraphCard;