import { ParentProps } from "solid-js"

export interface TelemetryCardProps extends ParentProps {
    title: string
    subtitle?: string
    badge?: string
    class?: string
}
