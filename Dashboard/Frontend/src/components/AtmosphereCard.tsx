import { Component } from "solid-js"
import { AtmosphereCardProps } from "../models/ui/atmosphere-card-props"
import TelemetryCard from "./TelemetryCard"
import MetricStat from "./MetricStat"

const AtmosphereCard: Component<AtmosphereCardProps> = (props) => {
    return (
        <TelemetryCard
            title="Atmosphere"
            subtitle="Temperature, pressure, humidity"
            class={props.class}
        >
            <div class="grid grid-cols-1 sm:grid-cols-3 gap-3">
                <MetricStat label="Temperature" value={props.temperature} unit="C" precision={1} />
                <MetricStat label="Pressure" value={props.pressure} unit="hPa" precision={1} />
                <MetricStat label="Humidity" value={props.humidity} unit="%" precision={0} />
            </div>
        </TelemetryCard>
    )
}

export default AtmosphereCard
