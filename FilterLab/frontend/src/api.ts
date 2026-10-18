import { Catalog, Preset, Recipe, RecipeBlock, Recording, Series, Step } from "./types";

async function json<T>(res: Response): Promise<T> {
    let body: any = null;
    try {
        body = await res.json();
    } catch {
        // not JSON
    }
    if (!res.ok) {
        throw new Error(body?.error || body?.description || `HTTP ${res.status}`);
    }
    return body as T;
}

const post = (url: string, data: unknown, signal?: AbortSignal) =>
    fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(data),
        signal,
    });

export const api = {
    catalog: () => fetch("/api/catalog").then((r) => json<Catalog>(r)),
    presets: () => fetch("/api/presets").then((r) => json<{ presets: Preset[] }>(r)).then((d) => d.presets),
    recordings: () =>
        fetch("/api/recordings").then((r) => json<{ recordings: Recording[] }>(r)).then((d) => d.recordings),

    upload: async (file: File) => {
        const form = new FormData();
        form.append("file", file);
        return json<{ id: string }>(await fetch("/api/recordings", { method: "POST", body: form }));
    },

    run: (blocks: RecipeBlock[], signal?: AbortSignal) =>
        post("/api/run", { blocks }, signal).then((r) => json<{ steps: Step[] }>(r)).then((d) => d.steps),

    series: (key: string, names: string[], beforeKey: string | null, range: [number, number] | null,
             maxPoints: number, signal?: AbortSignal) =>
        post("/api/series", {
            key, names, beforeKey, maxPoints,
            start: range ? range[0] : null, end: range ? range[1] : null,
        }, signal).then((r) => json<{ series: Series[]; before?: Series[] }>(r)),

    exportCsv: async (key: string, names: string[], rate: number | null) => {
        const res = await post("/api/export", { key, names, rate });
        if (!res.ok) await json(res);
        return res.blob();
    },

    recipes: () =>
        fetch("/api/recipes").then((r) => json<{ recipes: { name: string; modified: number }[] }>(r))
            .then((d) => d.recipes),
    loadRecipe: (name: string) => fetch(`/api/recipes/${encodeURIComponent(name)}`).then((r) => json<Recipe>(r)),
    saveRecipe: (name: string, recipe: Recipe) =>
        fetch(`/api/recipes/${encodeURIComponent(name)}`, {
            method: "PUT",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(recipe),
        }).then((r) => json(r)),
};
