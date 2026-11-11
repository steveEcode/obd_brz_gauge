#pragma once
#include "bsp_obd_dsp/tpms_ble_client.h"

/* Theme namespace, not an OBD PID, registry wire ID or NVS slot. ESP-NOW
 * forwards raw subscribed wheel snapshots; all derived fields stay identical. */
#define TPMS_THEME_FIELDS 16
#define TPMS_THEME_COUNT (NVS_TPMS_WHEELS * TPMS_THEME_FIELDS)
int tpms_theme_source_index(const char *key);
/* One four-wheel snapshot for every binding and rule in a theme frame.
 * Unavailable/stale measurements become NAN; status/age remain meaningful. */
void tpms_theme_values(const tpms_wheel_data_t wheels[NVS_TPMS_WHEELS],
                       double values[TPMS_THEME_COUNT]);
const char *tpms_state_name(uint8_t state);

/* TireKing voltage-bin estimate, not a measured state of charge. */
uint8_t tpms_battery_percent(uint16_t voltage_mv);
