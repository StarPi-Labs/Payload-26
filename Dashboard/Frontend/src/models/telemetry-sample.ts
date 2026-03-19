import { AtmosphericSample } from "./atmospheric-sample"
import { InertialSample } from "./inertial-sample"

export type TelemetrySample = InertialSample | AtmosphericSample
