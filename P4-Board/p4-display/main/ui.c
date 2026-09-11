/**
 * @file ui.c
 * @brief LVGL live-flight screen - implementation.
 *
 * Layout (1024x600 landscape):
 *
 *   +--------------------------------------------------------------+
 *   |  MODE badge        Pi-LOG LIVE            link / rate / age   |
 *   +--------------------------------------------------------------+
 *   |  ALTITUDE (huge)          |  vertical vel   horizontal vel    |
 *   |                           |  accel X / Y / Z                  |
 *   +---------------------------+----------------------------------+
 *   |  altitude chart (rolling) |  temp  press  hum  gas            |
 *   |                           |  bus V  current  power            |
 *   |                           |  lat / lon / sats                 |
 *   +--------------------------------------------------------------+
 *
 * Everything is created once and only the label text is rewritten afterwards -
 * allocating widgets on every update would fragment the heap within minutes at
 * a 10 Hz refresh.
 */

#include "ui.h"

#include <stdio.h>
#include "lvgl.h"

/* Palette chosen for outdoor legibility: near-black background, high-contrast
 * text, colour reserved for state that matters (mode, link, warnings). */
#define COL_BG        lv_color_hex(0x0B0F14)
#define COL_PANEL     lv_color_hex(0x161C24)
#define COL_TEXT      lv_color_hex(0xE6EDF3)
#define COL_DIM       lv_color_hex(0x8B98A5)
#define COL_ACCENT    lv_color_hex(0x58A6FF)
#define COL_OK        lv_color_hex(0x3FB950)
#define COL_WARN      lv_color_hex(0xD29922)
#define COL_ERR       lv_color_hex(0xF85149)

#define CHART_POINTS  120

static lv_obj_t *s_mode_badge;
static lv_obj_t *s_link_lbl;
static lv_obj_t *s_alt_val;
static lv_obj_t *s_alt_unit;
static lv_obj_t *s_vvel_val, *s_hvel_val;
static lv_obj_t *s_ax_val, *s_ay_val, *s_az_val;
static lv_obj_t *s_temp_val, *s_press_val, *s_hum_val, *s_gas_val;
static lv_obj_t *s_volt_val, *s_curr_val, *s_pwr_val;
static lv_obj_t *s_lat_val, *s_lon_val, *s_sat_val;
static lv_obj_t *s_stale_banner;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_alt_series;

/* ── small builders ───────────────────────────────────────────────────── */

static lv_obj_t *make_panel(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_pad_all(p, 10, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* Caption above, value below - the pattern used for every readout. */
static lv_obj_t *make_stat(lv_obj_t *parent, const char *caption,
                           lv_coord_t x, lv_coord_t y, const lv_font_t *font)
{
    lv_obj_t *cap = lv_label_create(parent);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, COL_DIM, 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cap, x, y);

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, COL_TEXT, 0);
    lv_obj_set_style_text_font(val, font, 0);
    lv_obj_set_pos(val, x, y + 18);
    return val;
}

/* ── screen ───────────────────────────────────────────────────────────── */

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 8, 0);

    /* ---- header ---- */
    s_mode_badge = lv_label_create(scr);
    lv_label_set_text(s_mode_badge, "INIT");
    lv_obj_set_style_text_font(s_mode_badge, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_mode_badge, COL_ACCENT, 0);
    lv_obj_set_pos(s_mode_badge, 12, 6);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Pi-LOG  LIVE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, COL_DIM, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    s_link_lbl = lv_label_create(scr);
    lv_label_set_text(s_link_lbl, "NO LINK");
    lv_obj_set_style_text_font(s_link_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_link_lbl, COL_ERR, 0);
    lv_obj_align(s_link_lbl, LV_ALIGN_TOP_RIGHT, -12, 12);

    /* ---- altitude, deliberately the largest thing on screen ---- */
    lv_obj_t *alt_panel = make_panel(scr, 480, 210);
    lv_obj_set_pos(alt_panel, 4, 48);

    lv_obj_t *alt_cap = lv_label_create(alt_panel);
    lv_label_set_text(alt_cap, "ALTITUDE  (above pad)");
    lv_obj_set_style_text_color(alt_cap, COL_DIM, 0);
    lv_obj_set_style_text_font(alt_cap, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(alt_cap, 4, 0);

    s_alt_val = lv_label_create(alt_panel);
    lv_label_set_text(s_alt_val, "0");
    lv_obj_set_style_text_color(s_alt_val, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_alt_val, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_alt_val, 4, 40);

    s_alt_unit = lv_label_create(alt_panel);
    lv_label_set_text(s_alt_unit, "m");
    lv_obj_set_style_text_color(s_alt_unit, COL_DIM, 0);
    lv_obj_set_style_text_font(s_alt_unit, &lv_font_montserrat_28, 0);
    lv_obj_align(s_alt_unit, LV_ALIGN_BOTTOM_LEFT, 4, -8);

    /* ---- velocity + acceleration ---- */
    lv_obj_t *mot = make_panel(scr, 520, 210);
    lv_obj_set_pos(mot, 492, 48);

    s_vvel_val = make_stat(mot, "VERTICAL VEL  m/s",   4,   0, &lv_font_montserrat_28);
    s_hvel_val = make_stat(mot, "HORIZONTAL VEL  m/s", 260, 0, &lv_font_montserrat_28);
    s_ax_val   = make_stat(mot, "ACCEL X  g",   4,  90, &lv_font_montserrat_20);
    s_ay_val   = make_stat(mot, "ACCEL Y  g", 170,  90, &lv_font_montserrat_20);
    s_az_val   = make_stat(mot, "ACCEL Z  g", 336,  90, &lv_font_montserrat_20);

    /* ---- rolling altitude chart ---- */
    lv_obj_t *chart_panel = make_panel(scr, 480, 300);
    lv_obj_set_pos(chart_panel, 4, 264);

    lv_obj_t *ch_cap = lv_label_create(chart_panel);
    lv_label_set_text(ch_cap, "ALTITUDE TRACE");
    lv_obj_set_style_text_color(ch_cap, COL_DIM, 0);
    lv_obj_set_style_text_font(ch_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ch_cap, 4, 0);

    s_chart = lv_chart_create(chart_panel);
    lv_obj_set_size(s_chart, 440, 236);
    lv_obj_set_pos(s_chart, 4, 22);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(s_chart, COL_BG, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);   /* line only */
    s_alt_series = lv_chart_add_series(s_chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    /* ---- environment + power + position ---- */
    lv_obj_t *env = make_panel(scr, 520, 300);
    lv_obj_set_pos(env, 492, 264);

    s_temp_val  = make_stat(env, "TEMP  C",     4,   0, &lv_font_montserrat_20);
    s_press_val = make_stat(env, "PRESS  kPa", 130,  0, &lv_font_montserrat_20);
    s_hum_val   = make_stat(env, "HUM  %",     256,  0, &lv_font_montserrat_20);
    s_gas_val   = make_stat(env, "GAS  kOhm",  382,  0, &lv_font_montserrat_20);

    s_volt_val  = make_stat(env, "BUS  V",      4,  78, &lv_font_montserrat_20);
    s_curr_val  = make_stat(env, "CURRENT  mA",130, 78, &lv_font_montserrat_20);
    s_pwr_val   = make_stat(env, "POWER  W",   256, 78, &lv_font_montserrat_20);

    s_lat_val   = make_stat(env, "LATITUDE",     4, 156, &lv_font_montserrat_20);
    s_lon_val   = make_stat(env, "LONGITUDE", 190, 156, &lv_font_montserrat_20);
    s_sat_val   = make_stat(env, "SATS",       380, 156, &lv_font_montserrat_20);

    /* ---- stale-data banner, hidden until the link goes quiet ---- */
    s_stale_banner = lv_label_create(scr);
    lv_label_set_text(s_stale_banner, "SIGNAL LOST");
    lv_obj_set_style_text_font(s_stale_banner, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_stale_banner, COL_ERR, 0);
    lv_obj_set_style_bg_color(s_stale_banner, lv_color_hex(0x2D0F12), 0);
    lv_obj_set_style_bg_opa(s_stale_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_stale_banner, 10, 0);
    lv_obj_set_style_radius(s_stale_banner, 8, 0);
    lv_obj_align(s_stale_banner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_stale_banner, LV_OBJ_FLAG_HIDDEN);
}

/* ── updates ──────────────────────────────────────────────────────────── */

static void set_f(lv_obj_t *lbl, const char *fmt, float v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, v);
    lv_label_set_text(lbl, buf);
}

void ui_update(const telem_state_t *st, bool link_ok, uint32_t age_ms, float pkt_rate)
{
    char buf[64];

    /* Mode badge, coloured by how much attention it deserves. */
    lv_label_set_text(s_mode_badge, telemetry_mode_name(st->mode));
    lv_color_t mode_col = COL_ACCENT;
    if (st->mode == TELEM_MODE_BOOST)      mode_col = COL_ERR;
    else if (st->mode == TELEM_MODE_COAST) mode_col = COL_WARN;
    else if (st->mode == TELEM_MODE_ARMED) mode_col = COL_OK;
    lv_obj_set_style_text_color(s_mode_badge, mode_col, 0);

    /* Link health. CRC failures are expected under load - the transports drop
     * whole chunks rather than stall the payload - so they are shown but not
     * treated as an error state. */
    snprintf(buf, sizeof(buf), "%s  %.0f pkt/s  crc %lu%s",
             link_ok ? "LINK" : "NO LINK", (double)pkt_rate,
             (unsigned long)st->frames_crc_fail,
             st->have_calib ? "" : "  [no calib]");
    lv_label_set_text(s_link_lbl, buf);
    lv_obj_set_style_text_color(s_link_lbl, link_ok ? COL_OK : COL_ERR, 0);

    set_f(s_alt_val,   "%.0f",  st->altitude);
    set_f(s_vvel_val,  "%.1f",  st->vertical_velocity);
    set_f(s_hvel_val,  "%.1f",  st->horizontal_velocity);
    set_f(s_ax_val,    "%.2f",  st->accel_x);
    set_f(s_ay_val,    "%.2f",  st->accel_y);
    set_f(s_az_val,    "%.2f",  st->accel_z);

    set_f(s_temp_val,  "%.1f",  st->temperature);
    set_f(s_press_val, "%.2f",  st->pressure);
    set_f(s_hum_val,   "%.0f",  st->humidity);
    set_f(s_gas_val,   "%.1f",  st->gas_resistance / 1000.0f);

    set_f(s_volt_val,  "%.2f",  st->bus_voltage);
    set_f(s_curr_val,  "%.0f",  st->current);
    set_f(s_pwr_val,   "%.2f",  st->power);

    /* Position stays blank rather than showing 0.000000 before a fix - a
     * plausible-looking zero is worse than an obvious placeholder. */
    if (st->gps_fix) {
        snprintf(buf, sizeof(buf), "%.5f", st->gps_lat);
        lv_label_set_text(s_lat_val, buf);
        snprintf(buf, sizeof(buf), "%.5f", st->gps_lon);
        lv_label_set_text(s_lon_val, buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)st->sat_count);
        lv_label_set_text(s_sat_val, buf);
        lv_obj_set_style_text_color(s_sat_val, COL_OK, 0);
    } else {
        lv_label_set_text(s_lat_val, "--");
        lv_label_set_text(s_lon_val, "--");
        lv_label_set_text(s_sat_val, "NO FIX");
        lv_obj_set_style_text_color(s_sat_val, COL_WARN, 0);
    }

    lv_chart_set_next_value(s_chart, s_alt_series, (int32_t)st->altitude);

    if (link_ok) {
        lv_obj_add_flag(s_stale_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(buf, sizeof(buf), "SIGNAL LOST  %lus", (unsigned long)(age_ms / 1000));
        lv_label_set_text(s_stale_banner, buf);
        lv_obj_clear_flag(s_stale_banner, LV_OBJ_FLAG_HIDDEN);
    }
}
