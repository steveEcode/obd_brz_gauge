#pragma once
// ================================================================
//  ui_disp_item.h — configurable data-item system (extracted from ui.c)
//
//  Data-item metadata and utilities shared by the Temp / Info / Needle / Chart pages:
//  name/unit/color, raw value reading, sweep animation values, text formatting,
//  alarm coloring, natural ranges.
//
//  Raw value conventions: temperature °C, percentage %, RPM rpm, speed km/h, voltage mV,
//              oil pressure / boost pressure / brake temp are all ×10 integers.
//  Natural range: nmin/nmax are in natural units, div satisfies raw = natural × div.
// ================================================================

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISP_ITEM_CLT = 0,
    DISP_ITEM_IAT,
    DISP_ITEM_OIL,
    DISP_ITEM_LOAD,
    DISP_ITEM_TPS,
    DISP_ITEM_RPM,
    DISP_ITEM_SPEED,
    DISP_ITEM_BAT,
    DISP_ITEM_OILP,
    DISP_ITEM_BKT,
    DISP_ITEM_BOOST,
    DISP_ITEM_AFR,
    DISP_ITEM_COUNT
} disp_item_t;

// Invalid-sample sentinel for the generic chart page (distinct from legit negative values like coolant temp -10)
#define CHART_INVALID (-32768)

typedef struct {
    int32_t nmin;
    int32_t nmax;
    int32_t div;
} needle_scale_meta_t;

typedef struct {
    const char *name;
    const char *unit;
    uint32_t color;
} disp_item_meta_t;

// Global metadata tables (ui.c page-refresh logic indexes them directly)
extern const disp_item_meta_t s_disp_meta[DISP_ITEM_COUNT];
extern const needle_scale_meta_t s_needle_scale_meta[DISP_ITEM_COUNT];

// Read the given data item from the OBD cache raw values; returns true and writes *out on success
bool disp_item_read_value(disp_item_t item,
                          int16_t clt, int16_t iat, int16_t oil,
                          int16_t load_pct, int16_t tps, int32_t bat_mv,
                          int16_t oilp_x10, int16_t brake_x10,
                          uint16_t rpm, uint16_t speed, int16_t boost_x10,
                          int16_t afr_x100,
                          int32_t *out);

// Sweep animation value: r ∈ [0,1] → sweep peak raw value for this data item
int32_t disp_item_sweep_value(disp_item_t item, float r);

// Format the numeric text per data item into label (includes unit conversion and dirty-check update)
void disp_item_set_text(lv_obj_t *label, disp_item_t item, int32_t value, bool valid);

// Color per NVS alarm threshold (nvs_chart_alarm_get): red above threshold, else white
void disp_item_set_value_color(lv_obj_t *label, disp_item_t item, int32_t value, bool valid);

// All-in-one update: adaptive-step smoothing + text formatting + alarm coloring
// *state holds the currently displayed value; when valid=false the last valid value is kept
void disp_item_update(int32_t *state, lv_obj_t *label, disp_item_t item,
                      int32_t raw, bool valid, int32_t threshold);

// Accessors (out-of-range falls back to CLT; name returns "" when out of range)
const char *ui_disp_item_name(uint8_t item);
const char *ui_disp_item_unit(uint8_t item);
uint32_t ui_disp_item_color(uint8_t item);
void ui_disp_item_range(uint8_t item, int32_t *nmin, int32_t *nmax, int32_t *div);

// Get the natural-range metadata directly (item must be < DISP_ITEM_COUNT)
const needle_scale_meta_t *ui_disp_item_scale(disp_item_t item);

#ifdef __cplusplus
}
#endif
