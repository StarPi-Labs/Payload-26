import { Spinner as SpinnerModel } from "../models/spinner"
import { Component, Show } from "solid-js"

const Spinner: Component<{ spinner: SpinnerModel }> = (props) => {
  return (
    <Show when={props.spinner?.isVisible}>
      <div class="fixed inset-0 z-50 flex flex-col items-center justify-center bg-black/40">
        <span class="loading loading-ring loading-xl"></span>
        <div class="mt-4 text-white text-lg">{props.spinner?.message}</div>
      </div>
    </Show>
  )
}

export default Spinner
