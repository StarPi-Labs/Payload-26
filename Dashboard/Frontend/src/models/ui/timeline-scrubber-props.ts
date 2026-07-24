export interface TimelineScrubberProps {
    elapsedSeconds: number;
    durationSeconds: number;
    isPlaying: boolean;
    disabled?: boolean;
    onPlayPause: () => void;
    onSeek: (seconds: number) => void;
    onSkip: (deltaSeconds: number) => void;
    class?: string;
}
