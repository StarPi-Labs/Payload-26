/** Shapes of what lab_server.py sends and receives. */

export interface ParamDef {
    name: string;
    kind: "number" | "int" | "bool" | "choice" | "signal" | "signals" | "text" | "recording";
    default: unknown;
    help: string;
    unit: string;
    min?: number;
    max?: number;
    precision?: number;
    options?: [string, string][];
    filter?: string;
    optional?: boolean;
    /** only relevant when these other parameters have one of these values */
    showIf?: Record<string, string[]>;
}

export interface BlockDef {
    type: string;
    category: string;
    title: string;
    template: string;
    help: string;
    hat: boolean;
    params: ParamDef[];
}

export interface Category {
    id: string;
    label: string;
    colour: string;
}

export interface Catalog {
    categories: Category[];
    blocks: BlockDef[];
}

export interface Recording {
    id: string;
    kind: "raw" | "dashboard";
    label: string;
    group: string;
    size: number;
    modified: number;
}

/** One block of a recipe, as stored and as sent to /api/run. */
export interface RecipeBlock {
    id?: string;
    type: string;
    params: Record<string, unknown>;
    enabled: boolean;
}

export interface Recipe {
    name?: string;
    blocks: RecipeBlock[];
}

export interface Preset {
    id: string;
    name: string;
    description: string;
    blocks: RecipeBlock[];
}

export interface SignalInfo {
    name: string;
    unit: string;
    desc: string;
    axis: "time" | "frequency" | "tau";
    step: boolean;
    n: number;
    x0?: number;
    x1?: number;
    rate?: number;
}

/** The result of one block of a run. */
export interface Step {
    id?: string;
    key: string;
    type: string;
    ok: boolean;
    error: string | null;
    trace?: string;
    changed: string[];
    notes: string[];
    warnings: string[];
    table: string[][] | null;
    download: { name: string; content: string } | null;
    ms: number;
    cached: boolean;
    signals: SignalInfo[];
    /** failed, but after an earlier block failed: most likely a consequence */
    afterError?: boolean;
    /** not run at all, because the Recording block failed */
    notRun?: boolean;
}

export interface Series extends SignalInfo {
    x: number[];
    y: (number | null)[];
    decimated: boolean;
}
