import { Component } from "solid-js"
import { AttitudeCardProps } from "../models/ui/attitude-card-props"
import TelemetryCard from "./TelemetryCard"
import MetricStat from "./MetricStat"
import StatusChip from "./StatusChip"

const AttitudeCard: Component<AttitudeCardProps> = (props) => {
    return (
        <TelemetryCard
            title="Attitude"
            subtitle="Roll, pitch, yaw"
            badge={props.timestampLabel}
            class={props.class}
        >
            <div class="grid grid-cols-1 sm:grid-cols-3 gap-3">
                <MetricStat label="Roll" value={props.roll} unit="deg" precision={1} />
                <MetricStat label="Pitch" value={props.pitch} unit="deg" precision={1} />
                <MetricStat label="Yaw" value={props.yaw} unit="deg" precision={1} />
            </div>
            <div class="flex flex-wrap gap-3">
                <StatusChip label="Frame status" value={props.status} trueLabel="OK" falseLabel="FAULT" />
            </div>
        </TelemetryCard>
    )
}

export default AttitudeCard
