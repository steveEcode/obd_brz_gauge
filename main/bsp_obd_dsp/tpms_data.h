#pragma once
#include "tpms_fbb0.h"

#define TPMS_WHEEL_COUNT 4u
/* Freshness is a display timeout, not a guaranteed sensor reporting period. */
#define TPMS_FRESH_MS 120000u

typedef struct {
    tpms_fbb0_data_t data;
    uint8_t mac[6];                    /* diagnostic only, never binding identity */
    uint8_t addr_type;
    int rssi;
} tpms_scan_result_t;

typedef struct {
    char sensor_id[7];
    bool bound, received, fresh;
    uint32_t age_ms;
    tpms_scan_result_t sample;
} tpms_wheel_data_t;
