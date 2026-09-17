import { Component, For, Show } from "solid-js"
import { FlightSelectorProps, FlightSummary } from "../../models/ui/flight-selector-props"
import { formatDuration } from "../../utils/format-time"
import { formatBytes } from "../../utils/telemetry-series"

/** Short, human status suffix for one flight in the dropdown. */
function describe(flight: FlightSummary): string {
    if (flight.status === "error") return " — unreadable"
    const parts: string[] = []
    parts.push(flight.hasTelemetry ? formatDuration(flight.duration) : "no telemetry")
    if (flight.cameras > 0) parts.push(`${flight.cameras} cam`)

    // Large recordings are worth flagging: their first load has to build a
    // server-side cache, so the user knows why it is not instant.
    if ((flight.telemetryBytes ?? 0) >= 100 * 1024 * 1024) {
        parts.push(formatBytes(flight.telemetryBytes))
    }
    const state = flight.cache?.state
    if (state === "building") {
        parts.push(`preparing ${Math.round((flight.cache?.progress ?? 0) * 100)}%`)
    } else if (state === "error") {
        parts.push("cache failed")
    }
    return " — " + parts.join(" — ")
}

const FlightSelector: Component<FlightSelectorProps> = (props) => {
    return (
        <div class={`flex items-center gap-2 ${props.class ?? ""}`}>
            <span class="text-xs uppercase tracking-wide text-base-content/60 whitespace-nowrap">Flight</span>
            <Show
                when={props.flights.length > 0}
                fallback={<span class="text-sm text-base-content/50">No flights found</span>}
            >
                <select
                    class="select select-sm select-bordered min-w-48"
                    value={props.selectedId ?? ""}
                    onChange={(event) => props.onSelect(event.currentTarget.value)}
                >
                    <For each={props.flights}>
                        {(flight) => (
                            <option value={flight.id} disabled={flight.status === "error"}>
                                {flight.date}{describe(flight)}
                            </option>
                        )}
                    </For>
                </select>
            </Show>
        </div>
    )
}

export default FlightSelector
