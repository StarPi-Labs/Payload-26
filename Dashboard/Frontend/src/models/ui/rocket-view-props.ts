export interface RocketViewProps {
    /** Flight whose rocket model to draw (falls back to a generic rocket). */
    flightId: string | null;
    /** Orientation quaternion body -> pad frame, as in AtmosphericSample.
     * All zero = no attitude data: the rocket is drawn upright and dimmed. */
    qw: number;
    qx: number;
    qy: number;
    qz: number;
    /** Acceleration in the pad frame (X towards the viewer, Y right, Z up),
     * gravity removed, m/s^2 - pushes the rocket off centre. */
    accelX: number;
    accelY: number;
    accelZ: number;
    class?: string;
}

/** What GET /api/flights/<id>/model returns. */
export interface RocketModelConfig {
    url: string;
    /** Model axis pointing to the nose, e.g. "+Y". */
    noseAxis: string;
    /** Model axis pointing away from the launch rail (the rocket's front), e.g. "+Z". */
    frontAxis: string;
    /** Point the rocket turns about, in model units. Default: bounding-box centre. */
    pivot?: [number, number, number];
}
