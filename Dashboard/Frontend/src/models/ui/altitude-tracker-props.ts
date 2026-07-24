export interface AltitudeTrackerProps {
    currentAltitude: number;
    targetAltitude: number;
    maxAltitude?: number;
    /** GPS-derived altitude above the pad (gpsAltitude - departureAltitude),
     * on the same scale as currentAltitude. Pass undefined/null when there's
     * no GPS fix yet -- the marker is hidden rather than showing a bogus
     * value. Lets you compare the barometric reading (rocket icon) against
     * the GPS one (arrow) at a glance. */
    gpsAltitude?: number | null;
    class?: string;
}