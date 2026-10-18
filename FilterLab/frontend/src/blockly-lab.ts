/**
 * The Blockly side of the lab: blocks built from the server's catalog, the
 * toolbox, the theme, and turning the block stack into a recipe and back.
 *
 * Every block is a statement block, so blocks snap under each other like a
 * Scratch script, read top to bottom. The Recording block is the "hat" that
 * starts the stack; blocks not attached under it are greyed out and do not run.
 */
import * as Blockly from "blockly/core";
import * as En from "blockly/msg/en";
import { BlockDef, Catalog, ParamDef, RecipeBlock, Recording } from "./types";

Blockly.setLocale(En as unknown as { [key: string]: string });
// Blockly's default scrollbars are thick enough for touch screens; these are for a mouse.
Blockly.Scrollbar.scrollbarThickness = 8;

const PREFIX = "lab_";
const CHAIN = "lab";

/** What the dynamic dropdowns offer; refreshed after every run. */
export const options = {
    recordings: [] as Recording[],
    /** block id -> signal names available to that block (what the blocks above produced) */
    before: new Map<string, string[]>(),
    /** every signal name seen in the last run, for blocks it did not reach */
    all: [] as string[],
    /** values a loaded recipe uses, so a dropdown accepts them before the first run */
    known: new Set<string>(),
};

function globToRegex(pattern: string): RegExp[] {
    return pattern.split(",").map((p) => p.trim()).filter(Boolean).map((p) =>
        new RegExp("^" + p.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*").replace(/\?/g, ".") + "$"));
}

function matches(name: string, filter?: string) {
    if (!filter) return true;
    return globToRegex(filter).some((r) => r.test(name));
}

function available(block: Blockly.Block | null): string[] {
    if (block && options.before.has(block.id)) return options.before.get(block.id)!;
    return options.all;
}

/** Dropdown entries for a signal parameter: what exists above this block. */
function signalOptions(field: Blockly.FieldDropdown, p: ParamDef, multi: boolean): Blockly.MenuOption[] {
    const names = available(field.getSourceBlock()).filter((n) => matches(n, p.filter));
    const out: string[] = [];
    if (p.optional) out.push("(none)");
    if (multi) {
        // patterns first: whole groups at once
        const groups = new Set<string>();
        for (const n of names) {
            const dot = n.indexOf(".");
            if (dot > 0) groups.add(n.slice(0, dot) + ".*");
        }
        const axes = [["imu.a*", ["imu.ax", "imu.ay", "imu.az"]], ["imu.g*", ["imu.gx", "imu.gy", "imu.gz"]],
                      ["nav.a*", ["nav.ax", "nav.ay", "nav.az"]]] as const;
        for (const [pat, members] of axes) {
            if (members.every((m) => names.includes(m))) out.push(pat);
        }
        out.push(...[...groups].filter((g) => !out.includes(g)));
    }
    out.push(...names);
    // Keep the current value and the default selectable even before a run has
    // produced them, and any value a loaded recipe uses.
    const keep = [field.getValue(), String(p.default ?? ""),
                  ...[...options.known].filter((v) => multi || matches(v, p.filter))];
    for (const v of keep) {
        if (v && !out.includes(v)) out.push(v);
    }
    if (!out.length) out.push("(none)");
    return out.map((v) => [v, v] as Blockly.MenuOption);
}

function recordingOptions(field: Blockly.FieldDropdown): Blockly.MenuOption[] {
    const out: Blockly.MenuOption[] = options.recordings.map((r) => [`${r.group}: ${r.label}`, r.id]);
    const v = field.getValue();
    if (v && !options.recordings.some((r) => r.id === v)) out.unshift([v, v]);
    if (!out.length) out.push(["(no recordings)", ""]);
    else if (!v) out.unshift(["(pick a recording)", ""]);
    return out;
}

function makeField(p: ParamDef): Blockly.Field {
    switch (p.kind) {
        case "number":
        case "int": {
            const f = new Blockly.FieldNumber(Number(p.default ?? 0), p.min ?? -Infinity, p.max ?? Infinity,
                                              p.kind === "int" ? 1 : (p.precision ?? 0));
            return f;
        }
        case "bool":
            return new Blockly.FieldCheckbox(p.default ? "TRUE" : "FALSE");
        case "choice":
            return new Blockly.FieldDropdown((p.options ?? []).map(([v, label]) => [label, v] as Blockly.MenuOption));
        case "text":
            return new Blockly.FieldTextInput(String(p.default ?? ""));
        case "recording":
            return new Blockly.FieldDropdown(function (this: Blockly.FieldDropdown) {
                return recordingOptions(this);
            });
        case "signal":
        case "signals": {
            const multi = p.kind === "signals";
            return new Blockly.FieldDropdown(function (this: Blockly.FieldDropdown) {
                return signalOptions(this, p, multi);
            });
        }
    }
}

type Part = { text: string } | { param: string };

function parseTemplate(row: string): Part[] {
    const parts: Part[] = [];
    const re = /\{(\w+)\}/g;
    let last = 0;
    let m: RegExpExecArray | null;
    while ((m = re.exec(row))) {
        if (m.index > last) parts.push({ text: row.slice(last, m.index).trim() });
        parts.push({ param: m[1] });
        last = m.index + m[0].length;
    }
    if (last < row.length) parts.push({ text: row.slice(last).trim() });
    return parts.filter((p) => !("text" in p) || p.text.length > 0);
}

/** A lab block; labRefresh re-applies which lines are shown (events are off while loading). */
type LabBlock = Blockly.Block & { labRefresh?: () => void };

export function firstParagraph(help: string) {
    return help.trim().split(/\n\s*\n/)[0].replace(/\s+/g, " ");
}

/** Define one Blockly block per catalog entry. */
export function defineBlocks(catalog: Catalog) {
    const colours = new Map(catalog.categories.map((c) => [c.id, c.colour]));
    for (const def of catalog.blocks) {
        const params = new Map(def.params.map((p) => [p.name, p]));
        Blockly.Blocks[PREFIX + def.type] = {
            init(this: Blockly.Block) {
                const rows: ParamDef[][] = [];
                def.template.split("|").forEach((row, i) => {
                    const input = this.appendDummyInput(`ROW${i}`);
                    const inRow: ParamDef[] = [];
                    rows.push(inRow);
                    for (const part of parseTemplate(row)) {
                        if ("text" in part) {
                            input.appendField(part.text);
                        } else {
                            const p = params.get(part.param)!;
                            inRow.push(p);
                            const field = makeField(p);
                            input.appendField(field, p.name);
                            if (p.kind === "choice" || p.kind === "signal" || p.kind === "signals") {
                                try {
                                    field.setValue(String(p.default ?? ""));
                                } catch {
                                    // default not offered (yet): the first option stays
                                }
                            }
                        }
                    }
                });
                if (def.hat) {
                    (this as Blockly.Block & { hat?: string }).hat = "cap";
                    this.setDeletable(false);
                } else {
                    this.setPreviousStatement(true, CHAIN);
                }
                this.setNextStatement(true, CHAIN);
                this.setColour(colours.get(def.category) ?? "#888888");
                this.setTooltip(firstParagraph(def.help));

                // A line whose settings only apply to some choices (the Mahony
                // gains, typed calibration numbers...) is hidden otherwise.
                const conditional = rows.map((ps) => ps.length > 0 && ps.every((p) => p.showIf));
                if (conditional.some(Boolean)) {
                    const update = () => {
                        let changed = false;
                        rows.forEach((ps, i) => {
                            if (!conditional[i]) return;
                            const visible = ps.some((p) => Object.entries(p.showIf!).every(
                                ([other, values]) => values.includes(String(this.getFieldValue(other) ?? ""))));
                            const input = this.getInput(`ROW${i}`);
                            if (input && input.isVisible() !== visible) {
                                input.setVisible(visible);
                                changed = true;
                            }
                        });
                        const svg = this as Blockly.BlockSvg;
                        if (changed && svg.rendered) void svg.queueRender();
                    };
                    this.setOnChange(update);
                    (this as LabBlock).labRefresh = update;
                    update();
                }
            },
        };
    }
}

export function toolbox(catalog: Catalog): Blockly.utils.toolbox.ToolboxDefinition {
    return {
        kind: "categoryToolbox",
        contents: catalog.categories
            .map((c) => ({
                kind: "category",
                name: c.label,
                colour: c.colour,
                contents: catalog.blocks
                    .filter((b) => b.category === c.id && !b.hat)
                    .map((b) => ({ kind: "block", type: PREFIX + b.type })),
            }))
            .filter((c) => c.contents.length > 0),
    } as Blockly.utils.toolbox.ToolboxDefinition;
}

export const theme = Blockly.Theme.defineTheme("pilog-dark", {
    name: "pilog-dark",
    base: Blockly.Themes.Zelos,
    componentStyles: {
        workspaceBackgroundColour: "#1d232a",
        toolboxBackgroundColour: "#15191e",
        toolboxForegroundColour: "#e5e7eb",
        flyoutBackgroundColour: "#252b33",
        flyoutForegroundColour: "#e5e7eb",
        flyoutOpacity: 0.97,
        scrollbarColour: "#4b5563",
        scrollbarOpacity: 0.6,
        insertionMarkerColour: "#ffffff",
        insertionMarkerOpacity: 0.3,
        selectedGlowColour: "#fde047",
    },
    fontStyle: { family: "ui-sans-serif, system-ui, sans-serif", weight: "500", size: 11 },
});

// ---- recipe <-> workspace ----------------------------------------------------

export function hatBlock(ws: Blockly.Workspace) {
    return ws.getTopBlocks(true).find((b) => b.type === PREFIX + "source") ?? null;
}

export function chain(ws: Blockly.Workspace): Blockly.Block[] {
    const out: Blockly.Block[] = [];
    let b = hatBlock(ws);
    while (b) {
        out.push(b);
        b = b.getNextBlock();
    }
    return out;
}

export function readRecipe(ws: Blockly.Workspace, catalog: Catalog): RecipeBlock[] {
    const defs = new Map(catalog.blocks.map((d) => [d.type, d]));
    return chain(ws).map((b) => {
        const type = b.type.slice(PREFIX.length);
        const def = defs.get(type);
        const params: Record<string, unknown> = {};
        for (const p of def?.params ?? []) {
            const v = b.getFieldValue(p.name);
            params[p.name] = p.kind === "number" || p.kind === "int" ? Number(v)
                : p.kind === "bool" ? v === "TRUE" : v;
        }
        return { id: b.id, type, params, enabled: b.isEnabled() };
    });
}

/** Replace the workspace with a recipe, stacked under the Recording block. */
export function loadRecipe(ws: Blockly.WorkspaceSvg, catalog: Catalog, blocks: RecipeBlock[]) {
    const defs = new Map(catalog.blocks.map((d) => [d.type, d]));
    for (const b of blocks) {
        for (const p of defs.get(b.type)?.params ?? []) {
            if ((p.kind === "signal" || p.kind === "signals") && b.params[p.name]) {
                options.known.add(String(b.params[p.name]));
            }
        }
    }
    Blockly.Events.disable();
    try {
        // Put the stack where the user is looking, not wherever the canvas
        // origin happens to be after scrolling around.
        const m = ws.getMetrics();
        const origin = { x: m.viewLeft / ws.scale + 24, y: m.viewTop / ws.scale + 24 };
        ws.clear();
        let prev: Blockly.BlockSvg | null = null;
        const list = blocks.length && blocks[0].type === "source" ? blocks
            : [{ type: "source", params: {}, enabled: true }, ...blocks];
        for (const b of list) {
            if (!defs.has(b.type)) continue;
            const block = ws.newBlock(PREFIX + b.type) as Blockly.BlockSvg;
            for (const p of defs.get(b.type)!.params) {
                const v = b.params[p.name];
                if (v === undefined || v === null) continue;
                try {
                    block.setFieldValue(p.kind === "bool" ? (v ? "TRUE" : "FALSE") : String(v), p.name);
                } catch {
                    // a value this version no longer offers: keep the default
                }
            }
            if (b.enabled === false) block.setDisabledReason(true, "MANUALLY_DISABLED");
            (block as LabBlock).labRefresh?.();
            block.initSvg();
            block.render();
            if (prev) {
                prev.nextConnection!.connect(block.previousConnection!);
            } else {
                block.moveBy(origin.x, origin.y);
            }
            prev = block;
        }
    } finally {
        Blockly.Events.enable();
    }
}

export { Blockly };
