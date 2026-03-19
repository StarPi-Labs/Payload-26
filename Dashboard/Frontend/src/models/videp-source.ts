export interface VideoSource {
    id: string;         // id: 'cam-front'
    label: string;      // Etichetta: 'Telecamera Frontale'
    src?: string;       // MP4 di test
    wsUrl?: string;  // per WebSocket!
    stream?: MediaStream | null; // per WebRTC
}