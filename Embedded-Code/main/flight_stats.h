/**
 * @file flight_stats.h
 * @brief Live per-sensor sample-rate monitor.
 *
 * Each sensor task calls flight_stats_tick() once per produced sample;
 * flight_stats_task() prints the achieved Hz for every sensor once per second,
 * tagged with the current flight mode. Lets you actually see the per-mode rate
 * change (the per-sensor debug echoes are throttled and don't reflect it).
 */

#ifndef _FLIGHT_STATS_H_
#define _FLIGHT_STATS_H_

#include <stdint.h>

enum {
    STAT_MPU6050 = 0,
    STAT_INA219,
    STAT_BME680,
    STAT_GPS,
    STAT_COUNT
};

extern volatile uint32_t g_flight_stats[STAT_COUNT];

/** Count one produced sample for sensor `idx`. Cheap; called from sensor tasks. */
static inline void flight_stats_tick(int idx) { g_flight_stats[idx]++; }

/** 1 Hz task. arg = struct SysContext * (for the mode label); may be NULL. */
void flight_stats_task(void *arg);

#endif /* _FLIGHT_STATS_H_ */
