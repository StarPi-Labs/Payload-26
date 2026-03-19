import { VideoSource } from "../videp-source";
export interface MultiVideoPlayerProps {
    sources: VideoSource[];

    controls?: boolean;
    autoplay?: boolean;
    loop?: boolean;
    muted?: boolean; // Applica il muto al video principale (le miniature sono sempre mute)
    class?: string;
    objectFit?: "cover" | "contain" | "fill";
}