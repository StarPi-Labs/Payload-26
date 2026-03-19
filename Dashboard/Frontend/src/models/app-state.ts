import { Spinner } from "./spinner"
import { Notification } from "./notification"
import { AppConfig } from "./app-config"


export interface AppState {
    notification: Notification
    spinner: Spinner
    appConfig: AppConfig
}
