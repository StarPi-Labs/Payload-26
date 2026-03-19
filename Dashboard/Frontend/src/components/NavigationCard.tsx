import { Component } from "solid-js"
import { NavigationCardProps } from "../models/ui/navigation-card-props"
import TelemetryCard from "./TelemetryCard"
import MetricStat from "./MetricStat"
import StatusChip from "./StatusChip"

const NavigationCard: Component<NavigationCardProps> = (props) => {
    return (
        <TelemetryCard
            title="Navigation"
            subtitle="Altitude, velocities, coordinates"
            class={props.class}
        >
            <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-6 gap-3">
                <MetricStat
                    label="Altitude"
                    value={props.altitude}
                    unit="m"
                    precision={1}
                    class="lg:col-span-2"
                />
                <MetricStat
                    label="Vertical vel"
                    value={props.verticalVelocity}
                    unit="m/s"
                    precision={1}
                    class="lg:col-span-2"
                />
                <MetricStat
                    label="Horizontal vel"
                    value={props.horizontalVelocity}
                    unit="m/s"
                    precision={1}
                    class="lg:col-span-2"
                />
                <MetricStat
                    label="Latitude"
                    value={props.latitude}
                    unit="deg"
                    precision={5}
                    class="lg:col-span-3"
                />
                <MetricStat
                    label="Longitude"
                    value={props.longitude}
                    unit="deg"
                    precision={5}
                    class="lg:col-span-3"
                />
            </div>
            <div class="flex flex-wrap gap-3">
                <StatusChip label="GPS" value={props.gpsFix} trueLabel="FIX" falseLabel="NO FIX" />
            </div>
        </TelemetryCard>
    )
}

export default NavigationCard
