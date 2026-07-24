import { Component } from "solid-js"
import { AttitudeCardProps } from "../models/ui/attitude-card-props"
import TelemetryCard from "./base/TelemetryCard"
import MetricStat from "./base/MetricStat"
import StatusChip from "./base/StatusChip"

const AttitudeCard: Component<AttitudeCardProps> = (props) => {
    return (
        <TelemetryCard
            title="Attitude"
            subtitle="Roll, pitch, yaw, acceleration"
            badge={props.timestampLabel}
            class={props.class}
        >
            <div class="grid grid-cols-1 sm:grid-cols-3 lg:grid-cols-6 gap-3">
                <MetricStat label="Roll" value={props.roll} unit="deg" precision={1} />
                <MetricStat label="Pitch" value={props.pitch} unit="deg" precision={1} />
                <MetricStat label="Yaw" value={props.yaw} unit="deg" precision={1} />
                <MetricStat label="Accel X" value={props.accelX} unit="m/s²" precision={2} />
                <MetricStat label="Accel Y" value={props.accelY} unit="m/s²" precision={2} />
                <MetricStat label="Accel Z" value={props.accelZ} unit="m/s²" precision={2} />
            </div>
            <div class="flex flex-wrap gap-3">
                <StatusChip label="Frame status" value={props.status} trueLabel="OK" falseLabel="FAULT" />
            </div>
        </TelemetryCard>
    )
}

export default AttitudeCard
