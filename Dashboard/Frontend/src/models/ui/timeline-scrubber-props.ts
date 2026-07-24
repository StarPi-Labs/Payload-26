export interface ModeTransition {
    time: number;
    mode: string;
}

export interface TimelineScrubberProps {
    elapsedSeconds: number;
    durationSeconds: number;
    isPlaying: boolean;
    disabled?: boolean;
    onPlayPause: () => void;
    onSeek: (seconds: number) => void;
    onSkip: (deltaSeconds: number) => void;
    /** Playback rate multiplier, e.g. 0.25 / 0.5 / 1 / 2 / 4. Defaults to 1 if omitted. */
    speed?: number;
    onSpeedChange?: (speed: number) => void;
    /** Points in the flight where the state machine changed mode, rendered as
     * ticks along the scrubber track. */
    modeTransitions?: ModeTransition[];
    class?: string;
}
