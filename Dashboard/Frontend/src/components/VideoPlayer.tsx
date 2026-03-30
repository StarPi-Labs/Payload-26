import { Component, mergeProps, Show, For, createSignal, createEffect, onCleanup, createMemo } from "solid-js";
import { MultiVideoPlayerProps } from "../models/ui/video-player-props";
import { VideoSource } from "../models/videp-source";

// serve per agganciare il ref del singolo video
const VideoTag: Component<{
    source: VideoSource;
    muted: boolean;
    objectFit: "cover" | "contain" | "fill";
    class: string;
    startAt?: number | null;
}> = (props) => {
    let videoRef: HTMLVideoElement | undefined;
    const [currentFrame, setCurrentFrame] = createSignal<string>("");

    createEffect(() => {
        // SCENARIO 1: WebSocket
        if (props.source.wsUrl) {
            const ws = new WebSocket(props.source.wsUrl);
            ws.binaryType = "blob";
            ws.onmessage = (event) => {
                if (event.data instanceof Blob) {
                    const imageUrl = URL.createObjectURL(event.data);
                    setCurrentFrame(imageUrl);
                }
            };
            onCleanup(() => ws.close());
            return;
        }

        // SCENARIO 2: WebRTC
        if (videoRef && props.source.stream) {
            videoRef.srcObject = props.source.stream;
            onCleanup(() => {
                if (videoRef) videoRef.srcObject = null;
            });
        }
    });

    return (
        <Show
            when={!props.source.wsUrl}
            fallback={
                <img
                    src={currentFrame()}
                    class={props.class}
                    style={{ "object-fit": props.objectFit }}
                    alt="Live Feed"
                />
            }
        >
            <video
                ref={videoRef}
                src={props.source.src}
                autoplay
                controls={false}
                muted={props.muted}
                loop
                playsinline
                class={props.class}
                style={{ "object-fit": props.objectFit }}
                onLoadedData={(e) => {
                    if (props.startAt && props.startAt > 0) {
                        e.currentTarget.currentTime = props.startAt + 0.7;
                    }
                }}
            />
        </Show>
    );
};


// --- COMPONENTE PRINCIPALE: VideoPlayer ---
const VideoPlayer: Component<MultiVideoPlayerProps> = (rawProps) => {
    const props = mergeProps({
        controls: false,
        autoplay: true,
        loop: true,
        muted: true,
        objectFit: "cover"
    }, rawProps);

    // Stato per tracciare la telecamera e il tempo
    const [activeId, setActiveId] = createSignal<string | undefined>(props.sources[0]?.id);
    const [syncTime, setSyncTime] = createSignal<number | null>(null);

    createEffect(() => {
        if (props.sources.length > 0 && !props.sources.find(s => s.id === activeId())) {
            setActiveId(props.sources[0].id);
        }
    });

    const activeSource = createMemo(() => props.sources.find(s => s.id === activeId()));

    return (
        <div class={`flex flex-col gap-3 h-full w-full ${props.class ?? ""}`}>

            {/* --- VIDEO PRINCIPALE --- */}
            <div class="relative w-full flex-1 rounded-2xl overflow-hidden bg-black border border-base-content/10 flex items-center justify-center">
                <Show
                    when={activeSource()}
                    fallback={<span class="text-sm text-base-content/50">Nessun segnale video</span>}
                >
                    <VideoTag
                        source={activeSource()!}
                        muted={props.muted}
                        objectFit={props.objectFit}
                        class="w-full h-full"
                        startAt={syncTime()}
                    />
                    <div class="absolute top-4 left-4 bg-black/60 text-white backdrop-blur-md px-3 py-1 rounded-md text-sm font-medium z-10">
                        {activeSource()?.label}
                    </div>
                </Show>
            </div>

            {/* --- Thumbnails --- */}
            <Show when={props.sources.length > 1}>
                <div class="flex gap-3 h-24 overflow-x-auto pb-1 w-full">
                    <For each={props.sources}>
                        {(source) => (
                            <div
                                class={`relative h-full aspect-video rounded-xl overflow-hidden cursor-pointer transition-all border-2 
                                    ${activeId() === source.id
                                        ? 'border-primary shadow-[0_0_15px_rgba(var(--p),0.5)]'
                                        : 'border-transparent opacity-60 hover:opacity-100 hover:border-base-content/30'
                                    }
                                `}
                                onClick={(e) => {
                                    const videoEl = e.currentTarget.querySelector('video');
                                    if (videoEl) {
                                        setSyncTime(videoEl.currentTime);
                                    }
                                    setActiveId(source.id);
                                }}
                            >
                                <VideoTag
                                    source={source}
                                    muted={true}
                                    objectFit="cover"
                                    class="w-full h-full pointer-events-none"
                                />
                                <div class="absolute bottom-1 left-1 bg-black/60 text-white px-2 py-0.5 rounded text-[10px] truncate max-w-[90%] z-10">
                                    {source.label}
                                </div>
                            </div>
                        )}
                    </For>
                </div>
            </Show>
        </div>
    );
};

export default VideoPlayer;