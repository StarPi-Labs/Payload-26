/**
 * @file status_led.h
 * @brief Onboard addressable WS2812 RGB LED (GPIO38) — shows the flight mode.
 *
 *   POST / boot   -> blue  (solid)
 *   ARMED         -> green (slow blink = armed, waiting for launch)
 *   BOOST         -> red   (solid, bright = powered flight)
 *   COAST         -> amber (solid)
 *   init / other  -> dim white
 *
 * Driven straight off the RMT peripheral (esp_driver_rmt) — no external
 * component. A small task mirrors ctx->mode onto the LED.
 */
#ifndef _STATUS_LED_H_
#define _STATUS_LED_H_

#include "systemp2i.h"   /* struct SysContext, MODE_* */

/** Bring up the WS2812 on GPIO38. Safe to call once at boot. */
void status_led_init(void);

/** Spawn a task that mirrors ctx->mode onto the LED colour. */
void status_led_start_task(struct SysContext *ctx);

#endif /* _STATUS_LED_H_ */
