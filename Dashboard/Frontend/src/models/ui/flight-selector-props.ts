export interface FlightCacheStatus {
    /** 'ready' | 'building' | 'missing' | 'error' | 'none' */
    state: string;
    progress?: number;
    error?: string;
}

export interface FlightSummary {
    id: string;
    date: string;
    duration: number;
    status: string;
    cameras: number;
    hasTelemetry: boolean;
    /** Total size of the flight's telemetry files, in bytes. */
    telemetryBytes?: number;
    /** Server-side column cache state; large recordings need one before they can load. */
    cache?: FlightCacheStatus;
    /** Present when the server could not summarise this flight at all. */
    error?: string;
}

export interface FlightSelectorProps {
    flights: FlightSummary[];
    selectedId: string | null;
    onSelect: (id: string) => void;
    class?: string;
}
