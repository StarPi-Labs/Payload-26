export interface RocketMapCardProps {
    latitude: number;
    longitude: number;
    gpsFix: boolean;
    class?: string;
    /** Change this value (e.g. a counter) when the loaded flight changes, so
     * the trajectory/start marker are re-anchored at the new flight's first
     * position instead of drawing a line back to the previous one. */
    resetKey?: any;
}