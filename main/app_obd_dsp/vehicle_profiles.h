#pragma once

#include <stdint.h>
#include "obd_data_cache.h"
#include "vehicle_custom_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define VEHICLE_MAX_GEARS 9   // accommodates index0 + up to 8 forward gears (e.g. ZF 8HP)

// Oil temp query mode priority
typedef enum {
    OIL_TEMP_MODE_NONE = 0xFF,
    OIL_TEMP_MODE_PID_5C = 0,       // Standard PID 01 5C
    OIL_TEMP_MODE_UDS_22_10_17 = 1, // UDS Mode 22 10 17
    OIL_TEMP_MODE_TOYOTA_21_01 = 2, // Toyota Mode 21 01
    OIL_TEMP_MODE_MAZDA_22_111F = 3, // Mazda Skyactiv Mode 22 PID 111F, single byte A-50
    OIL_TEMP_MODE_MAZDA_22_1310 = 4, // Mazda Skyactiv Mode 22 PID 1310, two bytes (A*256+B)/100-40
    OIL_TEMP_MODE_PORSCHE_CAN_441 = 5, // Porsche 997.2/987.2 CAN broadcast frame 0x441 (byte5 oil temp x-60, byte6 oil pressure)
    OIL_TEMP_MODE_MINI_22_5822 = 6,    // MINI/BMW Mode 22 PID 5822, single byte °C = A-60 (°F=A*9/5-76)
    OIL_TEMP_MODE_BMW_22_4402 = 7,     // BMW F-series (F48 etc., B38/B48) Mode 22 PID 4402, second byte °C = B-64
    OIL_TEMP_MODE_BMW_22_03F3 = 8,     // BMW G-series Mode 22 PID 03F3, single byte °C = A-40
    OIL_TEMP_MODE_BMW_G_22_4402 = 9,   // BMW G-series Mode 22 PID 4402, two bytes °C = (A*256+B)*191.25/255-48
    OIL_TEMP_MODE_BMW_22_D002 = 10,    // BMW G-series Mode 22 PID D002, two bytes °C = (A*256+B)*191.25/255-48
    OIL_TEMP_MODE_BMW_22_111F = 11,    // BMW Mode 22 PID 111F (Header 7E0), single byte °C = A-50
} oil_temp_query_mode_t;

// Vehicle gear ratio ranges (used for gear detection)
typedef struct {
    float min_ratio;
    float max_ratio;
    enGear gear;
} gear_ratio_range_t;

// Oil temp query strategy
typedef struct {
    oil_temp_query_mode_t primary;     // preferred query mode
    oil_temp_query_mode_t secondary;   // backup 1
    oil_temp_query_mode_t tertiary;    // backup 2
    oil_temp_query_mode_t quaternary;  // backup 3 (final fallback)
    // CAN-441 (Porsche) oil temp linear formula: °C = raw * can_num / can_den + can_off
    // Only used in OIL_TEMP_MODE_PORSCHE_CAN_441 mode; when can_den=0, falls back to the built-in default (x-60).
    // Different generations only need to change these three coefficients: 997.2/987.2 = 1/1/-60; 997.1/987.1 = 3/4/-48.
    int16_t can_num;
    int16_t can_den;
    int16_t can_off;
} oil_temp_strategy_t;

// Vehicle parameter configuration
typedef struct {
    const char *name;                    // display name (e.g. "BRZ ZC6")
    float final_drive_ratio;             // final drive ratio
    float tire_rolling_radius_m;         // tire rolling radius (m)
    uint8_t gear_count;                  // number of forward gears (5 or 6)
    float gear_ratios[VEHICLE_MAX_GEARS]; // per-gear ratios, index 0 unused, 1~gear_count valid
    float gear_tolerance;                // gear detection tolerance (e.g. 0.15 = ±15%)
    oil_temp_strategy_t oil_temp_strategy; // oil temp query strategy
    bool has_boost;                      // whether turbocharged (decides whether to query/display boost pressure)
    uint8_t forced_protocol;             // forced ELM327 protocol number (ATSP), 0=auto-detect; lock to 6 for cars like BMW where auto-detect is unstable
    bool obd_functional_addr;            // true=standard PIDs use functional addressing (ATSH 7DF, same as phone apps); false=physical addressing (ATSH 7E0, Subaru etc.)
    float speed_scale;                   // speed correction factor (read value × this factor), 0 or unset = 1.0 (no correction)
    uint8_t obd_timeout;                 // ATST timeout value (ELM327 units, 0=default 0x19); for BMW G CAN fast responses, set 0x0F to reduce NO DATA waits
    uint8_t poll_gap_ms;                 // poll slot interval (ms), 0=use the default OBD_POLL_SLOT_GAP_MS(30ms)
    bool can_broadcast_mode;             // true=read data by listening to CAN broadcast frames via ATMA (FT86 0x140/360/D1/141), replacing standard OBD PID polling
} vehicle_profile_t;

// Get all predefined vehicle profiles
const vehicle_profile_t *vehicle_profile_get_all(uint8_t *count);

// Get the vehicle profile at the given index
const vehicle_profile_t *vehicle_profile_get(uint8_t index);

// Get the currently active vehicle profile
const vehicle_profile_t *vehicle_profile_get_active(void);

// Set the active vehicle profile (also saved to NVS)
void vehicle_profile_set_active(uint8_t index);

// Calculate the speed constant: 1 / (final_drive * 0.377 * tire_radius)
float vehicle_profile_calc_constant(const vehicle_profile_t *p);

// Generate the gear range array from the currently active vehicle profile
// Returns the range array pointer; count outputs the number of valid elements
const gear_ratio_range_t *vehicle_profile_get_gear_ranges(uint8_t *count);

// Get the oil temp query strategy of the currently active vehicle
const oil_temp_strategy_t *vehicle_profile_get_oil_temp_strategy(void);

// Get the override configuration of the currently active vehicle (NULL = pure OBD2 standard)
const vehicle_override_t *vehicle_profile_get_override(void);

#ifdef __cplusplus
}
#endif
