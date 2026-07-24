import { MetricValue } from "./metric-value"

export interface MetricStatProps {
    label: string
    value?: MetricValue
    unit?: string
    precision?: number
    hint?: string
    class?: string
    /** Small arrow shown next to the value, e.g. to indicate whether it's
     * currently rising, falling, or holding steady. */
    trend?: "up" | "down" | "flat"
}
