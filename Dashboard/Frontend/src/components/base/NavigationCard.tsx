import { Component, createMemo } from "solid-js"
import { NavigationCardProps } from "../../models/ui/navigation-card-props"
import TelemetryCard from "./TelemetryCard"
import MetricStat from "./MetricStat"
import StatusChip from "./StatusChip"

const NavigationCard: Component<NavigationCardProps> = (props) => {
    // Vertical velocity is the derivative of the barometric altitude, so its
    // sign directly says whether altitude is currently rising or falling.
    const altitudeTrend = createMemo(() => {
        const v = props.verticalVelocity ?? 0
        if (v > 0.2) return "up" as const
        if (v < -0.2) return "down" as const
        return "flat" as const
    })

    return (
        <TelemetryCard
            title="Navigation"
            subtitle="Altitude, velocities, coordinates"
            class={props.class}
        >
            <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-5 gap-3">
                <MetricStat
                    label="Vertical vel"
                    value={props.verticalVelocity}
                    unit="m/s"
                    precision={1}
                />
                <MetricStat
                    label="Horizontal vel"
                    value={props.horizontalVelocity}
                    unit="m/s"
                    precision={1}
                />
                <MetricStat
                    label="Altitude"
                    value={props.altitude}
                    unit="m"
                    precision={1}
                    trend={altitudeTrend()}
                />
                <MetricStat
                    label="Altitude MSL"
                    value={props.altitudeMSL}
                    unit="m"
                    precision={1}
                />
                <MetricStat
                    label="GPS Altitude"
                    value={props.gpsAltitude}
                    unit="m"
                    precision={1}
                />
            </div>
            <div class="grid grid-cols-1 sm:grid-cols-2 gap-3">
                <MetricStat
                    label="Latitude"
                    value={props.latitude}
                    unit="deg"
                    precision={5}
                />
                <MetricStat
                    label="Longitude"
                    value={props.longitude}
                    unit="deg"
                    precision={5}
                />
            </div>
            <div class="flex flex-wrap gap-3">
                <StatusChip label="GPS" value={props.gpsFix} trueLabel="FIX" falseLabel="NO FIX" />
            </div>
        </TelemetryCard>
    )
}

export default NavigationCard
