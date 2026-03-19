import { createStore } from "solid-js/store";
import { NotificationType } from "../models/notification-type";
import { AppState } from "../models/app-state";
import { defaultAppState } from "./default-app-state";
import { AppTheme } from "../models/app-theme";

const [store, setStore] = createStore<AppState>(defaultAppState)

const spinner = {
    show: (message?: string) => setStore("spinner", { isVisible: true, message }),
    hide: () => setStore("spinner", { isVisible: false, message: null })
}

const _setNotification = (timeoutMs = 5000, timeout: NodeJS.Timeout = null) => (type: NotificationType) => (message: string) => {
    if (timeout) {
        clearTimeout(timeout)
    }
    setStore("notification", { type, message, isVisible: true })
    timeout = setTimeout(() => {
        setStore("notification", defaultAppState.notification)
    }, timeoutMs)
}

const setNotification = _setNotification()

const notification = {
    success: setNotification(NotificationType.Success),
    info: setNotification(NotificationType.Info),
    warning: setNotification(NotificationType.Warning),
    error: setNotification(NotificationType.Error)
}

const theme = {
    set: (theme: AppTheme) => setStore("appConfig", "theme", theme)
}


export default {
    get: store,
    spinner,
    notification,
    theme
}