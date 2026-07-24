import { Component, createMemo } from "solid-js"
import { AtmosphereCardProps } from "../models/ui/atmosphere-card-props"
import TelemetryCard from "./base/TelemetryCard"
import MetricStat from "./base/MetricStat"

const AtmosphereCard: Component<AtmosphereCardProps> = (props) => {
    // Gas resistance reads in the tens-to-hundreds of kOhm -- kOhm is a more
    // readable unit than raw ohms for the stat tile.
    const gasKOhm = createMemo(() => {
        const v = props.gasResistance
        return v === null || v === undefined ? v : v / 1000
    })

    return (
        <TelemetryCard
            title="Atmosphere"
            subtitle="Temperature, pressure, humidity, gas, power"
            class={props.class}
        >
            <div class="grid grid-cols-1 sm:grid-cols-3 lg:grid-cols-5 gap-3">
                <MetricStat label="Temperature" value={props.temperature} unit="C" precision={1} />
                <MetricStat label="Pressure" value={props.pressure} unit="kPa" precision={1} />
                <MetricStat label="Humidity" value={props.humidity} unit="%" precision={0} />
                <MetricStat label="Gas" value={gasKOhm()} unit="kΩ" precision={1} />
                <MetricStat label="Power" value={props.power} unit="W" precision={2} />
            </div>
        </TelemetryCard>
    )
}

export default AtmosphereCard
