import { Component } from "solid-js"
import { StatusChipProps } from "../models/ui/status-chip-props"

const StatusChip: Component<StatusChipProps> = (props) => {
    const label = () => {
        if (props.value === true) return props.trueLabel ?? "OK"
        if (props.value === false) return props.falseLabel ?? "OFF"
        return props.unknownLabel ?? "UNKNOWN"
    }

    const tone = () => {
        if (props.value === true) return "badge-success"
        if (props.value === false) return "badge-error"
        return "badge-ghost"
    }

    const sizeClass = () => {
        if (!props.size || props.size === "sm") return "badge-sm"
        if (props.size === "xs") return "badge-xs"
        return "badge-md"
    }

    return (
        <div class={`flex items-center gap-2 ${props.class ?? ""}`}>
            <span class="text-xs uppercase tracking-wide text-base-content/60">{props.label}</span>
            <span class={`badge ${sizeClass()} ${tone()}`}>{label()}</span>
        </div>
    )
}

export default StatusChip
