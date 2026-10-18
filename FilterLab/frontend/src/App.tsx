import { Component, createEffect, createMemo, createSignal, For, JSX, on, onCleanup, onMount, Show } from "solid-js";
import { api } from "./api";
import { Blockly, chain, defineBlocks, firstParagraph, hatBlock, loadRecipe, options, readRecipe, theme, toolbox } from "./blockly-lab";
import Chart, { PALETTE } from "./Chart";
import { BlockDef, Catalog, Preset, RecipeBlock, Recording, Series, SignalInfo, Step } from "./types";

const LOCAL_RECIPE = "pilog-lab-recipe";
const LOCAL_HELP_SEEN = "pilog-lab-help-seen";
const MAX_CHOSEN = 8;

function storageGet(key: string): string | null {
    try {
        return localStorage.getItem(key);
    } catch {
        return null;
    }
}

function storageSet(key: string, value: string) {
    try {
        localStorage.setItem(key, value);
    } catch {
        // private window or storage blocked: the recipe just is not remembered
    }
}

function download(name: string, blob: Blob) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
}

function formatBytes(n: number) {
    if (n < 1024) return `${n} B`;
    const u = ["KB", "MB", "GB"];
    let v = n / 1024, i = 0;
    while (v >= 1024 && i < u.length - 1) {
        v /= 1024;
        i++;
    }
    return `${v.toFixed(v >= 10 ? 0 : 1)} ${u[i]}`;
}

/** The catalog's help text: paragraphs, "- " bullet lists and `code`. */
function HelpText(props: { text: string }) {
    const inline = (s: string): JSX.Element[] =>
        s.split(/(`[^`]+`)/g).map((part) =>
            part.startsWith("`") && part.endsWith("`") ? <code class="px-1 rounded bg-base-300">{part.slice(1, -1)}</code> : <>{part}</>);
    const blocks = () => props.text.trim().split(/\n\s*\n/);
    return (
        <div class="help-text text-sm leading-relaxed text-base-content/80">
            <For each={blocks()}>
                {(para) => {
                    const lines = para.split("\n").map((l) => l.trim()).filter(Boolean);
                    if (lines.length && lines[0].startsWith("- ")) {
                        const items: string[] = [];
                        for (const l of lines) {
                            if (l.startsWith("- ")) items.push(l.slice(2));
                            else items[items.length - 1] += " " + l;
                        }
                        return <ul class="list-disc pl-5 space-y-1 my-2"><For each={items}>{(it) => <li>{inline(it)}</li>}</For></ul>;
                    }
                    return <p>{inline(lines.join(" "))}</p>;
                }}
            </For>
        </div>
    );
}

const App: Component = () => {
    const [catalog, setCatalog] = createSignal<Catalog | null>(null);
    const [recordings, setRecordings] = createSignal<Recording[]>([]);
    const [presets, setPresets] = createSignal<Preset[]>([]);
    const [steps, setSteps] = createSignal<Step[]>([]);
    const [running, setRunning] = createSignal(false);
    const [runError, setRunError] = createSignal<string | null>(null);
    const [selectedId, setSelectedId] = createSignal<string | null>(null);
    const [chosen, setChosen] = createSignal<Record<string, string[]>>({});
    const [series, setSeries] = createSignal<Series[]>([]);
    const [before, setBefore] = createSignal<Series[]>([]);
    const [showBefore, setShowBefore] = createSignal(true);
    const [range, setRange] = createSignal<[number, number] | null>(null);
    const [recording, setRecording] = createSignal("");
    const [toast, setToast] = createSignal<string | null>(null);
    const [showHelp, setShowHelp] = createSignal(storageGet(LOCAL_HELP_SEEN) !== "1");
    const [savedRecipes, setSavedRecipes] = createSignal<{ name: string }[]>([]);
    const [loadError, setLoadError] = createSignal<string | null>(null);

    let host: HTMLDivElement | undefined;
    let ws: Blockly.WorkspaceSvg | null = null;
    let runTimer: number | undefined;
    let runAbort: AbortController | null = null;
    let dirtyWhileRunning = false;
    let lastRun = "";
    let seriesAbort: AbortController | null = null;
    let fileInput: HTMLInputElement | undefined;
    let recipeInput: HTMLInputElement | undefined;
    let toastTimer: number | undefined;

    const notify = (msg: string) => {
        setToast(msg);
        clearTimeout(toastTimer);
        toastTimer = window.setTimeout(() => setToast(null), 4000);
    };

    const defs = createMemo(() => new Map((catalog()?.blocks ?? []).map((b) => [b.type, b])));
    const colours = createMemo(() => new Map((catalog()?.categories ?? []).map((c) => [c.id, c.colour])));

    // ---- running -------------------------------------------------------------

    function scheduleRun(delay = 350) {
        clearTimeout(runTimer);
        runTimer = window.setTimeout(runNow, delay);
    }

    async function runNow() {
        const cat = catalog();
        if (!ws || !cat) return;
        if (running()) {
            dirtyWhileRunning = true;
            return;
        }
        const blocks = readRecipe(ws, cat);
        const signature = JSON.stringify(blocks);
        if (signature === lastRun && steps().length) return;   // e.g. a block moved, nothing changed
        storageSet(LOCAL_RECIPE, JSON.stringify({ blocks }));
        const rec = String(blocks[0]?.params.recording ?? "");
        setRecording(rec);
        if (!rec) {
            setSteps([]);
            setRunError("Pick a recording - in the bar above or in the Recording block.");
            return;
        }
        setRunning(true);
        setRunError(null);
        runAbort = new AbortController();
        try {
            const result = await api.run(blocks, runAbort.signal);
            lastRun = signature;
            applySteps(result);
        } catch (e) {
            if ((e as Error).name !== "AbortError") setRunError((e as Error).message);
        } finally {
            setRunning(false);
            if (dirtyWhileRunning) {
                dirtyWhileRunning = false;
                scheduleRun(50);
            }
        }
    }

    function applySteps(result: Step[]) {
        options.before.clear();
        result.forEach((s, i) => {
            if (s.id) options.before.set(s.id, i > 0 ? result[i - 1].signals.map((x) => x.name) : []);
        });
        options.all = result.length ? result[result.length - 1].signals.map((x) => x.name) : [];
        for (const s of result) {
            const block = s.id ? ws?.getBlockById(s.id) : null;
            if (!block) continue;
            const text = s.notRun ? null
                : s.error ? `${s.afterError ? "✖ (probably because a block above failed) " : "✖ "}${s.error}`
                : s.warnings.length ? s.warnings.map((w) => `⚠ ${w}`).join("\n\n") : null;
            block.setWarningText(text);
        }
        setSteps(result);
    }

    const runSummary = createMemo(() => {
        const list = steps();
        const failed = list.filter((s) => !s.ok && !s.afterError);
        const affected = list.filter((s) => s.afterError).length;
        const notRun = list.filter((s) => s.notRun).length;
        if (!failed.length) return { ok: true, text: `${list.length} blocks ok` };
        const name = defs().get(failed[0].type)?.title ?? failed[0].type;
        const more = [failed.length > 1 ? `${failed.length - 1} more failed` : "",
                      affected ? `${affected} affected` : "", notRun ? `${notRun} not run` : ""].filter(Boolean);
        return { ok: false, text: `"${name}" failed${more.length ? " · " + more.join(" · ") : ""}` };
    });

    // While a run takes long (the first opening of a big recording), say why.
    const [elapsed, setElapsed] = createSignal(0);
    let elapsedTimer: number | undefined;
    createEffect(on(running, (isRunning) => {
        clearInterval(elapsedTimer);
        setElapsed(0);
        if (isRunning) {
            const t0 = Date.now();
            elapsedTimer = window.setInterval(() => setElapsed(Math.round((Date.now() - t0) / 1000)), 1000);
        }
    }));
    const bigRecording = createMemo(() => (recordings().find((r) => r.id === recording())?.size ?? 0) > 100e6);

    // ---- selection & chart ------------------------------------------------------

    const selectedIndex = createMemo(() => {
        const list = steps();
        const id = selectedId();
        const i = id ? list.findIndex((s) => s.id === id) : -1;
        return i >= 0 ? i : list.length - 1;
    });
    const selected = createMemo<Step | null>(() => steps()[selectedIndex()] ?? null);
    const previous = createMemo<Step | null>(() => (selectedIndex() > 0 ? steps()[selectedIndex() - 1] : null));
    const selectedDef = createMemo<BlockDef | undefined>(() => (selected() ? defs().get(selected()!.type) : undefined));

    const shownSignals = createMemo<string[]>(() => {
        const step = selected();
        if (!step) return [];
        const names = new Set(step.signals.map((s) => s.name));
        const mine = chosen()[step.id ?? ""];
        if (mine) return mine.filter((n) => names.has(n));
        if (step.type === "source") {
            // everything is "new" at the source: start with something readable
            const acc = ["imu.ax", "imu.ay", "imu.az"].filter((n) => names.has(n));
            return acc.length ? acc : step.signals.slice(0, 3).map((s) => s.name);
        }
        const changed = step.changed.filter((n) => names.has(n));
        if (changed.length) return changed.slice(0, MAX_CHOSEN);
        // nothing new here (statistics, a switched-off block): what the block above showed
        const prev = previous();
        const fallback = prev ? (chosen()[prev.id ?? ""] ?? prev.changed) : [];
        const keep = fallback.filter((n) => names.has(n));
        return keep.length ? keep.slice(0, MAX_CHOSEN) : step.signals.slice(0, 3).map((s) => s.name);
    });

    function toggleSignal(name: string) {
        const step = selected();
        if (!step) return;
        const cur = shownSignals();
        const next = cur.includes(name) ? cur.filter((n) => n !== name) : [...cur, name].slice(-MAX_CHOSEN);
        setChosen({ ...chosen(), [step.id ?? ""]: next });
    }

    createEffect(on([selected, previous, shownSignals, range, showBefore], async ([step, prev, names, r, withBefore]) => {
        seriesAbort?.abort();
        if (!step || !names.length || !step.ok && !step.signals.length) {
            setSeries([]);
            setBefore([]);
            return;
        }
        seriesAbort = new AbortController();
        const beforeKey = withBefore && prev && step.type !== "source" ? prev.key : null;
        try {
            const res = await api.series(step.key, names, beforeKey, r, 3000, seriesAbort.signal);
            setSeries(res.series);
            setBefore(res.before ?? []);
        } catch (e) {
            if ((e as Error).name === "AbortError") return;
            if (/no longer cached/.test((e as Error).message)) {
                lastRun = "";                    // same recipe, but the server must compute it again
                scheduleRun(10);
            } else {
                notify((e as Error).message);
            }
        }
    }));

    // a new selection starts unzoomed
    createEffect(on(() => selected()?.key, () => setRange(null), { defer: true }));

    // ---- recipes -------------------------------------------------------------------

    function currentBlocks(): RecipeBlock[] {
        const cat = catalog();
        return ws && cat ? readRecipe(ws, cat).map(({ id, ...b }) => b) : [];
    }

    function setRecordingInWorkspace(id: string) {
        const hat = ws ? hatBlock(ws) : null;
        if (!hat) return;
        if (!options.recordings.some((r) => r.id === id)) return;
        hat.setFieldValue(id, "recording");
        setRecording(id);
    }

    function load(blocks: RecipeBlock[], keepRecording = true) {
        const cat = catalog();
        if (!ws || !cat) return;
        const rec = recording();
        const list = JSON.parse(JSON.stringify(blocks)) as RecipeBlock[];
        if (keepRecording && list[0]?.type === "source" && rec && !list[0].params.recording) {
            list[0].params.recording = rec;
        }
        setChosen({});
        setSelectedId(null);
        loadRecipe(ws, cat, list);
        scheduleRun(10);
    }

    function applyPreset(id: string) {
        const p = presets().find((x) => x.id === id);
        if (!p) return;
        if (ws && chain(ws).length > 1 && !confirm(`Replace the current recipe with "${p.name}"?`)) return;
        load(p.blocks);
        notify(p.description);
    }

    async function saveToServer() {
        const name = prompt("Save this recipe for the team as:", "");
        if (!name) return;
        try {
            await api.saveRecipe(name.trim(), { name: name.trim(), blocks: currentBlocks() });
            notify(`Saved "${name.trim()}"`);
        } catch (e) {
            notify((e as Error).message);
        }
    }

    async function refreshSaved() {
        try {
            setSavedRecipes(await api.recipes());
        } catch {
            setSavedRecipes([]);
        }
    }

    async function openFromServer(name: string) {
        if (!name) return;
        try {
            const r = await api.loadRecipe(name);
            load(r.blocks);
            notify(`Opened "${name}"`);
        } catch (e) {
            notify((e as Error).message);
        }
    }

    function downloadRecipe() {
        const blob = new Blob([JSON.stringify({ blocks: currentBlocks() }, null, 2)], { type: "application/json" });
        download("pilog-recipe.json", blob);
    }

    async function uploadRecipe(file: File) {
        try {
            const data = JSON.parse(await file.text());
            if (!Array.isArray(data.blocks)) throw new Error("not a recipe file");
            load(data.blocks);
        } catch (e) {
            notify(`Could not open ${file.name}: ${(e as Error).message}`);
        }
    }

    async function uploadRecording(file: File) {
        notify(`Uploading ${file.name} (${formatBytes(file.size)})...`);
        try {
            const { id } = await api.upload(file);
            const list = await api.recordings();
            options.recordings = list;
            setRecordings(list);
            setRecordingInWorkspace(id);
            notify(`${file.name} uploaded`);
        } catch (e) {
            notify((e as Error).message);
        }
    }

    async function exportCsv() {
        const step = selected();
        if (!step) return;
        const names = shownSignals().filter((n) => step.signals.find((s) => s.name === n)?.axis === "time");
        if (!names.length) {
            notify("Pick time signals to export");
            return;
        }
        const rateText = prompt("Export at a fixed rate (Hz)? Leave empty to use the first signal's own samples.", "");
        if (rateText === null) return;
        try {
            const blob = await api.exportCsv(step.key, names, rateText.trim() ? Number(rateText) : null);
            download("pilog-lab.csv", blob);
        } catch (e) {
            notify((e as Error).message);
        }
    }

    // ---- setup ---------------------------------------------------------------------

    onMount(async () => {
        let cat: Catalog;
        try {
            const [c, recs, pre] = await Promise.all([api.catalog(), api.recordings(), api.presets()]);
            cat = c;
            options.recordings = recs;
            setRecordings(recs);
            setPresets(pre);
            setCatalog(c);
        } catch (e) {
            setLoadError(`Cannot reach the lab server: ${(e as Error).message}`);
            return;
        }
        defineBlocks(cat);
        ws = Blockly.inject(host!, {
            toolbox: toolbox(cat),
            renderer: "zelos",
            theme,
            trashcan: true,
            sounds: false,
            zoom: { controls: true, wheel: true, startScale: 0.72, maxScale: 2, minScale: 0.3, scaleSpeed: 1.15 },
            move: { scrollbars: true, drag: true, wheel: false },
            grid: { spacing: 24, length: 2, colour: "#ffffff10", snap: false },
        });
        ws.addChangeListener(Blockly.Events.disableOrphans);
        ws.addChangeListener((e: Blockly.Events.Abstract) => {
            if (e.isUiEvent) {
                if (e.type === Blockly.Events.SELECTED) {
                    const id = (e as Blockly.Events.Selected).newElementId;
                    if (id && ws?.getBlockById(id)) setSelectedId(id);
                }
                return;
            }
            scheduleRun();
        });
        const resize = new ResizeObserver(() => ws && Blockly.svgResize(ws));
        resize.observe(host!);
        onCleanup(() => resize.disconnect());

        let initial: RecipeBlock[] | null = null;
        try {
            const saved = JSON.parse(storageGet(LOCAL_RECIPE) || "null");
            if (saved && Array.isArray(saved.blocks) && saved.blocks.length) initial = saved.blocks;
        } catch {
            initial = null;
        }
        if (!initial) {
            const raw = presets().find((p) => p.id === "raw");
            initial = JSON.parse(JSON.stringify(raw?.blocks ?? [{ type: "source", params: {}, enabled: true }]));
            const first = recs().find((r) => r.kind === "dashboard") ?? recs()[0];
            if (first && initial![0]) initial![0].params.recording = first.id;
        }
        loadRecipe(ws, cat, initial!);
        scheduleRun(10);
        void refreshSaved();

        function recs() {
            return options.recordings;
        }
    });

    onCleanup(() => {
        clearTimeout(runTimer);
        clearInterval(elapsedTimer);
        runAbort?.abort();
        seriesAbort?.abort();
        ws?.dispose();
    });

    const groups = createMemo(() => {
        const out = new Map<string, Recording[]>();
        for (const r of recordings()) {
            if (!out.has(r.group)) out.set(r.group, []);
            out.get(r.group)!.push(r);
        }
        return [...out.entries()];
    });

    const signalGroups = createMemo(() => {
        const step = selected();
        const out = new Map<string, SignalInfo[]>();
        for (const s of step?.signals ?? []) {
            const g = s.name.includes(".") ? s.name.split(".")[0] : "other";
            if (!out.has(g)) out.set(g, []);
            out.get(g)!.push(s);
        }
        return [...out.entries()];
    });

    const colourOf = (name: string) => {
        const i = shownSignals().indexOf(name);
        return i >= 0 ? PALETTE[i % PALETTE.length] : undefined;
    };

    return (
        <div class="h-full flex flex-col bg-base-100 text-base-content">
            {/* ---- top bar ---- */}
            <header class="flex flex-wrap items-center gap-2 px-3 py-2 border-b border-base-300 bg-base-200">
                <div class="flex items-center gap-2 mr-2">
                    <div class="w-7 h-7 rounded-lg bg-gradient-to-br from-amber-400 to-pink-500 grid place-items-center text-black font-black text-sm">π</div>
                    <div class="leading-tight">
                        <div class="font-semibold">Pi-LOG Filter Lab</div>
                        <div class="text-[11px] text-base-content/60">stack blocks, see what each one does</div>
                    </div>
                </div>
                <select class="select select-sm select-bordered max-w-[22rem]" value={recording()}
                        onChange={(e) => setRecordingInWorkspace(e.currentTarget.value)} title="The recording the recipe starts from">
                    <option value="" disabled>Pick a recording…</option>
                    <For each={groups()}>
                        {([group, list]) => (
                            <optgroup label={group}>
                                <For each={list}>{(r) => <option value={r.id}>{r.label} · {formatBytes(r.size)}</option>}</For>
                            </optgroup>
                        )}
                    </For>
                </select>
                <button class="btn btn-sm btn-ghost" onClick={() => fileInput?.click()} title="Upload a .bin or .json recording">Upload…</button>
                <input ref={fileInput} type="file" accept=".bin,.json" class="hidden"
                       onChange={(e) => { const f = e.currentTarget.files?.[0]; if (f) void uploadRecording(f); e.currentTarget.value = ""; }} />
                <div class="divider divider-horizontal mx-0" />
                <select class="select select-sm select-bordered" value="" onChange={(e) => { applyPreset(e.currentTarget.value); e.currentTarget.value = ""; }}
                        title="Start from a ready-made recipe">
                    <option value="">Start from a preset…</option>
                    <For each={presets()}>{(p) => <option value={p.id}>{p.name}</option>}</For>
                </select>
                <select class="select select-sm select-bordered" value="" onFocus={() => void refreshSaved()}
                        onChange={(e) => { void openFromServer(e.currentTarget.value); e.currentTarget.value = ""; }}
                        title="Recipes saved on the server for the team">
                    <option value="">Open saved…</option>
                    <For each={savedRecipes()}>{(r) => <option value={r.name}>{r.name}</option>}</For>
                </select>
                <button class="btn btn-sm btn-ghost" onClick={() => void saveToServer()}>Save</button>
                <div class="dropdown dropdown-end">
                    <button tabindex="0" class="btn btn-sm btn-ghost">More ▾</button>
                    <ul tabindex="0" class="dropdown-content menu bg-base-200 rounded-box z-20 w-56 p-2 shadow-xl border border-base-300">
                        <li><button onClick={downloadRecipe}>Download recipe (.json)</button></li>
                        <li><button onClick={() => recipeInput?.click()}>Open recipe file…</button></li>
                        <li><button onClick={() => void exportCsv()}>Export shown signals (.csv)</button></li>
                        <li><button onClick={() => { setShowHelp(true); }}>How it works</button></li>
                    </ul>
                </div>
                <input ref={recipeInput} type="file" accept=".json" class="hidden"
                       onChange={(e) => { const f = e.currentTarget.files?.[0]; if (f) void uploadRecipe(f); e.currentTarget.value = ""; }} />
                <div class="ml-auto flex items-center gap-2 text-sm">
                    <Show when={running()}>
                        <span class="loading loading-spinner loading-sm" />
                        <span class="text-base-content/70">running… {elapsed() >= 2 ? `${elapsed()} s` : ""}</span>
                    </Show>
                    <Show when={!running() && steps().length}>
                        <span class={runSummary().ok ? "text-success" : "text-error"}>{runSummary().text}</span>
                    </Show>
                </div>
            </header>

            <Show when={loadError()}>
                <div class="alert alert-error rounded-none">{loadError()}</div>
            </Show>
            <Show when={runError()}>
                <div class="alert alert-warning rounded-none py-2 text-sm">{runError()}</div>
            </Show>
            <Show when={running() && elapsed() >= 3 && bigRecording()}>
                <div class="alert alert-info rounded-none py-2 text-sm">
                    Opening a large recording the first time takes a while (roughly a minute per GB). It is cached
                    afterwards, so the next run starts in seconds.
                </div>
            </Show>

            {/* ---- main ---- */}
            <main class="flex-1 min-h-0 grid grid-cols-1 lg:grid-cols-[minmax(0,1.1fr)_minmax(0,1fr)]">
                <section class="relative min-h-[55vh] lg:min-h-0 border-b lg:border-b-0 lg:border-r border-base-300">
                    <div ref={host} class="blockly-host" />
                </section>

                <section class="min-h-0 grid grid-rows-[minmax(260px,1.25fr)_minmax(0,1fr)]">
                    {/* chart + signal list */}
                    <div class="min-h-0 flex border-b border-base-300">
                        <div class="relative flex-1 min-w-0 m-2">
                            <Show when={series().length} fallback={
                                <div class="absolute inset-0 grid place-items-center text-sm text-base-content/50 text-center px-6">
                                    {steps().length ? "Tick signals on the right to plot them." : "Pick a recording to start."}
                                </div>}>
                                <Chart series={series()} before={showBefore() ? before() : []} onZoom={setRange} />
                            </Show>
                        </div>
                        <aside class="w-56 shrink-0 overflow-auto border-l border-base-300 p-2 text-xs">
                            <label class="flex items-center gap-2 mb-2 cursor-pointer">
                                <input type="checkbox" class="checkbox checkbox-xs" checked={showBefore()} onChange={(e) => setShowBefore(e.currentTarget.checked)} />
                                <span>show <span class="opacity-70">before this block</span> dashed</span>
                            </label>
                            <Show when={range()}>
                                <button class="btn btn-xs btn-ghost w-full mb-2" onClick={() => setRange(null)}>reset zoom</button>
                            </Show>
                            <For each={signalGroups()}>
                                {([group, list]) => (
                                    <div class="mb-2">
                                        <div class="uppercase tracking-wide text-[10px] text-base-content/50 mb-1">{group}</div>
                                        <For each={list}>
                                            {(s) => (
                                                <label class="flex items-center gap-1.5 py-0.5 cursor-pointer hover:bg-base-200 rounded px-1" title={`${s.desc}${s.rate ? ` · ${s.rate} samples/s` : ""}`}>
                                                    <input type="checkbox" class="checkbox checkbox-xs" checked={shownSignals().includes(s.name)}
                                                           onChange={() => toggleSignal(s.name)} />
                                                    <span class="w-2 h-2 rounded-full shrink-0" style={{ background: colourOf(s.name) ?? "transparent" }} />
                                                    <span class="truncate">{s.name}</span>
                                                    <span class="ml-auto text-base-content/40 shrink-0">{s.unit}</span>
                                                </label>
                                            )}
                                        </For>
                                    </div>
                                )}
                            </For>
                        </aside>
                    </div>

                    {/* what the selected block does and found */}
                    <div class="min-h-0 overflow-auto p-4 space-y-3">
                        <Show when={selected() && selectedDef()} fallback={
                            <div class="text-sm text-base-content/60">Click a block to see what it does and what it found.</div>}>
                            <div class="flex items-center gap-2 flex-wrap">
                                <span class="w-3 h-3 rounded" style={{ background: colours().get(selectedDef()!.category) }} />
                                <h2 class="text-lg font-semibold">{selectedDef()!.title}</h2>
                                <span class="badge badge-sm badge-ghost" title="time this block took to compute">
                                    {selected()!.ms} ms{selected()!.cached ? " · reused" : ""}
                                </span>
                                <Show when={!selected()!.ok}>
                                    <span class={`badge badge-sm ${selected()!.afterError ? "badge-warning" : "badge-error"}`}>
                                        {selected()!.afterError ? "affected" : "error"}
                                    </span>
                                </Show>
                                <Show when={selected()!.notRun}><span class="badge badge-sm badge-ghost">not run</span></Show>
                            </div>
                            <Show when={selected()!.error}>
                                <div class={`alert py-2 text-sm ${selected()!.afterError ? "alert-warning" : "alert-error"}`}>
                                    {selected()!.afterError ? "Probably a consequence of a block above failing: " : ""}{selected()!.error}
                                </div>
                            </Show>
                            <For each={selected()!.warnings}>{(w) => <div class="alert alert-warning py-2 text-sm">{w}</div>}</For>
                            <Show when={selected()!.notes.length}>
                                <ul class="text-sm space-y-1">
                                    <For each={selected()!.notes}>{(n) => <li class="flex gap-2"><span class="text-success">›</span><span>{n}</span></li>}</For>
                                </ul>
                            </Show>
                            <Show when={selected()!.table}>
                                <div class="overflow-x-auto">
                                    <table class="table table-xs">
                                        <thead><tr><For each={selected()!.table![0]}>{(h) => <th>{h}</th>}</For></tr></thead>
                                        <tbody>
                                            <For each={selected()!.table!.slice(1)}>
                                                {(row) => <tr><For each={row}>{(c) => <td class="font-mono">{c}</td>}</For></tr>}
                                            </For>
                                        </tbody>
                                    </table>
                                </div>
                            </Show>
                            <Show when={selected()!.download}>
                                <button class="btn btn-sm btn-primary" onClick={() => download(selected()!.download!.name,
                                    new Blob([selected()!.download!.content], { type: "application/json" }))}>
                                    Download {selected()!.download!.name}
                                </button>
                            </Show>
                            <details open class="pt-1">
                                <summary class="cursor-pointer text-sm font-medium text-base-content/70">What this block does</summary>
                                <div class="mt-2"><HelpText text={selectedDef()!.help} /></div>
                            </details>
                        </Show>
                    </div>
                </section>
            </main>

            <Show when={toast()}>
                <div class="toast toast-end toast-bottom z-30"><div class="alert alert-info text-sm max-w-md">{toast()}</div></div>
            </Show>

            <Show when={showHelp()}>
                <div class="fixed inset-0 z-40 grid place-items-center bg-black/60 p-4" onClick={() => { setShowHelp(false); storageSet(LOCAL_HELP_SEEN, "1"); }}>
                    <div class="card bg-base-200 max-w-xl shadow-2xl" onClick={(e) => e.stopPropagation()}>
                        <div class="card-body gap-3 text-sm leading-relaxed">
                            <h2 class="card-title">How the Filter Lab works</h2>
                            <p>A <b>recipe</b> is a stack of blocks, read from top to bottom like a Scratch script. It always starts with the
                                yellow <b>Recording</b> block: the payload's data exactly as logged, every sensor at its own rate.</p>
                            <p>Drag blocks from the categories on the left and snap them under the stack. Each block does one thing
                                (crop, filter, compute altitude, run the Kalman filter...) and hands its result to the next. Blocks lying loose
                                are greyed out and do not run. Right-click a block to switch it off or delete it.</p>
                            <p>Every change runs the recipe again. <b>Click a block</b> to see its result on the chart: solid lines are
                                after the block, dashed ones before it. The panel underneath explains what the block does and what it found.</p>
                            <p>Not sure where to start? <b>Start from a preset</b> - "Website trajectory" is exactly what the Dashboard shows,
                                "EuRoC estimator" is the team's MATLAB chain block by block.</p>
                            <div class="card-actions justify-end">
                                <button class="btn btn-primary btn-sm" onClick={() => { setShowHelp(false); storageSet(LOCAL_HELP_SEEN, "1"); }}>Got it</button>
                            </div>
                        </div>
                    </div>
                </div>
            </Show>
        </div>
    );
};

export default App;
