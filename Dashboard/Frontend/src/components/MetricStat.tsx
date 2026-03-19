import { Component, Show } from "solid-js"
import { MetricStatProps } from "../models/ui/metric-stat-props"

const MetricStat: Component<MetricStatProps> = (props) => {
    const formattedValue = () => {
        const value = props.value
        if (value === null || value === undefined || value === "") return "N/A"
        if (typeof value === "number") {
            if (Number.isNaN(value)) return "N/A"
            if (typeof props.precision === "number") return value.toFixed(props.precision)
            return value.toString()
        }
        return value
    }

    return (
        <div class={`stat bg-base-100/70 border border-base-300 rounded-box p-3 ${props.class ?? ""}`}>
            <div class="stat-title text-xs uppercase tracking-wide text-base-content/60">{props.label}</div>
            <div class="stat-value text-2xl">
                {formattedValue()}
                <Show when={props.unit}>
                    <span class="text-base font-medium ml-1">{props.unit}</span>
                </Show>
            </div>
            <Show when={props.hint}>
                <div class="stat-desc">{props.hint}</div>
            </Show>
        </div>
    )
}

export default MetricStat
