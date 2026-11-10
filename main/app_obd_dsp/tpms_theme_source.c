#include "tpms_theme_source.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *const positions[] = {"fl", "fr", "rl", "rr"};
static const char *const fields[TPMS_THEME_FIELDS] = {
    "pressure_pa", "pressure_kpa", "pressure_bar", "temperature_c", "voltage_v",
    "state", "version", "ble_version", "rssi", "age_s", "bound", "received",
    "online", "sensor_id", "addr_type", "battery_percent"
};

int tpms_theme_source_index(const char *key)
{
    if (!key || strncmp(key, "tpms.", 5)) return -1;
    for (unsigned w = 0; w < NVS_TPMS_WHEELS; ++w) {
        if (strncmp(key + 5, positions[w], 2) || key[7] != '.') continue;
        for (unsigned f = 0; f < TPMS_THEME_FIELDS; ++f)
            if (!strcmp(key + 8, fields[f])) return (int)(w * TPMS_THEME_FIELDS + f);
    }
    return -1;
}

void tpms_theme_values(const tpms_wheel_data_t wheels[NVS_TPMS_WHEELS], double out[TPMS_THEME_COUNT])
{
    for (unsigned w = 0; w < NVS_TPMS_WHEELS; ++w) {
        const tpms_wheel_data_t *v = &wheels[w];
        const tpms_fbb0_data_t *d = &v->sample.data;
        double *p = out + w * TPMS_THEME_FIELDS;
        for (unsigned f = 0; f < TPMS_THEME_FIELDS; ++f) p[f] = NAN;
        p[10] = v->bound; p[11] = v->received; p[12] = v->fresh;
        if (v->bound) p[13] = (double)strtoul(v->sensor_id, NULL, 16);
        if (v->received) p[9] = v->age_ms / 1000.0;
        if (!v->fresh) continue;
        p[0] = d->pressure_pa; p[1] = d->pressure_pa / 1000.0;
        p[2] = d->pressure_pa / 100000.0; p[3] = d->temperature_c;
        p[4] = d->voltage_mv / 1000.0; p[5] = d->state; p[6] = d->version;
        p[7] = d->ble_version_code / 10.0; p[8] = v->sample.rssi;
        p[14] = v->sample.addr_type; p[15] = tpms_battery_percent(d->voltage_mv);
    }
}

const char *tpms_state_name(uint8_t state)
{
    static const char *const names[] = {"NORMAL", "LEAK", "INFLATE", "FIRST", "POWER ON", "PERIODIC"};
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "UNKNOWN";
}

uint8_t tpms_battery_percent(uint16_t voltage_mv)
{
    /* APK stores voltage as float32 before strict double comparisons. */
    double voltage = (double)(float)(voltage_mv / 1000.0);
    if (voltage > 2.8) return 100;
    if (voltage > 2.6) return 75;
    if (voltage > 2.4) return 50;
    if (voltage > 2.2) return 25;
    return 0;
}
