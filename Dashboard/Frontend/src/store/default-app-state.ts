import { AppState } from "../models/app-state";
import { AppTheme } from "../models/app-theme";
import { NotificationType } from "../models/notification-type";

export const defaultAppState: AppState = {
    notification: { isVisible: false, message: null, type: NotificationType.Info },
    spinner: { isVisible: false, message: null },
    appConfig: {
        theme: AppTheme.Dark
    }
}
