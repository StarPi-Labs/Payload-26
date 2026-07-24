export interface FlightSummary {
    id: string;
    date: string;
    duration: number;
    status: string;
    cameras: number;
    hasTelemetry: boolean;
}

export interface FlightSelectorProps {
    flights: FlightSummary[];
    selectedId: string | null;
    onSelect: (id: string) => void;
    class?: string;
}
