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
        .obd_gear_did = 0xD031,   // Mode 22 DID D031 = ZF 8HP current gear (BMW_GEAR_V2); read over the 6F1 EGS header
        .forced_protocol = 6,
        .obd_functional_addr = true,
        .obd_timeout = 0x0F,
        .poll_gap_ms = 1,      // BRZ PID-style 1ms slot gap for faster RPM refresh
    },
    {
        // Toyota GR Supra A90 (2019+, BMW B58 3.0T / B48 2.0T, ZF 8HP51)
        // Mechanically the BMW CLAR platform with a BMW DME, so OBD behaves like BMW F/G: 11-bit CAN, 7DF functional.
        // RPM path optimized like the BRZ PID profile: obd_timeout 0x0A (40ms) + poll_gap_ms 1 for a faster refresh.
        // Engine oil temp: the B58 DME reports it via Mode 22 DID 4402 ("oil temperature after filter"),
        // °C = raw*0.75 - 48 (2 bytes, 7E0 physical) — same as BMW G-series. Verified against bmw_pid_data/b58_pid_data.h.
        // Alternative DIDs: 4408 ("unfiltered", °C = raw*0.1 - 273.14) and 4425 ("sump", °C = raw/10).
        // Fallback to standard 01 5C if 4402 is unavailable. Gear/final-drive are placeholders to refine per trim.
        // Engine oil pressure: B58 DME reports it via Mode 22 DID 4436 (absolute pressure in hPa, 7E0 physical,
        // unsigned 16-bit, raw×1). Enables OBD oil pressure so the external ADS1115 ADC is skipped for this car.
        .name = "Supra A90",
        .final_drive_ratio = 2.813f,       // placeholder from BMW F/G; 3.0T Supra is ~3.15
        .tire_rolling_radius_m = 0.330f,   // 225/45R18 placeholder
        .gear_count = 8,                   // ZF 8HP51 8-speed
        .gear_ratios = {0, 5.250f, 3.360f, 2.172f, 1.720f, 1.316f, 1.000f, 0.822f, 0.640f},  // ZF 8HP51
        .gear_tolerance = 0.09f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_BMW_G_22_4402,  // B58 Mode 22 DID 4402: °C = raw*0.75 - 48
            .secondary = OIL_TEMP_MODE_PID_5C,       // standard 01 5C fallback
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,                 // B58/B48 turbo
        .obd_oil_pressure_did = 0x4436,    // Mode 22 DID 4436 (absolute hPa); supersedes the ADS1115 ADC
        .forced_protocol = 6,
        .obd_functional_addr = true,
        .obd_timeout = 0x0A,               // BRZ PID-style 40ms (faster than BMW F/G's 0x0F)
        .poll_gap_ms = 1,                  // BRZ PID-style 1ms polling
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
        // BMW E-series (E9x M3 S65 / E87 130i N52 / E9x N54 N55 / E46 E39). Standard mode 01 PIDs
        // go through 7DF functional addressing (the E-series DME does not answer physical 7E0 for
        // mode 01, same behaviour as BMW F/G). RPM/speed/coolant/intake/load/TPS all standard.
        // Oil temp & oil pressure use the N55 Mode 22 DIDs (bmw_pid_data/n55_pid_data.h) over the
        // 6F1 DME request header (see the override in vehicle_custom_config.h): 4402 oil temp
        // (°C = raw×0.75 − 48), 5822 oil temp (°C = raw − 60), 586F oil pressure (hPa, raw×1).
        // Confirmed on N55; N52/N54 likely share the same DIDs (unverified); S65/MSS60 may differ.
        .name = "BMW E",
        .final_drive_ratio = 3.846f,       // E92 M3 6MT final drive (M-DCT is 3.154)
        .tire_rolling_radius_m = 0.335f,   // rear 265/40R18
        .gear_count = 6,
        .gear_ratios = {0, 4.055f, 2.396f, 1.582f, 1.192f, 1.000f, 0.872f},  // Getrag GS6-53BZ 6MT
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // ignored when the override formula is present (see above)
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,              // ISO 15765-4 CAN 11-bit 500k
        .obd_functional_addr = true,       // 7DF functional addressing (same as phone apps)
        .obd_oil_pressure_did = 0x586F,    // N55 oil pressure DID (hPa, raw×1), read over ATSH6F1
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
        // FCA Giorgio platform is 29-bit CAN: standard OBD (RPM/speed/coolant temp/intake temp/load/TPS/voltage/MAP)
        // goes through the 29-bit functional broadcast 18DB33F1 (protocol 7), same as Jeep/Honda Integra — NOT 11-bit 7DF.
        // Oil temp 01 5C is not supported; use FCA UDS extended addressing ATSH18DA10F1 + 22 13 02 instead
        // (see the override in vehicle_custom_config.h).
        // MY2018+ SGW only blocks write operations (code clearing/matching); read-only live data needs no bypass.
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
        .forced_protocol = 7,              // ISO 15765-4 CAN 29-bit 500k (critical: not protocol 6)
        .obd_functional_addr = true,       // functional broadcast, not physical ECU address
        .obd_29bit_functional = true,      // 29-bit functional broadcast address (18DB33F1, not 7DF)
        .obd_timeout = 0x0F,
        .poll_gap_ms = 50,                 // extended UDS responses are a bit slow, keep it conservative
    },
    {
        // Jeep (generic placeholder; refine gear ratios/tire size once the exact model/engine/transmission is known)
        // Generic standard mode 01 PIDs (RPM/speed/coolant/intake/load/TPS/voltage) use the 29-bit functional
        // broadcast 18DB33F1 (forced protocol 7), same as Honda Integra. Oil temp goes through the generic
        // standard PID 01 5C — no manufacturer-specific CAN rules / formulas are applied.
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
        .forced_protocol = 7,              // ISO 15765-4 CAN 29-bit 500k (critical: not protocol 6)
        .obd_functional_addr = true,       // functional broadcast, not physical ECU address
        .obd_29bit_functional = true,      // 29-bit functional broadcast address (18DB33F1, not 7DF)
        .obd_timeout = 0x0F,
    },
    {
        // Honda Integra/本田形格 (2023+, 11th gen Civic Si platform, L15C7 1.5T CVT)
        // This vehicle uses 29-bit CAN extended addressing for ALL OBD communications, including standard mode 01 PIDs.
        // Functional broadcast address is 18DB33F1 (not the typical 11-bit 7DF). Confirmed via real-world scan and
        // autosportlabs forum (https://forum.autosportlabs.com/viewtopic.php?t=4671): "Hondas need 29-bit extended
        // IDs for basic MODE $01 PIDs. Rather than a traditional 11-bit 0x7DF broadcast, it needs an extended 29-bit
        // 0x18DB33F1 broadcast".
        //
        // Probing via fake_elm327.py with Car Scanner's Honda profile confirmed that under header 18DB33F1, several
        // mode 22 UDS DIDs successfully mapped to gauges (RPM/speed/coolant temp/fuel rail pressure), while standard
        // mode 01 PIDs were NOT tested in that specific scan (the CSV contained only proprietary mode 22 requests).
        // This profile attempts standard mode 01 first under the 29-bit functional header; if those don't respond,
        // the vehicle will need a custom override table mapping mode 22 DIDs (22 F40C→RPM, 22 F40D→speed, etc.).
        //
        // Oil temperature: non-Type R Integra/Civic models do NOT have a physical oil temp sensor (per IntegraForums
        // discussion); the Type R's oil temp is calculated, not a direct sensor reading. Standard PID 01 5C will be
        // attempted as primary strategy, but it may return NO DATA or a placeholder/calculated value. If it fails
        // consistently, user can manually switch to a different vehicle profile or we add a custom mode 22 fallback.
        //
        // Gear ratios: placeholder values for a generic CVT (continuously variable, no discrete gears). Ratio-based
        // gear detection is disabled (gear_count=0). If the actual transmission reports gear position via CAN or a
        // mode 22 DID, a custom override can be added later.
        .name = "Honda Integra",
        .final_drive_ratio = 4.438f,       // L15C7 CVT final drive (approximation from 11th gen Civic CVT specs)
        .tire_rolling_radius_m = 0.325f,   // 215/50R17 or 215/55R16 depending on trim; this is a mid-range estimate
        .gear_count = 0,                   // CVT has no discrete gears; disable ratio-based gear detection
        .gear_ratios = {0},
        .gear_tolerance = 0.0f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // attempt standard PID first; may not be a real sensor
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,                 // L15C7 is turbocharged; boost pressure via standard 01 0B
        .forced_protocol = 7,              // ISO 15765-4 CAN 29-bit 500k (critical: not protocol 6)
        .obd_functional_addr = true,       // use functional broadcast, not physical ECU address
        .obd_29bit_functional = true,      // 29-bit functional broadcast address (18DB33F1, not 7DF)
        .obd_timeout = 0x19,               // default timeout; adjust if responses are slow
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
