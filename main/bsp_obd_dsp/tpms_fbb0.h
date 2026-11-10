#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TireKing FBB0 broadcast path. Offsets apply to a complete scan record,
 * never an extracted manufacturer payload or concatenated partial streams. */
typedef struct {
    char sensor_id[7];                 /* 24-bit ID, uppercase HEX including zeros */
    uint32_t pressure_pa;              /* exact fixed-point: code * 3144 */
    int16_t temperature_c;
    uint16_t voltage_mv;
    uint8_t state, version, ble_version_code;
} tpms_fbb0_data_t;

bool tpms_fbb0_decode(const uint8_t *raw, size_t len, tpms_fbb0_data_t *out);
bool tpms_fbb0_id_valid(const char *id);
