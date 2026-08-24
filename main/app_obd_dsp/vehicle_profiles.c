#include "vehicle_profiles.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include <string.h>

#define TAG "vehicle_profile"

// Predefined vehicle profiles
static const vehicle_profile_t s_profiles[] = {
    {
        // Generic OBD2 standard profile (SAE J1979 / ISO 15031-5)
        // Uses only standard PIDs, no manufacturer-specific protocols:
        //   RPM 010C, Speed 010D, Coolant 0105, Oil Temp 015C, Intake 010F, Load 0104, TPS 0111, Voltage 0142
        // Gear ratios are common 6MT placeholders (only affects gear detection accuracy, not data reading).
        .name = "OBD2 Generic",
        .final_drive_ratio = 3.500f,       // Generic placeholder
        .tire_rolling_radius_m = 0.315f,   // Common for 205/55R16
        .gear_count = 6,
        .gear_ratios = {0, 3.500f, 2.000f, 1.400f, 1.100f, 0.900f, 0.750f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // Standard OBD2 oil temp PID (°C = A - 40)
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = false,                // Generic defaults to NA; turbo cars can still use standard 010B (manual enable)
    },
    {
        // BRZ ZC6 Gen1 (2013-2020, FA20 NA, Gen1)
        // RPM stays on OBD; TPS/coolant/oil come from CAN broadcast frames.
        // For the explicit OBD-only fallback version, see "ZN/C6 PID" right below
        .name = "ZN/C6 CAN",
        .final_drive_ratio = 4.100f,
        .tire_rolling_radius_m = 0.314f,   // 215/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.188f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_TOYOTA_21_01,
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,
        .can_broadcast_mode = true,
        .obd_timeout = 0x0A,
        .poll_gap_ms = 1,
    },
    {
        // BRZ ZC6 / GT86 ZN6 standard PID fallback version (no ATMA CAN monitoring)
        // Some cheap clone ELM327 adapters do not support ATMA monitoring, or the car's bus does not emit the 0x140/0x360 frames,
        // so "ZN/C6 CAN" would get no RPM/oil temp/coolant temp. In this version RPM/coolant temp go through standard OBD PIDs (01 0C / 01 05),
        // oil temp goes through Toyota Mode 21 01 (same as the old config before the CAN version was introduced), trading the 100Hz RPM refresh rate for stability.
        // If ATMA monitoring works fine, prefer "ZN/C6 CAN" above.
        .name = "ZN/C6 PID",
        .final_drive_ratio = 4.100f,
        .tire_rolling_radius_m = 0.314f,   // 215/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.188f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_TOYOTA_21_01,  // FA20 does not support 01 5C; oil temp fixed to Mode 21 01
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,
        .obd_timeout = 0x0A,
        .poll_gap_ms = 1,
    },
    {
        // BRZ ZD8 OBD-only fallback (2022+, FA24 NA, Gen2, 6MT)
        // CAN monitoring is disabled here to keep the ELM327 loop single-threaded.
        // Remaining channels use standard OBD PID.
        .name = "ZD8 OBD",
        .final_drive_ratio = 4.100f,       // ZD8 6MT final drive ratio
        .tire_rolling_radius_m = 0.318f,   // 225/40R18
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.189f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // ZD8 uses standard PID 5C
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,
        .obd_timeout = 0x0A,
        .poll_gap_ms = 1,
    },
    {
        // BRZ ZD8 standard OBD fallback (6MT).
        // This variant routes RPM/coolant temp/oil temp all through standard OBD PIDs (01 0C / 01 05 / 01 5C), trading refresh rate for stability.
        .name = "ZD8",
        .final_drive_ratio = 4.100f,
        .tire_rolling_radius_m = 0.318f,   // 225/40R18
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.189f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // ZD8 supports standard PID 5C
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,
        .obd_timeout = 0x0A,
        .poll_gap_ms = 1,
    },
    {
        .name = "MX-5 ND",
        .final_drive_ratio = 2.866f,       // ND 6MT (all manuals identical; auto is 3.583)
        .tire_rolling_radius_m = 0.300f,   // 195/50R16
        .gear_count = 6,
        .gear_ratios = {0, 5.087f, 2.991f, 2.035f, 1.594f, 1.286f, 1.000f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // Try PID 1310 (double byte) first, fallback to 111F (single byte) on consecutive failures
            .primary = OIL_TEMP_MODE_MAZDA_22_1310,
            .secondary = OIL_TEMP_MODE_MAZDA_22_111F,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        // .has_boost defaults to false (NA)
        .obd_timeout = 0x0A,  // 40ms timeout; Mazda CAN typically responds in 5-15ms, reduces NO DATA waits
        .poll_gap_ms = 1,     // Min slot interval 1ms (0=skip vTaskDelay, 1ms lets scheduler switch tasks)
    },
    {
        // BMW G-series (G20/G21/G22, B48/B58 turbo, ZF 8HP)
        // OBD-only fallback; CAN monitoring is disabled to avoid interleaving with OBD requests on the single-threaded ELM327 loop.
        // Standard OBD only responds to 7DF functional addressing, 7E0 physical gets no response; hence obd_functional_addr=true.
        // Oil temp: try PID 4402 (double byte) first, then PID D002 (oil pan backup), finally fallback to PID 03F3.
        .name = "BMW F/G",
        .final_drive_ratio = 2.813f,       // G20 330i final drive ratio
        .tire_rolling_radius_m = 0.330f,   // 225/45R18
        .gear_count = 8,                   // ZF 8HP 8-speed
        .gear_ratios = {0, 5.250f, 3.360f, 2.172f, 1.720f, 1.316f, 1.000f, 0.822f, 0.640f},  // ZF 8HP75
        .gear_tolerance = 0.09f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // Standard OBD2 PID 01 5C (reliable via OBDII)
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,
        .forced_protocol = 6,
        .obd_functional_addr = true,
        .obd_timeout = 0x0F,
    },
    {
        // BMW G-series compatibility profile (G20/G21/G22/G80/G82, B48/B58 turbo, ZF 8HP)
        // CAN broadcast is disabled; use the same OBD-only request path as BMW F/G to keep the ELM327 loop serial.
        .name = "BMW G OBD",
        .final_drive_ratio = 2.813f,       // G20 330i final drive ratio
        .tire_rolling_radius_m = 0.330f,   // 225/45R18
        .gear_count = 8,                   // ZF 8HP 8-speed
        .gear_ratios = {0, 5.250f, 3.360f, 2.172f, 1.720f, 1.316f, 1.000f, 0.822f, 0.640f},  // ZF 8HP75
        .gear_tolerance = 0.09f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // CAN 0x3F9 provides oil temp directly; OBD 01 5C as fallback
            .secondary = OIL_TEMP_MODE_BMW_22_4402, // Mode 22 PID 4402 backup
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,                 // B48/B58 turbo
        .forced_protocol = 7,              // PT-CAN auto-detect is unstable; lock to protocol 7
        .obd_functional_addr = true,       // 7DF functional addressing
        .obd_timeout = 0x0A,               // 40ms; BMW CAN responds quickly
        .poll_gap_ms = 1,
    },
    {
        // BMW E-series (E9x M3, S65B40 NA V8; also usable as a base for E46/E39/E60/E9x non-M)
        // OBD-only; standard mode 01 PIDs go through 7DF functional addressing (the E-series DME
        // does not answer physical 7E0 for mode 01, same behaviour as BMW F/G above).
        // RPM/speed/coolant/intake/load/TPS all standard; S65 is NA so no boost.
        // Oil temp: S65 does NOT support standard 01 5C. Car Scanner reads it via proprietary
        //   mode 21 (KWP local IDs 01/04/05/07/08/0B @6F1) and mode 22 (DID 448/58F0/58F3 @6F1,
        //   DID F5xx @7DF). Those formulas are not reverse-engineered yet, so we fall back to 01 5C.
        .name = "BMW E",
        .final_drive_ratio = 3.846f,       // E92 M3 6MT final drive (M-DCT is 3.154)
        .tire_rolling_radius_m = 0.335f,   // rear 265/40R18
        .gear_count = 6,
        .gear_ratios = {0, 4.055f, 2.396f, 1.582f, 1.192f, 1.000f, 0.872f},  // Getrag GS6-53BZ 6MT
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // fallback only; S65 has no standard 5C (see note above)
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,              // ISO 15765-4 CAN 11-bit 500k
        .obd_functional_addr = true,       // 7DF functional addressing (same as phone apps)
        .obd_timeout = 0x0A,
        .poll_gap_ms = 1,
    },
    {
        // MINI John Cooper Works F56 (BMW B48 2.0T, FWD transverse)
        .name = "JCW F56",
        .final_drive_ratio = 3.824f,       // F56 JCW 6MT final drive ratio
        .tire_rolling_radius_m = 0.308f,   // Front wheels (FWD drive wheels) 205/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.923f, 2.136f, 1.276f, 0.921f, 0.756f, 0.628f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // Boost pressure uses standard PID 010B (has_boost), no extra adaptation needed.
            // Oil temp: MINI/BMW enhanced Mode 22 PID 5822, °C = A-60 (community verified, same monitor as N18/N16/B48);
            // Fallback to standard 01 5C if unavailable.
            .primary = OIL_TEMP_MODE_MINI_22_5822,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,                 // B48 turbo, boost pressure via standard 010B
    },
    {
        // Porsche Gen2: 987.2/997.2 (2009-2012, DFI 9A1; NA; OBD fallback)
        .name = "POS 997.2",
        .final_drive_ratio = 3.44f,        // 997.2 PDK final drive ratio (user measured)
        .tire_rolling_radius_m = 0.325f,   // User measured rolling radius
        .gear_count = 7,                   // PDK 7-speed
        .gear_ratios = {0, 3.91f, 2.29f, 1.65f, 1.30f, 1.08f, 0.88f, 0.62f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // CAN oil-temp paths are disabled here; use the standard OBD PID 01 5C fallback.
            .primary = OIL_TEMP_MODE_PID_5C,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = false,                // Naturally aspirated
        .forced_protocol = 6,
    },
    {
        // Porsche Gen1: 987.1/997.1 (2005-2008, M96/M97; OBD fallback)
        // Note: Gear ratios use Gen2 placeholders (987.1 differs slightly).
        .name = "POS 997.1",
        .final_drive_ratio = 3.89f,
        .tire_rolling_radius_m = 0.335f,
        .gear_count = 6,
        .gear_ratios = {0, 3.67f, 2.05f, 1.46f, 1.13f, 0.97f, 0.84f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // CAN oil-temp paths are disabled here; use the standard OBD PID 01 5C fallback.
            .primary = OIL_TEMP_MODE_PID_5C,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = false,
        .forced_protocol = 6,
    },
    {
        // Alfa Romeo Giulia 2.0T (GME 2.0 turbo + ZF 8HP50 8AT, RWD)
        // Standard OBD (RPM/speed/coolant temp/intake temp/load/TPS/voltage/MAP) goes through functional addressing 7DF; oil temp 01 5C is not supported,
        // use FCA UDS extended addressing ATSH18DA10F1 + 22 13 02 instead (see the override in vehicle_custom_config.h).
        // MY2018+ SGW only blocks write operations (code clearing/matching); read-only live data needs no bypass.
        // If 22 13 02 gets no response, change forced_protocol to 7 (29bit) and retry.
        .name = "GIULIA 2.0T",
        .final_drive_ratio = 2.35f,        // RWD standard final drive (Q4 AWD is 2.65, adjust to the actual car)
        .tire_rolling_radius_m = 0.330f,   // 225/45R18, adjust to the actual tires
        .gear_count = 8,                   // ZF 8HP50
        .gear_ratios = {0, 5.000f, 3.200f, 2.143f, 1.720f, 1.313f, 1.000f, 0.823f, 0.640f},
        .gear_tolerance = 0.09f,
        .oil_temp_strategy = {
            // Oil temp is handled uniformly by the override formula (22 13 02 @18DA10F1), not the enum strategy
            .primary = OIL_TEMP_MODE_NONE,
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,                 // boost via standard 01 0B (MAP absolute pressure)
        .forced_protocol = 6,              // CAN 11bit 500k
        .obd_functional_addr = true,       // standard PIDs use the 7DF broadcast (same as phone apps)
        .obd_timeout = 0x0F,
        .poll_gap_ms = 50,                 // extended UDS responses are a bit slow, keep it conservative
    },
    {
        // Jeep (generic placeholder; refine gear ratios/tire size once the exact model/engine/transmission is known)
        // Confirmed via probe: standard mode 01 PIDs (RPM/speed/coolant/intake/load/TPS/voltage/MAP) all respond
        // over 7DF functional addressing. Oil temp 01 5C is a REAL sensor here (r=1.0, not just a table lookup),
        // so no custom override/formula is needed — the default OIL_TEMP_MODE_PID_5C strategy just works.
        // Still unresolved in the probe: oem-mode01 (0169/016A/016C/016E/016F/01BD...), proprietary mode 22 F5xx
        // series, and a 29-bit extended header (DA18F1, likely TCU) with mode 22 DIDs 1D07/1D08/1D09/1D12 — none
        // mapped to a gauge yet, so no CAN rules / secondary oil formula added.
        .name = "jeep",
        .final_drive_ratio = 3.45f,        // Generic placeholder (Wrangler JL Pentastar ballpark)
        .tire_rolling_radius_m = 0.373f,   // Generic placeholder for 245/75R17
        .gear_count = 8,                   // Generic placeholder (8HP75-class 8-speed auto)
        .gear_ratios = {0, 4.710f, 3.140f, 2.100f, 1.670f, 1.290f, 1.000f, 0.840f, 0.670f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // confirmed real sensor via probe (r=1.0)
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = false,                // set true if the specific engine is turbocharged (e.g. 2.0L Hurricane)
        .forced_protocol = 6,              // ISO 15765-4 CAN 11-bit 500k
        .obd_functional_addr = true,       // 7DF functional addressing confirmed by probe
        .obd_timeout = 0x0F,
    },
};

#define PROFILE_COUNT (sizeof(s_profiles) / sizeof(s_profiles[0]))

// Cached gear ranges for the active profile
static gear_ratio_range_t s_gear_ranges[VEHICLE_MAX_GEARS];
static uint8_t s_gear_range_count = 0;
static uint8_t s_active_idx = 0;
static bool s_ranges_dirty = true;

// Recalculate gear ratio ranges from profile
static void rebuild_gear_ranges(const vehicle_profile_t *p)
{
    s_gear_range_count = 0;
    for (uint8_t i = 1; i <= p->gear_count && i < VEHICLE_MAX_GEARS; i++) {
        float total = p->gear_ratios[i] * p->final_drive_ratio;
        s_gear_ranges[s_gear_range_count].min_ratio = total * (1.0f - p->gear_tolerance);
        s_gear_ranges[s_gear_range_count].max_ratio = total * (1.0f + p->gear_tolerance);
        s_gear_ranges[s_gear_range_count].gear = (enGear)i; // GEAR_1=1, GEAR_2=2, ...
        s_gear_range_count++;
    }
    s_ranges_dirty = false;
    ESP_LOGD(TAG, "Rebuilt gear ranges for '%s' (%d gears)", p->name, p->gear_count);
}

const vehicle_profile_t *vehicle_profile_get_all(uint8_t *count)
{
    if (count) *count = (uint8_t)PROFILE_COUNT;
    return s_profiles;
}

const vehicle_profile_t *vehicle_profile_get(uint8_t index)
{
    if (index >= PROFILE_COUNT) return &s_profiles[0];
    return &s_profiles[index];
}

const vehicle_profile_t *vehicle_profile_get_active(void)
{
    return vehicle_profile_get(s_active_idx);
}

void vehicle_profile_set_active(uint8_t index)
{
    if (index >= PROFILE_COUNT) index = 0;
    s_active_idx = index;
    s_ranges_dirty = true;
    obd_data_reset_temp_cache();

    // Save to NVS
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.vehicle_profile_idx = index;
    nvs_cfg_set(&cfg);

    ESP_LOGD(TAG, "Active profile set to [%d] '%s'", index, s_profiles[index].name);
}

float vehicle_profile_calc_constant(const vehicle_profile_t *p)
{
    if (!p) return 0;
    float denom = p->final_drive_ratio * 0.377f * p->tire_rolling_radius_m;
    if (denom == 0) return 0;
    return 1.0f / denom;
}

const gear_ratio_range_t *vehicle_profile_get_gear_ranges(uint8_t *count)
{
    if (s_ranges_dirty) {
        rebuild_gear_ranges(vehicle_profile_get_active());
    }
    if (count) *count = s_gear_range_count;
    return s_gear_ranges;
}

const oil_temp_strategy_t *vehicle_profile_get_oil_temp_strategy(void)
{
    const vehicle_profile_t *p = vehicle_profile_get_active();
    if (!p) return NULL;
    return &p->oil_temp_strategy;
}

const vehicle_override_t *vehicle_profile_get_override(void)
{
    const vehicle_profile_t *p = vehicle_profile_get_active();
    if (!p) return NULL;
    return vehicle_find_override(p->name);
}
