/**
 * @file flight_state.h
 * @brief Autonomous flight state machine: ARMED -> BOOST -> COAST.
 *
 *   - Launch:  ARMED -> BOOST when the rolling-average |accel| >= FLIGHT_LAUNCH_TRIP_G.
 *   - Burnout: BOOST -> COAST when the rolling-average |accel| < FLIGHT_LAUNCH_G,
 *              after at least FLIGHT_MIN_BOOST_MS in BOOST.
 *   - COAST is terminal (no apogee/landing detection).
 *   - The mode is persisted to NVS on every transition, so a power-cut mid-flight
 *     RESUMES the saved mode. The only way back to ARMED is to hold the BOOT
 *     button for ~FLIGHT_REARM_HOLD_MS right after startup.
 *
 * The MPU task feeds samples via flight_state_update(); it also owns the accel
 * range switch (+/-16g in BOOST, +/-2g elsewhere) and the SYSSTATE frame.
 */

#ifndef _FLIGHT_STATE_H_
#define _FLIGHT_STATE_H_

#include <stdint.h>
#include "systemp2i.h"   /* struct SysContext, MODE_* */

/* ── Tunables — adjust here ─────────────────────────────────── */
#define FLIGHT_WINDOW_SAMPLES   10      /* rolling-average length over |accel| (samples) */
#define FLIGHT_LAUNCH_TRIP_G    1.90f   /* ARMED->BOOST trip. Must sit BELOW the +/-2g full-scale
                                         * rail (32767/16384 = 1.99994 g): a clean axial launch
                                         * clips at the rail and would never reach 2.0. */
#define FLIGHT_LAUNCH_G         2.0f    /* BOOST->COAST burnout: avg |accel| < this (checked in
                                         * +/-16g range, no clipping concern) */
#define FLIGHT_MIN_BOOST_MS     300     /* minimum time held in BOOST before burnout -> COAST */
#define FLIGHT_REARM_HOLD_MS    1500    /* hold BOOT this long at startup to re-arm to ARMED */
#define FLIGHT_REARM_WAIT_MS    1500    /* window after boot in which pressing BOOT starts the
                                         * re-arm hold check (can't hold it through reset —
                                         * that straps the chip into ROM download mode) */

/**
 * Resolve the boot flight-mode: init NVS, check the BOOT-hold re-arm, load the
 * saved mode. Call FIRST in app_main — the SD logger needs the result to pick
 * append (in-flight resume) vs fresh log (new ARMED session).
 */
void flight_state_preinit(void);

/** The mode resolved by flight_state_preinit() (MODE_ARMED if never saved). */
uint32_t flight_state_boot_mode(void);

/** Apply the resolved boot mode to ctx (call after POST, which uses MODE_POST). */
void flight_state_init(struct SysContext *ctx);

/** Feed one accel-magnitude sample [g]; runs the ARMED->BOOST->COAST machine. */
void flight_state_update(struct SysContext *ctx, float accel_g);

#endif /* _FLIGHT_STATE_H_ */
