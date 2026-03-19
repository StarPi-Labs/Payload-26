import { For, ParentComponent } from "solid-js";
import { A } from "@solidjs/router";
import { ToolbarRoute } from "../models/toolbar-route";
import starPiLogo from "../../assets/star-pi-logo.png";

type ToolbarLayoutProps = {
    routes: ToolbarRoute[];
    themes?: string[];
    selectedTheme?: string;
    onThemeChange?: (theme: string) => void;
};

const ToolbarLayout: ParentComponent<ToolbarLayoutProps> = (props) => {
    return (
        <div class="min-h-screen">
            <header class="navbar bg-base-200 fixed top-0 z-50">
                <div class="flex flex-1 items-center gap-2 px-4">
                    <img
                        src={starPiLogo}
                        alt="StarPi logo"
                        class="w-15 h-15 mr-2"
                    />
                    <For each={props.routes}>
                        {(r) => (
                            <A href={r.path} class="btn btn-ghost btn-sm gap-2">
                                {r.icon && <r.icon class="w-5 h-5" />}
                                <span>{r.label}</span>
                            </A>
                        )}
                    </For>
                </div>
                {props.themes && props.themes.length > 0 && (
                    <div class="flex-none px-4">
                        <select
                            class="select select-sm select-bordered"
                            value={props.selectedTheme ?? ""}
                            onChange={(event) => {
                                const theme = event.currentTarget.value;
                                if (theme) props.onThemeChange?.(theme);
                            }}
                        >
                            <option value="" disabled>
                                Theme
                            </option>
                            <For each={props.themes}>
                                {(theme) => <option value={theme}>{theme}</option>}
                            </For>
                        </select>
                    </div>
                )}
            </header>

            <main class="pt-16 container mx-auto px-6 py-4">
                {props.children}
            </main>
        </div>
    );
};

export default ToolbarLayout;
