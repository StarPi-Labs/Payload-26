import { RouteSectionProps } from "@solidjs/router";
import Notifier from "./components/Notifier";
import Spinner from "./components/Spinner";
import ToolbarLayout from "./components/Toolbar";
import store from "./store/store";
import { FiGrid } from "solid-icons/fi";
import { createEffect, on } from "solid-js";
import { AppTheme } from "./models/app-theme";


const App = (props: RouteSectionProps) => {

  createEffect(
    on(
      () => store.get.appConfig?.theme,
      (theme) => {
        if (!theme) return
        document.documentElement.setAttribute("data-theme", theme)
      }
    )
  )

  const themes = Object.values(AppTheme);

  return (
    <div class="min-h-screen w-full">
      <Notifier notification={store.get.notification} />
      <Spinner spinner={store.get.spinner} />

      <ToolbarLayout
        routes={[
          { label: "Dashboard", path: "/dashboard", icon: FiGrid },
        ]}
        themes={themes}
        selectedTheme={store.get.appConfig?.theme}
        onThemeChange={(theme) => store.theme.set(theme as AppTheme)}
      >
        <div class="py-5">
          {props.children}
        </div>
      </ToolbarLayout>
    </div>
  );
};

export default App
