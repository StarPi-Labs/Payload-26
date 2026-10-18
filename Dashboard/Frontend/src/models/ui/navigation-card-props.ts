export interface NavigationCardProps {
    altitude?: number | null
    altitudeMSL?: number | null
    gpsAltitude?: number | null
    verticalVelocity?: number | null
    horizontalVelocity?: number | null
    latitude?: number | null
    longitude?: number | null
    /** satellites used in that fix */
    satellites?: number | null
    gpsFix?: boolean | null
    class?: string
}
