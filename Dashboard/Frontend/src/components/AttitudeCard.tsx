import { Component } from "solid-js"
import { AttitudeCardProps } from "../models/ui/attitude-card-props"
import TelemetryCard from "./base/TelemetryCard"
import MetricStat from "./base/MetricStat"
import StatusChip from "./base/StatusChip"

const AttitudeCard: Component<AttitudeCardProps> = (props) => {
    return (
        <TelemetryCard
            title="Attitude"
            subtitle="Acceleration and turn rates · X front, Y right, Z nose"
            badge={props.timestampLabel}
            class={props.class}
        >
            <div class="grid grid-cols-1 sm:grid-cols-3 lg:grid-cols-6 gap-3">
                <MetricStat label="Accel X" value={props.accelX} unit="m/s²" precision={2} />
                <MetricStat label="Accel Y" value={props.accelY} unit="m/s²" precision={2} />
                <MetricStat label="Accel Z" value={props.accelZ} unit="m/s²" precision={2} />
                {/* The telemetry's roll/pitch/yaw columns are gyro RATES about the
                    payload's X/Y/Z axes (frameparser's names), not angles. */}
                <MetricStat label="Rate X" value={props.roll} unit="°/s" precision={1} />
                <MetricStat label="Rate Y" value={props.pitch} unit="°/s" precision={1} />
                <MetricStat label="Rate Z (spin)" value={props.yaw} unit="°/s" precision={1} />
            </div>
            <div class="flex flex-wrap gap-3">
                <StatusChip label="Frame status" value={props.status} trueLabel="OK" falseLabel="FAULT" />
            </div>
        </TelemetryCard>
    )
}

export default AttitudeCard
