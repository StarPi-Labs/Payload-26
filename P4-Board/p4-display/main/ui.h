/**
 * @file ui.h
 * @brief LVGL ground-station UI: sensor overview + per-sensor detail pages.
 *
 * Layout (1024x600): a persistent header with the flight mode, link and SD
 * backup state; under it either the overview - one touchable tile per sensor
 * with on/off state and live rate - or the detail page of the tile touched,
 * with every value and auto-scaling graphs.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sd_backup.h"
#include "telemetry_codec.h"

/** Everything the UI shows, prepared by the caller once per refresh. */
typedef struct {
    telem_state_t    st;                                /**< decoder snapshot */
    bool             link_ok;
    uint32_t         link_up_ms;                        /**< time since the link came up, 0 if down */
    uint32_t         age_ms;                            /**< since the last packet, UINT32_MAX = never */
    float            pkt_rate;                          /**< packets/s over the last second */
    float            frame_rate;                        /**< decoded frames/s */
    float            sensor_hz[TELEM_SENSOR_COUNT];     /**< measured sample rate per sensor */
    bool             sensor_seen[TELEM_SENSOR_COUNT];   /**< at least one frame since boot */
    uint32_t         sensor_age_ms[TELEM_SENSOR_COUNT]; /**< since its last frame, local clock */
    bool             tick_1hz;                          /**< true once per second, for slow graphs */
    sd_backup_info_t sd;
} ui_model_t;

/** Build every screen. Call once, inside the display lock. */
void ui_create(void);

/** Refresh from @p m. Call at ~10 Hz, inside the display lock. */
void ui_update(const ui_model_t *m);
