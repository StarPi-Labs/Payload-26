import { Component } from "solid-js"
import { FiPlay, FiPause, FiSkipBack, FiSkipForward } from "solid-icons/fi"
import { TimelineScrubberProps } from "../../models/ui/timeline-scrubber-props"
import { formatDuration } from "../../utils/format-time"

const DEFAULT_SPEED_OPTIONS = [0.25, 0.5, 1, 2, 4];

/** "10s", "5 min", "1 h" - for the skip button tooltips. */
function formatStep(seconds: number): string {
    if (seconds >= 3600 && seconds % 3600 === 0) return `${seconds / 3600} h`
    if (seconds >= 60 && seconds % 60 === 0) return `${seconds / 60} min`
    return `${seconds}s`
}

function formatSpeed(speed: number): string {
    return speed >= 1000 ? `${speed / 1000}kx` : `${speed}x`
}

const TimelineScrubber: Component<TimelineScrubberProps> = (props) => {
    const speed = () => props.speed ?? 1;
    const duration = () => Math.max(props.durationSeconds, 0.001);
    const step = () => props.skipSeconds ?? 10;
    const speeds = () => props.speedOptions ?? DEFAULT_SPEED_OPTIONS;

    return (
        <div class={`flex items-center gap-3 ${props.class ?? ""}`}>
            <button
                class="btn btn-sm btn-square btn-ghost"
                disabled={props.disabled}
                onClick={() => props.onSkip(-step())}
                title={`Back ${formatStep(step())}`}
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
                onClick={() => props.onSkip(step())}
                title={`Forward ${formatStep(step())}`}
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
                    {speeds().map((option) => (
                        <option value={option}>{formatSpeed(option)}</option>
                    ))}
                </select>
            )}
        </div>
    )
}

export default TimelineScrubber
