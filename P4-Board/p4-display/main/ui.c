/**
 * @file ui.c
 * @brief LVGL ground-station UI - see ui.h.
 *
 *   +------------------------------------------------------------------+
 *   | MODE          Pi-LOG GROUND STATION      LINK  SD  T+hh:mm:ss    |  <- top layer,
 *   +------------------------------------------------------------------+     every screen
 *   | [ IMU      ]  [ BAROMETER ]  [ GAS       ]                        |
 *   | [ POWER    ]  [ GPS       ]  [ RADIO LINK]   <- overview tiles    |
 *   +------------------------------------------------------------------+
 *
 * Touching a tile opens that sensor's page: every value on the left, one or
 * two auto-scaling graphs on the right, Back in the corner.
 *
 * Everything is created once. Labels are only rewritten when their text
 * changes, and only on the visible screen, so a 10 Hz refresh costs little.
 * Graphs are fed on every screen, so a page shows its history when opened.
 */

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui";

/* Palette chosen for outdoor legibility: near-black background, high-contrast
 * text, colour reserved for state that matters. */
#define COL_BG        lv_color_hex(0x0B0F14)
#define COL_PANEL     lv_color_hex(0x161C24)
#define COL_PRESSED   lv_color_hex(0x232B36)
#define COL_TEXT      lv_color_hex(0xE6EDF3)
#define COL_DIM       lv_color_hex(0x8B98A5)
#define COL_ACCENT    lv_color_hex(0x58A6FF)
#define COL_OK        lv_color_hex(0x3FB950)
#define COL_WARN      lv_color_hex(0xD29922)
#define COL_ERR       lv_color_hex(0xF85149)
#define COL_X         lv_color_hex(0xF85149)
#define COL_Y         lv_color_hex(0x3FB950)
#define COL_Z         lv_color_hex(0x58A6FF)

#define SCR_W   1024
#define SCR_H   600
#define HDR_H   64
#define GAP     12

#define FAST_POINTS  300     /* 10 Hz sampling -> 30 s on screen */
#define SLOW_POINTS  180     /* 1 Hz sampling  -> 3 min on screen */
#define MAX_ROWS     9
#define MAX_CHARTS   2
#define MAX_SERIES   3

/* ── pages ────────────────────────────────────────────────────────────── */

enum { PG_IMU, PG_BARO, PG_GAS, PG_POWER, PG_GPS, PG_LINK, PG_COUNT };

static const char *const PAGE_NAME[PG_COUNT] = {
    "IMU", "BAROMETER", "GAS", "POWER", "GPS", "RADIO LINK" };
static const char *const PAGE_CHIP[PG_COUNT] = {
    "MPU6500  accel + gyro", "BME680  temp / pressure / humidity",
    "BME680  gas resistance", "INA219  supply monitor",
    "SAM-M10Q  GNSS", "ESP-NOW  channel 6  (2437 MHz)" };
/* page -> decoder sensor stream; the link page has none */
static const int PAGE_SENSOR[PG_COUNT] = {
    TELEM_SENSOR_IMU, TELEM_SENSOR_BARO, TELEM_SENSOR_GAS,
    TELEM_SENSOR_POWER, TELEM_SENSOR_GPS, -1 };

typedef struct {
    lv_obj_t          *obj;
    lv_chart_series_t *ser[MAX_SERIES];
    int                nser;
    float              scale;      /* value * scale -> chart int */
    int                decimals;   /* for the range label */
    const char        *unit;
    lv_obj_t          *range;      /* "min ... max unit" */
    bool               scatter;
} chart_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *status;
    lv_obj_t *row[MAX_ROWS];
    lv_obj_t *err[MAX_ROWS];      /* datasheet uncertainty, small line under the value */
    int       nrows;
    chart_t   chart[MAX_CHARTS];
    int       ncharts;
} page_t;

typedef struct {
    lv_obj_t *obj, *status, *rate, *summary;
} tile_t;

static lv_obj_t *s_main;
static tile_t    s_tile[PG_COUNT];
static page_t    s_page[PG_COUNT];
static int       s_current = -1;          /* -1 = overview */

static lv_obj_t *s_mode_badge, *s_link_lbl, *s_sd_lbl, *s_uptime_lbl, *s_stale_banner;

/* ── small helpers ────────────────────────────────────────────────────── */

/* LVGL redraws on every set_text, even an identical one. */
static void set_txt(lv_obj_t *l, const char *s)
{
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, s) == 0) return;
    lv_label_set_text(l, s);
}

static void set_col(lv_obj_t *l, lv_color_t c)
{
    lv_obj_set_style_text_color(l, c, 0);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       lv_color_t col, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_set_scrollable(p, false);
    return p;
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);
    return scr;
}

/* ── sensor state ─────────────────────────────────────────────────────── */

typedef enum { SS_NOLINK, SS_WAIT, SS_ON, SS_SILENT, SS_MISSING, SS_PAUSED } sstate_t;

static sstate_t sensor_state(const ui_model_t *m, int s)
{
    if (!m->link_ok) return SS_NOLINK;
    /* Slack beyond each sensor's own period before calling it silent. */
    uint32_t limit = (s == TELEM_SENSOR_GPS) ? 3500 : (s == TELEM_SENSOR_GAS) ? 15000 : 2000;
    if (!m->sensor_seen[s]) {
        return (m->link_up_ms < limit) ? SS_WAIT : SS_MISSING;
    }
    if (m->sensor_age_ms[s] <= limit) return SS_ON;
    /* The firmware deliberately pauses the gas heater during the burn. */
    if (s == TELEM_SENSOR_GAS && m->st.mode == TELEM_MODE_BOOST) return SS_PAUSED;
    return SS_SILENT;
}

static void show_state(lv_obj_t *l, sstate_t st)
{
    static const char *const TXT[] = {
        "\xE2\x80\xA2 NO LINK", "\xE2\x80\xA2 WAITING", "\xE2\x80\xA2 ON",
        "\xE2\x80\xA2 SILENT", "\xE2\x80\xA2 NOT DETECTED", "\xE2\x80\xA2 PAUSED (BOOST)" };
    const lv_color_t col[] = { COL_DIM, COL_DIM, COL_OK, COL_ERR, COL_ERR, COL_WARN };
    set_txt(l, TXT[st]);
    set_col(l, col[st]);
}

static void fmt_rate(char *buf, size_t n, float hz, const char *unit)
{
    if (hz < 0.05f)      snprintf(buf, n, "-- %s", unit);
    else if (hz < 10.0f) snprintf(buf, n, "%.1f %s", (double)hz, unit);
    else                 snprintf(buf, n, "%.0f %s", (double)hz, unit);
}

/* ── navigation (this is the touch path) ──────────────────────────────── */

static void open_page(int pg)
{
    s_current = pg;
    lv_screen_load(pg < 0 ? s_main : s_page[pg].scr);
    ESP_LOGI(TAG, "touch: %s", pg < 0 ? "overview" : PAGE_NAME[pg]);
}

static void on_tile(lv_event_t *e)
{
    open_page((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_back(lv_event_t *e)
{
    (void)e;
    open_page(-1);
}

/* ── header (top layer: stays put across screens) ─────────────────────── */

static void build_header(void)
{
    lv_obj_t *top = lv_layer_top();

    lv_obj_t *bar = lv_obj_create(top);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, SCR_W, HDR_H);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_clickable(bar, false);   /* touches fall through to the screen */

    s_mode_badge = label(bar, "INIT", &lv_font_montserrat_28, COL_ACCENT, 4, 2);
    label(bar, "Pi-LOG  GROUND STATION", &lv_font_montserrat_16, COL_DIM, 170, 10);

    s_uptime_lbl = label(bar, "T+ --:--:--", &lv_font_montserrat_16, COL_DIM, 0, 10);
    lv_obj_align(s_uptime_lbl, LV_ALIGN_TOP_RIGHT, -4, 0);
    s_sd_lbl = label(bar, "NO SD", &lv_font_montserrat_16, COL_DIM, 0, 10);
    lv_obj_align(s_sd_lbl, LV_ALIGN_TOP_RIGHT, -150, 0);
    s_link_lbl = label(bar, "NO LINK", &lv_font_montserrat_16, COL_ERR, 0, 10);
    lv_obj_align(s_link_lbl, LV_ALIGN_TOP_RIGHT, -380, 0);

    s_stale_banner = lv_label_create(top);
    lv_obj_set_style_text_font(s_stale_banner, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_stale_banner, COL_ERR, 0);
    lv_obj_set_style_bg_color(s_stale_banner, lv_color_hex(0x2D0F12), 0);
    lv_obj_set_style_bg_opa(s_stale_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_stale_banner, 12, 0);
    lv_obj_set_style_radius(s_stale_banner, 8, 0);
    lv_label_set_text(s_stale_banner, "NO SIGNAL YET");
    lv_obj_align(s_stale_banner, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/* ── overview ─────────────────────────────────────────────────────────── */

static void build_overview(void)
{
    s_main = new_screen();

    const int32_t tw = (SCR_W - 4 * GAP) / 3;
    const int32_t th = (SCR_H - HDR_H - 3 * GAP) / 2;

    for (int pg = 0; pg < PG_COUNT; pg++) {
        int32_t x = GAP + (pg % 3) * (tw + GAP);
        int32_t y = HDR_H + GAP + (pg / 3) * (th + GAP);
        tile_t *t = &s_tile[pg];

        t->obj = panel(s_main, x, y, tw, th);
        lv_obj_set_style_bg_color(t->obj, COL_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_clickable(t->obj, true);
        lv_obj_add_event_cb(t->obj, on_tile, LV_EVENT_CLICKED, (void *)(intptr_t)pg);

        label(t->obj, PAGE_NAME[pg], &lv_font_montserrat_28, COL_TEXT, 0, 0);
        label(t->obj, PAGE_CHIP[pg], &lv_font_montserrat_14, COL_DIM, 0, 38);
        lv_obj_t *chev = label(t->obj, LV_SYMBOL_RIGHT, &lv_font_montserrat_20, COL_DIM, 0, 0);
        lv_obj_align(chev, LV_ALIGN_TOP_RIGHT, 0, 4);

        t->status  = label(t->obj, "\xE2\x80\xA2 NO LINK", &lv_font_montserrat_20, COL_DIM, 0, 70);
        t->rate    = label(t->obj, "-- Hz", &lv_font_montserrat_48, COL_TEXT, 0, 102);
        t->summary = label(t->obj, "", &lv_font_montserrat_16, COL_DIM, 0, th - 24 - 22);
        lv_obj_set_width(t->summary, tw - 24);
        lv_label_set_long_mode(t->summary, LV_LABEL_LONG_MODE_CLIP);
    }
}

/* ── detail pages ─────────────────────────────────────────────────────── */

#define PAGE_TOP    (HDR_H + GAP)
#define BODY_TOP    (PAGE_TOP + 60)
#define BODY_H      (SCR_H - BODY_TOP - GAP)
#define VALUES_W    380
#define CHART_X     (GAP + VALUES_W + GAP)
#define CHART_W     (SCR_W - CHART_X - GAP)

static lv_obj_t *s_values_panel[PG_COUNT];

static void page_begin(int pg)
{
    page_t *p = &s_page[pg];
    p->scr = new_screen();

    lv_obj_t *back = lv_button_create(p->scr);
    lv_obj_set_pos(back, GAP, PAGE_TOP);
    lv_obj_set_size(back, 140, 48);
    lv_obj_set_style_bg_color(back, COL_PANEL, 0);
    lv_obj_set_style_bg_color(back, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = label(back, LV_SYMBOL_LEFT "  Back", &lv_font_montserrat_20, COL_TEXT, 0, 0);
    lv_obj_center(bl);

    label(p->scr, PAGE_NAME[pg], &lv_font_montserrat_28, COL_TEXT, GAP + 160, PAGE_TOP + 2);
    label(p->scr, PAGE_CHIP[pg], &lv_font_montserrat_14, COL_DIM, GAP + 160, PAGE_TOP + 36);
    p->status = label(p->scr, "", &lv_font_montserrat_20, COL_DIM, 0, PAGE_TOP + 12);
    lv_obj_align(p->status, LV_ALIGN_TOP_RIGHT, -GAP, PAGE_TOP + 12);

    s_values_panel[pg] = panel(p->scr, GAP, BODY_TOP, VALUES_W, BODY_H);
}

/* One "caption ........ value" line, uncertainty in small print under the
 * value; returns the value label. */
static lv_obj_t *page_row(int pg, const char *caption)
{
    page_t *p = &s_page[pg];
    lv_obj_t *vp = s_values_panel[pg];
    int32_t y = p->nrows * 46;
    label(vp, caption, &lv_font_montserrat_16, COL_DIM, 0, y + 4);
    lv_obj_t *v = label(vp, "--", &lv_font_montserrat_20, COL_TEXT, 0, y);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, y);
    lv_obj_t *e = label(vp, "", &lv_font_montserrat_14, COL_DIM, 0, y + 25);
    lv_obj_align(e, LV_ALIGN_TOP_RIGHT, 0, y + 25);
    p->err[p->nrows] = e;
    p->row[p->nrows++] = v;
    return v;
}

static void row_set(int pg, int r, const char *txt)
{
    lv_obj_t *v = s_page[pg].row[r];
    set_txt(v, txt);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, r * 46);   /* keep right-aligned as width changes */
}

static void err_set(int pg, int r, const char *txt)
{
    lv_obj_t *e = s_page[pg].err[r];
    set_txt(e, txt);
    lv_obj_align(e, LV_ALIGN_TOP_RIGHT, 0, r * 46 + 25);
}

/* Where the uncertainties come from, at the foot of the values panel. */
static void page_note(int pg, const char *txt)
{
    lv_obj_t *n = label(s_values_panel[pg], txt, &lv_font_montserrat_14, COL_DIM, 0, 0);
    lv_obj_set_width(n, VALUES_W - 24);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static chart_t *page_chart(int pg, int total, const char *title, const char *unit,
                           float scale, int decimals, uint32_t points, bool scatter)
{
    page_t *p = &s_page[pg];
    int idx = p->ncharts++;
    int32_t h = (BODY_H - (total - 1) * GAP) / total;   /* stacked, sharing the height */
    int32_t y = BODY_TOP + idx * (h + GAP);

    lv_obj_t *box = panel(p->scr, CHART_X, y, CHART_W, h);
    chart_t *c = &p->chart[idx];
    memset(c, 0, sizeof(*c));
    c->scale = scale;
    c->decimals = decimals;
    c->unit = unit;
    c->scatter = scatter;

    label(box, title, &lv_font_montserrat_16, COL_DIM, 0, 0);
    c->range = label(box, "", &lv_font_montserrat_14, COL_DIM, 0, 2);
    lv_obj_align(c->range, LV_ALIGN_TOP_RIGHT, 0, 2);

    c->obj = lv_chart_create(box);
    lv_obj_set_pos(c->obj, 0, 24);
    lv_obj_set_size(c->obj, CHART_W - 24, h - 24 - 24);
    lv_chart_set_type(c->obj, scatter ? LV_CHART_TYPE_SCATTER : LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c->obj, points);
    lv_chart_set_update_mode(c->obj, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(c->obj, 4, scatter ? 4 : 0);
    lv_obj_set_style_bg_color(c->obj, COL_BG, 0);
    lv_obj_set_style_border_width(c->obj, 0, 0);
    lv_obj_set_style_line_color(c->obj, lv_color_hex(0x232B36), LV_PART_MAIN);
    if (scatter) {
        lv_obj_set_style_line_width(c->obj, 0, LV_PART_ITEMS);
        lv_obj_set_style_size(c->obj, 5, 5, LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_line_width(c->obj, 2, LV_PART_ITEMS);
        lv_obj_set_style_size(c->obj, 0, 0, LV_PART_INDICATOR);   /* line only */
    }
    return c;
}

static void chart_series(chart_t *c, lv_color_t col)
{
    c->ser[c->nser++] = lv_chart_add_series(c->obj, col, LV_CHART_AXIS_PRIMARY_Y);
}

static void chart_push(chart_t *c, int s, float v)
{
    lv_chart_set_next_value(c->obj, c->ser[s], (int32_t)lroundf(v * c->scale));
}

/* Rescale the visible chart to its data, with some headroom. */
static void chart_autorange(chart_t *c)
{
    uint32_t n = lv_chart_get_point_count(c->obj);
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (int s = 0; s < c->nser; s++) {
        const int32_t *y = lv_chart_get_series_y_array(c->obj, c->ser[s]);
        for (uint32_t i = 0; i < n; i++) {
            if (y[i] == LV_CHART_POINT_NONE) continue;
            if (y[i] < lo) lo = y[i];
            if (y[i] > hi) hi = y[i];
        }
    }
    if (lo > hi) {                       /* no data yet */
        set_txt(c->range, "no data");
        return;
    }
    int32_t span = hi - lo;
    int32_t pad  = span / 8 + 2;
    lv_chart_set_axis_range(c->obj, LV_CHART_AXIS_PRIMARY_Y, lo - pad, hi + pad);

    char buf[48];
    snprintf(buf, sizeof(buf), "%.*f ... %.*f %s",
             c->decimals, (double)(lo / c->scale), c->decimals, (double)(hi / c->scale), c->unit);
    set_txt(c->range, buf);
}

/* Scatter: equal scaling on both axes, centred on the first fix. */
static void scatter_autorange(chart_t *c)
{
    uint32_t n = lv_chart_get_point_count(c->obj);
    const int32_t *x = lv_chart_get_series_x_array(c->obj, c->ser[0]);
    const int32_t *y = lv_chart_get_series_y_array(c->obj, c->ser[0]);
    int32_t m = 0;
    bool any = false;
    for (uint32_t i = 0; i < n; i++) {
        if (y[i] == LV_CHART_POINT_NONE) continue;
        any = true;
        int32_t ax = x[i] < 0 ? -x[i] : x[i];
        int32_t ay = y[i] < 0 ? -y[i] : y[i];
        if (ax > m) m = ax;
        if (ay > m) m = ay;
    }
    if (!any) {
        set_txt(c->range, "waiting for a fix");
        return;
    }
    m = m + m / 8 + 10;   /* at least +-1 m */
    lv_chart_set_axis_range(c->obj, LV_CHART_AXIS_PRIMARY_X, -m, m);
    lv_chart_set_axis_range(c->obj, LV_CHART_AXIS_PRIMARY_Y, -m, m);
    char buf[48];
    snprintf(buf, sizeof(buf), "+-%.1f m around first fix", (double)(m / c->scale));
    set_txt(c->range, buf);
}

static void build_pages(void)
{
    chart_t *c;

    page_begin(PG_IMU);
    page_row(PG_IMU, "Accel X");   page_row(PG_IMU, "Accel Y");   page_row(PG_IMU, "Accel Z");
    page_row(PG_IMU, "|Accel|");
    page_row(PG_IMU, "Gyro X");    page_row(PG_IMU, "Gyro Y");    page_row(PG_IMU, "Gyro Z");
    page_row(PG_IMU, "Die temperature");
    page_row(PG_IMU, "Sample rate");
    c = page_chart(PG_IMU, 2, "Acceleration   X / Y / Z", "g", 1000.0f, 2, FAST_POINTS, false);
    chart_series(c, COL_X); chart_series(c, COL_Y); chart_series(c, COL_Z);
    c = page_chart(PG_IMU, 2, "Rotation rate   X / Y / Z", "dps", 10.0f, 1, FAST_POINTS, false);
    chart_series(c, COL_X); chart_series(c, COL_Y); chart_series(c, COL_Z);
    page_note(PG_IMU, "+/- MPU-6500 datasheet rev 1.1, uncalibrated, RSS");

    page_begin(PG_BARO);
    page_row(PG_BARO, "Temperature");
    page_row(PG_BARO, "Pressure");
    page_row(PG_BARO, "Humidity");
    page_row(PG_BARO, "Altitude (above start)");
    page_row(PG_BARO, "Vertical speed");
    page_row(PG_BARO, "Sample rate");
    c = page_chart(PG_BARO, 2, "Altitude above start", "m", 100.0f, 2, FAST_POINTS, false);
    chart_series(c, COL_ACCENT);
    c = page_chart(PG_BARO, 2, "Temperature", "\xC2\xB0" "C", 100.0f, 2, FAST_POINTS, false);
    chart_series(c, COL_WARN);
    page_note(PG_BARO, "+/- BME680 datasheet; altitude: relative accuracy; board self-heating not included");

    page_begin(PG_GAS);
    page_row(PG_GAS, "Gas resistance");
    page_row(PG_GAS, "Heater");
    page_row(PG_GAS, "Sample rate");
    c = page_chart(PG_GAS, 1, "Gas resistance  (30 min)", "kOhm", 100.0f, 2, SLOW_POINTS, false);
    chart_series(c, COL_OK);
    page_note(PG_GAS, "No absolute spec: +/-15 % is Bosch's sensor-to-sensor deviation");

    page_begin(PG_POWER);
    page_row(PG_POWER, "Bus voltage");
    page_row(PG_POWER, "Current");
    page_row(PG_POWER, "Power");
    page_row(PG_POWER, "Energy used (since boot)");
    page_row(PG_POWER, "Sample rate");
    c = page_chart(PG_POWER, 2, "Power", "W", 1000.0f, 3, FAST_POINTS, false);
    chart_series(c, COL_WARN);
    c = page_chart(PG_POWER, 2, "Current", "mA", 10.0f, 1, FAST_POINTS, false);
    chart_series(c, COL_ACCENT);
    page_note(PG_POWER, "+/- INA219 A-grade (PGA /8) worst case + 1 % shunt; energy before the first sample is estimated");

    page_begin(PG_GPS);
    page_row(PG_GPS, "Fix");
    page_row(PG_GPS, "Satellites");
    page_row(PG_GPS, "Latitude");
    page_row(PG_GPS, "Longitude");
    page_row(PG_GPS, "Altitude (MSL)");
    page_row(PG_GPS, "Ground speed");
    page_row(PG_GPS, "Update rate");
    c = page_chart(PG_GPS, 2, "Position wander  (east / north)", "m", 10.0f, 1, FAST_POINTS, true);
    chart_series(c, COL_ACCENT);
    c = page_chart(PG_GPS, 2, "Satellites  (3 min)", "sats", 1.0f, 0, SLOW_POINTS, false);
    chart_series(c, COL_OK);
    page_note(PG_GPS, "+/- SAM-M10Q datasheet: CEP 50 %, open sky, >6 satellites per system");

    page_begin(PG_LINK);
    page_row(PG_LINK, "Packets / s");
    page_row(PG_LINK, "Frames / s");
    page_row(PG_LINK, "Checksum failures");
    page_row(PG_LINK, "Last packet");
    page_row(PG_LINK, "Channel");
    page_row(PG_LINK, "Calibration");
    page_row(PG_LINK, "SD backup");
    page_row(PG_LINK, "Backup dropped");
    c = page_chart(PG_LINK, 2, "Packets per second  (3 min)", "pkt/s", 10.0f, 1, SLOW_POINTS, false);
    chart_series(c, COL_ACCENT);
    c = page_chart(PG_LINK, 2, "Frames per second  (3 min)", "fr/s", 1.0f, 0, SLOW_POINTS, false);
    chart_series(c, COL_OK);
}

void ui_create(void)
{
    build_overview();
    build_pages();
    build_header();
    lv_screen_load(s_main);
}

/* ── refresh ──────────────────────────────────────────────────────────── */

static void fmt_mb(char *buf, size_t n, uint32_t bytes)
{
    if (bytes < 1024 * 1024) snprintf(buf, n, "%lu kB", (unsigned long)(bytes / 1024));
    else                     snprintf(buf, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

static void update_header(const ui_model_t *m)
{
    char buf[64];
    const telem_state_t *st = &m->st;

    set_txt(s_mode_badge, telemetry_mode_name(st->mode));
    lv_color_t mc = COL_ACCENT;
    if (st->mode == TELEM_MODE_BOOST)      mc = COL_ERR;
    else if (st->mode == TELEM_MODE_COAST) mc = COL_WARN;
    else if (st->mode == TELEM_MODE_ARMED) mc = COL_OK;
    set_col(s_mode_badge, mc);

    if (m->link_ok) {
        snprintf(buf, sizeof(buf), "LINK  %.0f pkt/s%s", (double)m->pkt_rate,
                 st->have_calib ? "" : "  [no calib]");
        set_txt(s_link_lbl, buf);
        set_col(s_link_lbl, COL_OK);
    } else {
        set_txt(s_link_lbl, "NO LINK");
        set_col(s_link_lbl, COL_ERR);
    }

    if (m->sd.active) {
        char mb[16];
        fmt_mb(mb, sizeof(mb), m->sd.bytes);
        snprintf(buf, sizeof(buf), "SD REC %s", mb);
        set_txt(s_sd_lbl, buf);
        set_col(s_sd_lbl, m->sd.dropped ? COL_WARN : COL_OK);
    } else {
        set_txt(s_sd_lbl, "NO SD");
        set_col(s_sd_lbl, COL_DIM);
    }

    uint32_t s = st->timestamp_ms / 1000;
    if (st->timestamp_ms) {
        snprintf(buf, sizeof(buf), "T+ %02lu:%02lu:%02lu", (unsigned long)(s / 3600),
                 (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
        set_txt(s_uptime_lbl, buf);
    }

    if (m->link_ok) {
        lv_obj_set_hidden(s_stale_banner, true);
    } else {
        if (m->age_ms == UINT32_MAX) {
            set_txt(s_stale_banner, "NO SIGNAL YET");
        } else {
            snprintf(buf, sizeof(buf), "SIGNAL LOST  %lu s", (unsigned long)(m->age_ms / 1000));
            set_txt(s_stale_banner, buf);
        }
        lv_obj_set_hidden(s_stale_banner, false);
    }
}

static void update_overview(const ui_model_t *m)
{
    char buf[64];
    const telem_state_t *st = &m->st;

    for (int pg = 0; pg < PG_COUNT; pg++) {
        tile_t *t = &s_tile[pg];
        int s = PAGE_SENSOR[pg];
        if (s >= 0) {
            show_state(t->status, sensor_state(m, s));
            fmt_rate(buf, sizeof(buf), m->sensor_hz[s], "Hz");
        } else {
            show_state(t->status, m->link_ok ? SS_ON : SS_NOLINK);
            fmt_rate(buf, sizeof(buf), m->pkt_rate, "pkt/s");
        }
        set_txt(t->rate, buf);
    }

    snprintf(buf, sizeof(buf), "|a| %.3f g   %.1f \xC2\xB0" "C", (double)st->accel_mag, (double)st->imu_temp);
    set_txt(s_tile[PG_IMU].summary, buf);
    snprintf(buf, sizeof(buf), "%.1f \xC2\xB0" "C   %.2f kPa   %.0f %%",
             (double)st->temperature, (double)st->pressure, (double)st->humidity);
    set_txt(s_tile[PG_BARO].summary, buf);
    snprintf(buf, sizeof(buf), "%.1f kOhm", (double)(st->gas_resistance / 1000.0f));
    set_txt(s_tile[PG_GAS].summary, buf);
    snprintf(buf, sizeof(buf), "%.2f V   %.0f mA   %.2f W",
             (double)st->bus_voltage, (double)st->current, (double)st->power);
    set_txt(s_tile[PG_POWER].summary, buf);
    snprintf(buf, sizeof(buf), "%s   %u satellites", st->gps_fix ? "FIX" : "NO FIX",
             (unsigned)st->sat_count);
    set_txt(s_tile[PG_GPS].summary, buf);
    snprintf(buf, sizeof(buf), "%.0f frames/s   %lu CRC fails", (double)m->frame_rate,
             (unsigned long)st->frames_crc_fail);
    set_txt(s_tile[PG_LINK].summary, buf);
}

/* ── datasheet accuracy ───────────────────────────────────────────────────
 * Every +/- on the detail pages comes from the sensor datasheets, independent
 * terms combined as root-sum-square, for uncalibrated parts. Board-level
 * effects (self-heating, mounting stress) are not in any datasheet and are
 * not included.
 *
 *  MPU-6500  PS-MPU-6500A-01 rev 1.1
 *    accel  zero-g initial +-60 mg; zero-g drift X/Y +-0.64, Z +-1 mg/degC;
 *           sensitivity +-3 %; nonlinearity +-0.5 % of FS; cross-axis +-2 %
 *    gyro   initial ZRO +-5 dps; ZRO drift +-0.24 dps/degC; sensitivity +-3 %;
 *           nonlinearity +-0.1 % of FS; cross-axis +-2 %
 *    die temperature: "Sensitivity Untrimmed" - no accuracy specified
 *  BME680  BST-BME680-DS001 rev 1.9
 *    temperature +-1.0 degC (0..65 degC); pressure absolute +-0.6 hPa,
 *    relative +-0.12 hPa, offset drift +-1.3 Pa/K, noise 1.4 Pa RMS (full
 *    bandwidth); humidity +-3 %RH (20..80 %RH); gas: no absolute spec, IAQ
 *    sensor-to-sensor deviation +-15 %
 *  INA219  SBOS448G, A grade worst case, PGA /8, 25 degC maximum
 *    shunt offset +-200 uV; current error +-0.5 %; bus voltage error +-0.5 %;
 *    bus LSB 4 mV - plus +-1 % for the 0.1 ohm shunt resistor (assumption)
 *  SAM-M10Q  UBX-22013293 R05
 *    position 1.5 m CEP (50 %, open sky, >6 SVs per system); velocity
 *    +-0.05 m/s; altitude: not specified
 */
#define ACC_ZG_G        0.060f      /* g */
#define ACC_ZG_TC_XY    0.00064f    /* g/degC */
#define ACC_ZG_TC_Z     0.001f      /* g/degC */
#define ACC_SENS        0.03f
#define ACC_NL          0.005f      /* of full scale */
#define ACC_XAX         0.02f
#define GYR_ZRO         5.0f        /* dps */
#define GYR_ZRO_TC      0.24f       /* dps/degC */
#define GYR_SENS        0.03f
#define GYR_NL          0.001f      /* of full scale */
#define GYR_XAX         0.02f
#define GYR_FS          500.0f      /* dps, as configured */
#define BME_T           1.0f        /* degC */
#define BME_P_ABS       0.06f       /* kPa */
#define BME_P_REL       0.012f      /* kPa */
#define BME_P_TC        0.0013f     /* kPa/K */
#define BME_P_NOISE     0.0014f     /* kPa RMS per sample */
#define BME_H           3.0f        /* %RH */
#define BME_GAS_S2S     0.15f
#define INA_OFFSET_MA   2.0f        /* 200 uV over 0.1 ohm */
#define INA_I_GAIN      0.005f
#define INA_SHUNT_TOL   0.01f
#define INA_V_GAIN      0.005f
#define INA_V_LSB       0.004f      /* V */
#define GPS_CEP_M       1.5f
#define GPS_VEL_MPS     0.05f

static float rss2(float a, float b) { return sqrtf(a * a + b * b); }

/* Accelerometer, one axis (0 = X, 1 = Y, 2 = Z), in g. */
static float acc_err(const telem_state_t *st, int axis)
{
    const float a[3] = { st->accel_x, st->accel_y, st->accel_z };
    float fs   = (st->mode == TELEM_MODE_BOOST) ? 16.0f : 2.0f;
    float drift = ((axis == 2) ? ACC_ZG_TC_Z : ACC_ZG_TC_XY) * fabsf(st->imu_temp - 25.0f);
    float perp = rss2(a[(axis + 1) % 3], a[(axis + 2) % 3]);
    return sqrtf(ACC_ZG_G * ACC_ZG_G + drift * drift
                 + powf(ACC_SENS * a[axis], 2) + powf(ACC_NL * fs, 2) + powf(ACC_XAX * perp, 2));
}

/* |accel|, propagated from the three axes, in g. */
static float accmag_err(const telem_state_t *st)
{
    const float a[3] = { st->accel_x, st->accel_y, st->accel_z };
    if (st->accel_mag <= 0.0f) return ACC_ZG_G;
    float s = 0.0f;
    for (int i = 0; i < 3; i++) s += powf(a[i] * acc_err(st, i), 2);
    return sqrtf(s) / st->accel_mag;
}

/* Gyro, one axis, in dps. */
static float gyro_err(const telem_state_t *st, int axis)
{
    const float w[3] = { st->roll, st->pitch, st->yaw };
    float drift = GYR_ZRO_TC * fabsf(st->imu_temp - 25.0f);
    float perp  = rss2(w[(axis + 1) % 3], w[(axis + 2) % 3]);
    return sqrtf(GYR_ZRO * GYR_ZRO + drift * drift
                 + powf(GYR_SENS * w[axis], 2) + powf(GYR_NL * GYR_FS, 2) + powf(GYR_XAX * perp, 2));
}

/* Metres of altitude per kPa at the current pressure, from
 * h = 44330 (1 - (p/p0)^(1/5.255)). */
static float dh_dp(const telem_state_t *st)
{
    float p0 = st->ref_pressure, p = st->pressure;
    if (p0 <= 0.0f || p <= 0.0f) return 84.0f;
    return 44330.0f / (5.255f * p0) * powf(p / p0, 1.0f / 5.255f - 1.0f);
}

/* Altitude above the reference: relative pressure accuracy plus the offset
 * drift since the reference was taken. */
static float alt_err(const telem_state_t *st)
{
    float drift = BME_P_TC * fabsf(st->temperature - st->ref_temperature);
    return dh_dp(st) * rss2(BME_P_REL, drift);
}

/* Vertical speed is a first difference of two altitudes: per-sample noise
 * dominates, sqrt(2) * sigma_h / dt. */
static float vspeed_err(const telem_state_t *st)
{
    if (st->vv_dt_s <= 0.0f) return NAN;
    return 1.41421356f * dh_dp(st) * BME_P_NOISE / st->vv_dt_s;
}

static float ina_v_err(const telem_state_t *st)
{
    return rss2(INA_V_GAIN * st->bus_voltage, INA_V_LSB / 3.4641f);   /* LSB / sqrt(12) */
}

static float ina_i_err(const telem_state_t *st)   /* mA */
{
    float i = st->current;
    return sqrtf(INA_OFFSET_MA * INA_OFFSET_MA + (INA_I_GAIN * INA_I_GAIN + INA_SHUNT_TOL * INA_SHUNT_TOL) * i * i);
}

static float ina_p_err(const telem_state_t *st)   /* W */
{
    return rss2(st->current * ina_v_err(st), st->bus_voltage * ina_i_err(st)) / 1000.0f;
}

/* "+/- 0.067 g" into a small-print label. */
static void err_fmt(int pg, int r, int decimals, float e, const char *unit)
{
    char b[40];
    if (isnan(e)) snprintf(b, sizeof(b), "+/- n/a");
    else          snprintf(b, sizeof(b), "+/- %.*f %s", decimals, (double)e, unit);
    err_set(pg, r, b);
}

static void update_page(const ui_model_t *m, int pg)
{
    char b[48];
    const telem_state_t *st = &m->st;
    int s = PAGE_SENSOR[pg];
    page_t *p = &s_page[pg];

    show_state(p->status, s >= 0 ? sensor_state(m, s) : (m->link_ok ? SS_ON : SS_NOLINK));
    if (s >= 0) fmt_rate(b, sizeof(b), m->sensor_hz[s], "Hz");

    switch (pg) {
    case PG_IMU: {
        char r[16];
        strcpy(r, b);
        snprintf(b, sizeof(b), "%+.3f g", (double)st->accel_x);   row_set(pg, 0, b);
        snprintf(b, sizeof(b), "%+.3f g", (double)st->accel_y);   row_set(pg, 1, b);
        snprintf(b, sizeof(b), "%+.3f g", (double)st->accel_z);   row_set(pg, 2, b);
        snprintf(b, sizeof(b), "%.3f g",  (double)st->accel_mag); row_set(pg, 3, b);
        snprintf(b, sizeof(b), "%+.1f dps", (double)st->roll);    row_set(pg, 4, b);
        snprintf(b, sizeof(b), "%+.1f dps", (double)st->pitch);   row_set(pg, 5, b);
        snprintf(b, sizeof(b), "%+.1f dps", (double)st->yaw);     row_set(pg, 6, b);
        snprintf(b, sizeof(b), "%.1f \xC2\xB0" "C", (double)st->imu_temp); row_set(pg, 7, b);
        row_set(pg, 8, r);
        for (int i = 0; i < 3; i++) {
            err_fmt(pg, i, 3, acc_err(st, i), "g");
            err_fmt(pg, 4 + i, 1, gyro_err(st, i), "dps");
        }
        err_fmt(pg, 3, 3, accmag_err(st), "g");
        err_set(pg, 7, "untrimmed - no accuracy spec");
        break;
    }
    case PG_BARO: {
        char r[16];
        strcpy(r, b);
        snprintf(b, sizeof(b), "%.2f \xC2\xB0" "C", (double)st->temperature); row_set(pg, 0, b);
        snprintf(b, sizeof(b), "%.3f kPa", (double)st->pressure);  row_set(pg, 1, b);
        snprintf(b, sizeof(b), "%.1f %%", (double)st->humidity);   row_set(pg, 2, b);
        snprintf(b, sizeof(b), "%+.2f m", (double)st->altitude);   row_set(pg, 3, b);
        snprintf(b, sizeof(b), "%+.2f m/s", (double)st->vertical_velocity); row_set(pg, 4, b);
        row_set(pg, 5, r);
        err_fmt(pg, 0, 1, BME_T, "\xC2\xB0" "C");
        err_fmt(pg, 1, 2, BME_P_ABS, "kPa");
        err_fmt(pg, 2, 0, BME_H, "%");
        err_fmt(pg, 3, 2, alt_err(st), "m");
        err_fmt(pg, 4, 2, vspeed_err(st), "m/s  (noise)");
        break;
    }
    case PG_GAS: {
        char r[16];
        strcpy(r, b);
        snprintf(b, sizeof(b), "%.2f kOhm", (double)(st->gas_resistance / 1000.0f)); row_set(pg, 0, b);
        row_set(pg, 1, st->mode == TELEM_MODE_BOOST ? "paused in BOOST" : "320 \xC2\xB0" "C pulse, every 10 s");
        row_set(pg, 2, r);
        err_fmt(pg, 0, 2, BME_GAS_S2S * st->gas_resistance / 1000.0f, "kOhm  (sensor-to-sensor)");
        break;
    }
    case PG_POWER: {
        char r[16];
        strcpy(r, b);
        snprintf(b, sizeof(b), "%.3f V", (double)st->bus_voltage); row_set(pg, 0, b);
        snprintf(b, sizeof(b), "%.1f mA", (double)st->current);    row_set(pg, 1, b);
        snprintf(b, sizeof(b), "%.3f W", (double)st->power);       row_set(pg, 2, b);
        snprintf(b, sizeof(b), st->energy_wh < 10.0 ? "%.3f Wh" : "%.2f Wh", st->energy_wh);
        row_set(pg, 3, b);
        row_set(pg, 4, r);
        float pe = ina_p_err(st);
        err_fmt(pg, 0, 3, ina_v_err(st), "V");
        err_fmt(pg, 1, 1, ina_i_err(st), "mA");
        err_fmt(pg, 2, 3, pe, "W");
        err_fmt(pg, 3, 4, (st->power > 0.0f) ? (float)st->energy_wh * pe / st->power : NAN, "Wh");
        break;
    }
    case PG_GPS: {
        char r[16];
        strcpy(r, b);
        row_set(pg, 0, st->gps_fix ? "YES" : "NO  (searching)");
        snprintf(b, sizeof(b), "%u", (unsigned)st->sat_count);    row_set(pg, 1, b);
        if (st->gps_fix) {
            snprintf(b, sizeof(b), "%.6f", st->gps_lat);           row_set(pg, 2, b);
            snprintf(b, sizeof(b), "%.6f", st->gps_lon);           row_set(pg, 3, b);
            snprintf(b, sizeof(b), "%.1f m", (double)st->gps_alt); row_set(pg, 4, b);
            snprintf(b, sizeof(b), "%.2f m/s", (double)st->horizontal_velocity); row_set(pg, 5, b);
            err_set(pg, 2, "+/- 1.5 m  (CEP 50 %)");
            err_set(pg, 3, "+/- 1.5 m  (CEP 50 %)");
            err_set(pg, 4, "not specified in datasheet");
            err_fmt(pg, 5, 2, GPS_VEL_MPS, "m/s");
        } else {
            for (int i = 2; i <= 5; i++) err_set(pg, i, "");
            /* a plausible-looking zero is worse than an obvious placeholder */
            row_set(pg, 2, "--"); row_set(pg, 3, "--"); row_set(pg, 4, "--"); row_set(pg, 5, "--");
        }
        row_set(pg, 6, r);
        break;
    }
    case PG_LINK: {
        fmt_rate(b, sizeof(b), m->pkt_rate, "");                    row_set(pg, 0, b);
        snprintf(b, sizeof(b), "%.0f", (double)m->frame_rate);     row_set(pg, 1, b);
        snprintf(b, sizeof(b), "%lu", (unsigned long)st->frames_crc_fail); row_set(pg, 2, b);
        if (m->age_ms == UINT32_MAX) snprintf(b, sizeof(b), "never");
        else snprintf(b, sizeof(b), "%lu ms ago", (unsigned long)m->age_ms);
        row_set(pg, 3, b);
        row_set(pg, 4, "6  (2437 MHz)");
        row_set(pg, 5, st->have_calib ? "received" : "waiting");
        if (m->sd.active) {
            char mb[16];
            fmt_mb(mb, sizeof(mb), m->sd.bytes);
            snprintf(b, sizeof(b), "%s  %s", m->sd.name, mb);
        } else {
            snprintf(b, sizeof(b), "no card");
        }
        row_set(pg, 6, b);
        snprintf(b, sizeof(b), "%lu B", (unsigned long)m->sd.dropped); row_set(pg, 7, b);
        break;
    }
    default: break;
    }

    for (int i = 0; i < p->ncharts; i++) {
        if (p->chart[i].scatter) scatter_autorange(&p->chart[i]);
        else                     chart_autorange(&p->chart[i]);
    }
}

/* Feed every graph with whatever is new - on all pages, visible or not. */
static void feed_charts(const ui_model_t *m)
{
    static uint32_t last[TELEM_SENSOR_COUNT];
    static bool     have_origin;
    static double   lat0, lon0;
    const telem_state_t *st = &m->st;

#define NEW(i) (st->sensor[i].frames != last[i])

    if (NEW(TELEM_SENSOR_IMU)) {
        chart_t *a = &s_page[PG_IMU].chart[0], *g = &s_page[PG_IMU].chart[1];
        chart_push(a, 0, st->accel_x); chart_push(a, 1, st->accel_y); chart_push(a, 2, st->accel_z);
        chart_push(g, 0, st->roll);    chart_push(g, 1, st->pitch);   chart_push(g, 2, st->yaw);
    }
    if (NEW(TELEM_SENSOR_BARO) && st->have_calib) {
        chart_push(&s_page[PG_BARO].chart[0], 0, st->altitude);
        chart_push(&s_page[PG_BARO].chart[1], 0, st->temperature);
    }
    if (NEW(TELEM_SENSOR_GAS) && st->have_calib) {
        chart_push(&s_page[PG_GAS].chart[0], 0, st->gas_resistance / 1000.0f);
    }
    if (NEW(TELEM_SENSOR_POWER)) {
        chart_push(&s_page[PG_POWER].chart[0], 0, st->power);
        chart_push(&s_page[PG_POWER].chart[1], 0, st->current);
    }
    if (NEW(TELEM_SENSOR_GPS)) {
        chart_push(&s_page[PG_GPS].chart[1], 0, st->sat_count);
        if (st->gps_fix) {
            if (!have_origin) {
                lat0 = st->gps_lat;
                lon0 = st->gps_lon;
                have_origin = true;
            }
            /* local east/north in metres - small-area flat-earth approximation */
            float north = (float)((st->gps_lat - lat0) * 111320.0);
            float east  = (float)((st->gps_lon - lon0) * 111320.0 * cos(lat0 * M_PI / 180.0));
            chart_t *c = &s_page[PG_GPS].chart[0];
            lv_chart_set_next_value2(c->obj, c->ser[0], (int32_t)lroundf(east * c->scale),
                                     (int32_t)lroundf(north * c->scale));
        }
    }
    if (m->tick_1hz) {
        chart_push(&s_page[PG_LINK].chart[0], 0, m->pkt_rate);
        chart_push(&s_page[PG_LINK].chart[1], 0, m->frame_rate);
    }
#undef NEW

    for (int s = 0; s < TELEM_SENSOR_COUNT; s++) last[s] = st->sensor[s].frames;
}

void ui_update(const ui_model_t *m)
{
    feed_charts(m);
    update_header(m);
    if (s_current < 0) update_overview(m);
    else               update_page(m, s_current);
}
