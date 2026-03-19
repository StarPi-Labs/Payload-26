import { MetricValue } from "./metric-value"

export interface MetricStatProps {
    label: string
    value?: MetricValue
    unit?: string
    precision?: number
    hint?: string
    class?: string
}
