/* @refresh reload */
import "./styles/index.css";
import { render } from "solid-js/web";
import "solid-devtools";
import { Route, Router } from "@solidjs/router";
import App from "./App";
import Dashboard from "./pages/Dashboard";

const root = document.getElementById("root");

if (import.meta.env.DEV && !(root instanceof HTMLElement)) {
  throw new Error("Root element not found.");
}

render(
  () => (
    <Router root={App}>
      <Route path="/" component={Dashboard} />
      <Route path="/dashboard" component={Dashboard} />
    </Router>
  ),
  root!
);
