import { InertialSample } from "./inertial-sample"

export interface AtmosphericSample extends InertialSample {
    alt: number
    altMsl: number
    galt: number
    vvel: number
    hvel: number
    lat: number
    long: number
    gps: boolean
    /** satellites used in the current fix; null when the flight's file has no gpsSats column */
    sats: number | null
    temp: number
    pres: number
    rh: number
    accelX: number
    accelY: number
    accelZ: number
    gasRes: number
    busVolt: number
    current: number
    power: number
    /** Orientation quaternion, body (CAD frame) -> pad frame (X towards the
     * viewer, Y right, Z up). Derived by the backend; all zero when the
     * flight has none. */
    qw: number
    qx: number
    qy: number
    qz: number
    /** Acceleration in that pad frame, gravity removed, m/s^2 (backend-derived). */
    padAx: number
    padAy: number
    padAz: number
}
