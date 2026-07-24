import { Component } from "solid-js"
import { FiPlay, FiPause, FiSkipBack, FiSkipForward } from "solid-icons/fi"
import { TimelineScrubberProps } from "../../models/ui/timeline-scrubber-props"
import { formatDuration } from "../../utils/format-time"

const TimelineScrubber: Component<TimelineScrubberProps> = (props) => {
    return (
        <div class={`flex items-center gap-3 ${props.class ?? ""}`}>
            <button
                class="btn btn-sm btn-square btn-ghost"
                disabled={props.disabled}
                onClick={() => props.onSkip(-10)}
                title="Back 10s"
            >
                <FiSkipBack class="w-4 h-4" />
            </button>

            <button
                class="btn btn-sm btn-square btn-primary"
                disabled={props.disabled}
                onClick={props.onPlayPause}
                title={props.isPlaying ? "Pause" : "Play"}
            >
                {props.isPlaying ? <FiPause class="w-4 h-4" /> : <FiPlay class="w-4 h-4" />}
            </button>

            <button
                class="btn btn-sm btn-square btn-ghost"
                disabled={props.disabled}
                onClick={() => props.onSkip(10)}
                title="Forward 10s"
            >
                <FiSkipForward class="w-4 h-4" />
            </button>

            <span class="text-xs font-mono text-base-content/70 whitespace-nowrap">
                {formatDuration(props.elapsedSeconds)} / {formatDuration(props.durationSeconds)}
            </span>

            <input
                type="range"
                class="range range-primary range-xs flex-1"
                min={0}
                max={Math.max(props.durationSeconds, 0.001)}
                step={0.01}
                value={props.elapsedSeconds}
                disabled={props.disabled}
                onInput={(event) => props.onSeek(Number(event.currentTarget.value))}
            />
        </div>
    )
}

export default TimelineScrubber
