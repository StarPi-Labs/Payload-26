import { NotificationType } from "./notification-type";

export interface Notification {
    type: NotificationType
    isVisible: boolean
    message: string
}
