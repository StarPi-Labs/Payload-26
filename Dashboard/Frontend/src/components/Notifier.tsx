
import { Component, Switch, Match, Show } from "solid-js"
import { TbFillInfoCircle, TbFillAlertHexagon, TbFillCircleCheck, TbFillSquareX } from 'solid-icons/tb'
import { Notification } from "../models/notification"
import { NotificationType } from "../models/notification-type"


const Notifier: Component<{ notification: Notification }> = (props) => {

    const iconSize = 24

    const cssMap = new Map<NotificationType, string>()
        .set(NotificationType.Info, "alert-info")
        .set(NotificationType.Success, "alert-success")
        .set(NotificationType.Warning, "alert-warning")
        .set(NotificationType.Error, "alert-error")


    const cssClass = () => {
        const baseCss = "alert absolute z-10 w-full"
        const cssClass = cssMap.get(props.notification?.type)
        return `${baseCss} ${cssClass}`;
    }


    return (
        <Show when={props.notification?.isVisible}>
            <div role="alert" class={cssClass()}>
                <Switch>
                    <Match when={props.notification?.type === NotificationType.Info}>
                        <TbFillInfoCircle size={iconSize} />
                    </Match>
                    <Match when={props.notification?.type === NotificationType.Success}>
                        <TbFillCircleCheck size={iconSize} />
                    </Match>
                    <Match when={props.notification?.type === NotificationType.Warning}>
                        <TbFillAlertHexagon size={iconSize} />
                    </Match>
                    <Match when={props.notification?.type === NotificationType.Error}>
                        <TbFillSquareX size={iconSize} />
                    </Match>
                </Switch>
                <span>{props.notification?.message}</span>
            </div>
        </Show>
    )
}

export default Notifier