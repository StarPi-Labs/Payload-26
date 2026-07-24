import { Component } from "solid-js"
import { FiPlay, FiPause, FiSkipBack, FiSkipForward } from "solid-icons/fi"
import { TimelineScrubberProps } from "../../models/ui/timeline-scrubber-props"
import { formatDuration } from "../../utils/format-time"

const SPEED_OPTIONS = [0.25, 0.5, 1, 2, 4];

const TimelineScrubber: Component<TimelineScrubberProps> = (props) => {
    const speed = () => props.speed ?? 1;
    const duration = () => Math.max(props.durationSeconds, 0.001);

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

            <div class="relative flex-1">
                <input
                    type="range"
                    class="range range-primary range-xs w-full"
                    min={0}
                    max={duration()}
                    step={0.01}
                    value={props.elapsedSeconds}
                    disabled={props.disabled}
                    onInput={(event) => props.onSeek(Number(event.currentTarget.value))}
                />
                {(props.modeTransitions ?? []).map((transition) => (
                    <button
                        type="button"
                        class="absolute top-1/2 -translate-y-1/2 -translate-x-1/2 w-0.5 h-3 bg-warning/80 hover:bg-warning cursor-pointer"
                        style={{ left: `${Math.min(100, Math.max(0, (transition.time / duration()) * 100))}%` }}
                        title={`${transition.mode} @ ${formatDuration(transition.time)}`}
                        onClick={() => props.onSeek(transition.time)}
                    />
                ))}
            </div>

            {props.onSpeedChange && (
                <select
                    class="select select-bordered select-xs w-20"
                    disabled={props.disabled}
                    value={speed()}
                    onChange={(event) => props.onSpeedChange?.(Number(event.currentTarget.value))}
                    title="Playback speed"
                >
                    {SPEED_OPTIONS.map((option) => (
                        <option value={option}>{option}x</option>
                    ))}
                </select>
            )}
        </div>
    )
}

export default TimelineScrubber
