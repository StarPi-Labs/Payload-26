import { Component, For, Show } from "solid-js"
import { FlightSelectorProps } from "../../models/ui/flight-selector-props"
import { formatDuration } from "../../utils/format-time"

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
                            <option value={flight.id}>
                                {flight.date}
                                {flight.hasTelemetry ? ` — ${formatDuration(flight.duration)}` : " — no telemetry"}
                                {flight.cameras > 0 ? ` — ${flight.cameras} cam` : ""}
                            </option>
                        )}
                    </For>
                </select>
            </Show>
        </div>
    )
}

export default FlightSelector
