import { JSX } from "solid-js";
import { GraphLineConfig } from "./graph-line";

export interface GraphCardProps {
    title: string;
    subtitle?: string;
    newPoint?: any;
    maxPoints?: number;
    xKey: string;
    lines: GraphLineConfig[];
    controls?: JSX.Element;
    class?: string;
    /** Change this value (e.g. a counter) to clear the rolling point buffer.
     * Use when the timeline is scrubbed/seeked, so the graph doesn't draw a
     * line back to wherever it was before the jump. */
    resetKey?: any;
}