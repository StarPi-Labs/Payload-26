/**
 * @file ui.h
 * @brief LVGL live-flight screen for the Pi-LOG ground display.
 *
 * Deliberately a live view only: no scrubbing, no playback, no camera feeds.
 * During a flight the useful thing is the current state at a glance from across
 * a field, so everything is large, high-contrast and updated in place.
 */
#pragma once

#include "telemetry_codec.h"

/** Build the screen. Call with the LVGL lock held. */
void ui_create(void);

/**
 * Push a new telemetry snapshot into the widgets.
 *
 * @param st         decoder state to render
 * @param link_ok    true while packets are arriving
 * @param age_ms     milliseconds since the last packet (drives the stale banner)
 * @param pkt_rate   packets per second, as a link-health readout
 *
 * Call with the LVGL lock held.
 */
void ui_update(const telem_state_t *st, bool link_ok, uint32_t age_ms, float pkt_rate);
