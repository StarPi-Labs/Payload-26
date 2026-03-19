import { ParentComponent, Show } from "solid-js"
import { TelemetryCardProps } from "../models/ui/telemetry-card-props"

const TelemetryCard: ParentComponent<TelemetryCardProps> = (props) => {
    return (
        <div class={`card bg-base-200/70 border border-base-300 shadow-sm ${props.class ?? ""}`}>
            <div class="card-body gap-4">
                <div class="flex items-start justify-between gap-4">
                    <div>
                        <h3 class="text-lg font-semibold">{props.title}</h3>
                        <Show when={props.subtitle}>
                            <p class="text-sm text-base-content/70">{props.subtitle}</p>
                        </Show>
                    </div>
                    <Show when={props.badge}>
                        <div class="badge badge-outline text-xs">{props.badge}</div>
                    </Show>
                </div>
                <div class="space-y-4">
                    {props.children}
                </div>
            </div>
        </div>
    )
}

export default TelemetryCard
