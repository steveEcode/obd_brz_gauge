#pragma once
// ================================================================
//  vehicle_custom_config.h — vehicle custom configuration (data-driven)
//
//  Developers only need to edit this single file to add a new vehicle.
//  A vehicle not declared here = pure OBD2 standard protocol, zero-config and ready to use.
//
//  See docs/VEHICLE_CONFIG.md for details
// ================================================================

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Data channels ----
enum {
    CH_RPM = 0, CH_SPEED, CH_OIL_TEMP, CH_COOLANT,
    CH_TPS, CH_LOAD, CH_INTAKE, CH_BOOST, CH_GEAR, CH_COUNT
};

// ---- CAN frame decode rules ----
// One rule = extract one data channel from a bit field of a given CAN ID
typedef struct {
    uint16_t can_id;       // CAN frame ID (11-bit)
    uint8_t  bit_off;      // start bit (LSB=0)
    uint8_t  bit_len;      // bit length (1-32)
    float    scale;        // multiplier
    float    offset;       // offset (added after scaling)
    uint8_t  channel;      // CH_RPM / CH_SPEED / ...
} can_rule_t;

// ---- Oil temp formula types ----
enum {
    OIL_STD_PID = 0,   // Standard Mode 01 PID (single byte, value + offset)
    OIL_UDS_22,        // UDS Mode 22 (1-2 bytes, value * scale + offset)
    OIL_SPECIAL,       // Special parsing (Toyota Mode21 and other legacy cases)
};

// ---- Oil temp formula ----
typedef struct {
    uint8_t  type;         // OIL_STD_PID / OIL_UDS_22 / OIL_SPECIAL
    uint8_t  pid[3];       // PID bytes (standard=1 byte, UDS=2-3 bytes)
    uint8_t  pid_len;      // PID length
    uint8_t  resp_byte;    // which response data byte (0-based)
    uint8_t  resp_bytes;   // 1=single byte, 2=two bytes big-endian
    float    scale;        // multiplier
    float    offset;       // offset
    uint8_t  special_id;   // for OIL_SPECIAL: 0=Toyota21, reserved for future special cases
} oil_formula_t;

// ---- Vehicle override configuration ----
typedef struct {
    const char          *match_name;     // matches the name in vehicle_profiles
    const can_rule_t    *can_rules;      // CAN decode rule table (NULL=no CAN)
    uint8_t              can_rule_count;
    const oil_formula_t *oil_primary;    // primary oil temp formula (NULL=standard 01 5C)
    const oil_formula_t *oil_secondary;  // secondary oil temp formula
    uint8_t              forced_protocol;// ELM327 protocol (0=auto)
    bool                 functional_addr;// true=ATSH7DF
    uint8_t              obd_timeout;    // ATST value (0=default)
    bool                 has_boost;      // turbocharged vehicle
    uint8_t              poll_gap_ms;    // poll interval (0=default)
    const char          *uds_header_cmd; // ATSH header temporarily switched to before querying UDS oil temp (e.g. FCA extended addressing "ATSH18DA10F1\r"), the standard header is restored after the query; NULL=default behavior
    const char          *obd_gear_header_cmd; // ATSH header temporarily switched to before querying the gear DID (e.g. BMW "ATSH6F1\r" for the EGS), restored afterwards; NULL=fall back to uds_header_cmd then 7E0 physical
} vehicle_override_t;

// ================================================================
//  CAN rule tables — one per vehicle
// ================================================================

// BRZ ZC6 Gen1 (2013-2020)
// TPS + oil temp/coolant temp use CAN broadcast frames; RPM stays on OBD.
// Ref: https://github.com/timurrrr/ft86/blob/main/can_bus/gen1.md
static const can_rule_t can_rules_zc6[] = {
    { 0x140, 48,  8, 100.0f/255, 0.0f, CH_TPS },       // throttle byte6
    { 0x360, 16,  8, 1.0f,      -40.0f, CH_OIL_TEMP },  // oil temp byte2
    { 0x360, 24,  8, 1.0f,      -40.0f, CH_COOLANT },   // coolant temp byte3
};

// ================================================================
//  Oil temp formulas — 1-2 per vehicle
// ================================================================

// Standard OBD2 PID 01 5C (°C = A - 40)
static const oil_formula_t oil_std_5c = {
    OIL_STD_PID, {0x5C}, 1, 0, 1, 1.0f, -40.0f, 0
};

// Toyota/Subaru Mode 21 01 (special multi-frame parsing)
static const oil_formula_t oil_toyota_21 = {
    OIL_SPECIAL, {0x01}, 1, 0, 1, 1.0f, -40.0f, 0  // special_id=0
};

// Mazda 22 13 10 (two bytes: (A*256+B)/100 - 40)
static const oil_formula_t oil_mazda_1310 = {
    OIL_UDS_22, {0x13,0x10}, 2, 0, 2, 0.01f, -40.0f, 0
};

// Alfa Romeo Giulia 2.0T (gasoline) 22 13 02 (the 2nd response data byte is directly the oil temp in °C, no offset)
// Must be queried under the extended header 18DA10F1 (see override uds_header_cmd); cross-verified from two sources:
// danarda78/Alfaromeo-Giulia-Stelvio-PIDs + ClaudeMarais/Simple_OBD2_for_AlfaRomeoGiulia
static const oil_formula_t oil_giulia_1302 = {
    OIL_UDS_22, {0x13,0x02}, 2, 1, 1, 1.0f, 0.0f, 0
};

// Mazda 22 11 1F (single byte: A - 50)
static const oil_formula_t oil_mazda_111f = {
    OIL_UDS_22, {0x11,0x1F}, 2, 0, 1, 1.0f, -50.0f, 0
};

// BMW F/G 22 44 02 (single byte: B - 64)
static const oil_formula_t oil_bmw_4402 = {
    OIL_UDS_22, {0x44,0x02}, 2, 1, 1, 1.0f, -64.0f, 0
};

// BMW G 22 44 02 (two bytes: (A*256+B)*191.25/255 - 48)
static const oil_formula_t oil_bmw_g_4402 = {
    OIL_UDS_22, {0x44,0x02}, 2, 0, 2, 191.25f/255.0f, -48.0f, 0
};

// BMW 22 D0 02 (two bytes: same formula as G 4402)
static const oil_formula_t oil_bmw_d002 = {
    OIL_UDS_22, {0xD0,0x02}, 2, 0, 2, 191.25f/255.0f, -48.0f, 0
};

// BMW 22 03 F3 (single byte: A - 40)
static const oil_formula_t oil_bmw_03f3 = {
    OIL_UDS_22, {0x03,0xF3}, 2, 0, 1, 1.0f, -40.0f, 0
};

// BMW/MINI 22 58 22 (single byte: A - 60)
static const oil_formula_t oil_mini_5822 = {
    OIL_UDS_22, {0x58,0x22}, 2, 0, 1, 1.0f, -60.0f, 0
};

// BMW 22 11 1F (single byte: A - 50)
static const oil_formula_t oil_bmw_111f = {
    OIL_UDS_22, {0x11,0x1F}, 2, 0, 1, 1.0f, -50.0f, 0
};

// ================================================================
//  Override table — vehicles not listed here = pure OBD2 standard protocol
// ================================================================
static const vehicle_override_t s_vehicle_overrides[] = {
    {
        // BRZ ZC6 Gen1 (2013-2020, FA20 NA, Gen1)
        // RPM stays on OBD; TPS/coolant/oil come from CAN broadcast frames.
        .match_name      = "ZN/C6 CAN",
        .can_rules       = can_rules_zc6,
        .can_rule_count  = 3,   // TPS/oil temp/coolant temp from CAN; RPM stays on OBD
        .oil_primary     = &oil_toyota_21,
        .forced_protocol = 6,
        .obd_timeout     = 0x0A,
        .poll_gap_ms     = 1,
    },
    {
        // ZN/C6 standard PID fallback: no can_rules, RPM/coolant temp go through standard PIDs (01 0C / 01 05),
        // oil temp goes through Toyota Mode 21 01 (same as the old config before the CAN version was introduced)
        .match_name      = "ZN/C6 PID",
        .oil_primary     = &oil_toyota_21,
        .forced_protocol = 6,
        .obd_timeout     = 0x0A,
        .poll_gap_ms     = 1,
    },
    {
        .match_name      = "ZD8 OBD",
        .oil_primary     = &oil_std_5c,
        .forced_protocol = 6,
        .obd_timeout     = 0x0A,
        .poll_gap_ms     = 1,
    },
    {
        // ZD8 standard OBD fallback: no can_rules, RPM/coolant temp/oil temp all go through standard PIDs
        .match_name      = "ZD8",
        .oil_primary     = &oil_std_5c,
        .forced_protocol = 6,
        .obd_timeout     = 0x0A,
        .poll_gap_ms     = 1,
    },
    {
        .match_name      = "MX-5 ND",
        .oil_primary     = &oil_mazda_1310,
        .oil_secondary   = &oil_mazda_111f,
        .obd_timeout     = 0x0A,
        .poll_gap_ms     = 1,
    },
    {
        .match_name      = "BMW G OBD",
        .oil_primary     = &oil_std_5c,
        .oil_secondary   = &oil_std_5c,
        .forced_protocol = 6,
        .functional_addr = true,
        .obd_timeout     = 0x0A,
        .has_boost       = true,
    },
    {
        .match_name      = "BMW F/G",
        .oil_primary     = &oil_std_5c,       // Standard 01 5C (Mode 22 4402 unreliable via OBDII)
        .oil_secondary   = &oil_std_5c,
        .forced_protocol = 6,
        .functional_addr = true,
        .obd_timeout     = 0x0F,
        .poll_gap_ms     = 1,                 // BRZ PID-style 1ms slot gap for faster RPM refresh
        .has_boost       = true,
        .obd_gear_header_cmd = "ATSH6F1\r",   // gear DID lives on the EGS (transmission ECU), queried over 6F1 functional
    },
    {
        // BMW E-series (E9x M3 S65 / E87 130i N52 / E9x N54 N55): standard PIDs via 7DF functional addressing.
        // Oil temp uses the N55 Mode 22 DIDs over the 6F1 DME request header (bmw_pid_data/n55_pid_data.h):
        //   4402 "oil temperature after filter" (°C = raw×0.75 − 48, 2-byte), 5822 "oil temperature" (°C = raw − 60, 1-byte).
        // The E-series DME answers Mode 22 on 6F1 (physical), so uds_header_cmd="ATSH6F1\r" switches the header for the
        // query and restores 7DF afterwards. Confirmed on N55; N52/N54 likely share the DIDs, S65/MSS60 may differ.
        .match_name      = "BMW E",
        .oil_primary     = &oil_bmw_g_4402,   // 22 44 02: °C = raw×0.75 − 48
        .oil_secondary   = &oil_mini_5822,    // 22 58 22: °C = raw − 60
        .forced_protocol = 6,
        .functional_addr = true,
        .obd_timeout     = 0x0A,
        .uds_header_cmd  = "ATSH6F1\r",
    },
    {
        .match_name      = "JCW F56",
        .oil_primary     = &oil_mini_5822,
        .oil_secondary   = &oil_std_5c,
        .has_boost       = true,
    },
    {
        .match_name      = "POS 997.2",
        .oil_primary     = &oil_std_5c,
        .oil_secondary   = &oil_std_5c,
        .forced_protocol = 6,
    },
    {
        .match_name      = "POS 997.1",
        .oil_primary     = &oil_std_5c,
        .oil_secondary   = &oil_std_5c,
        .forced_protocol = 6,
    },
    {
        // Giulia 2.0T: oil temp goes through FCA UDS extended addressing 18DA10F1 (response 18DAF110); standard 01 5C is not supported.
        // Standard PIDs (RPM/speed/coolant temp/intake temp/load/TPS/voltage/MAP) go through 29-bit functional broadcast 18DB33F1 (protocol 7).
        // Other extensible DIDs (same header 18DA10F1): oil pressure 22 13 0A=A*10/255, gear 22 19 2D (0=N,0x10=R),
        // boost gauge pressure 22 19 5A=((A*256+B)-32768)/1000-1 bar, throttle 22 19 24=(A*256+B)/655.35 %
        .match_name      = "GIULIA 2.0T",
        .oil_primary     = &oil_giulia_1302,
        .functional_addr = true,
        .obd_timeout     = 0x0F,
        .has_boost       = true,
        .poll_gap_ms     = 50,
        .uds_header_cmd  = "ATSH18DA10F1\r",
    },
    // OBD2 Generic: not in the table = pure standard protocol
};

#define VEHICLE_OVERRIDE_COUNT (sizeof(s_vehicle_overrides) / sizeof(s_vehicle_overrides[0]))

// Find the override configuration (not found = pure OBD2 standard)
static inline const vehicle_override_t *vehicle_find_override(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < (int)VEHICLE_OVERRIDE_COUNT; i++) {
        if (strcmp(s_vehicle_overrides[i].match_name, name) == 0)
            return &s_vehicle_overrides[i];
    }
    return NULL;
}

// ---- Generic CAN bit-field extraction (little-endian) ----
static inline bool can_extract_bits_le(const uint8_t data[8],
                                       uint8_t bit_off, uint8_t bit_len,
                                       uint32_t *out)
{
    if (!data || !out || bit_len == 0 || bit_len > 32 ||
        (uint16_t)bit_off + bit_len > 64)
        return false;
    uint32_t val = 0;
    for (uint8_t i = 0; i < bit_len; i++) {
        uint8_t bit = bit_off + i;
        if (data[bit / 8] & (1u << (bit % 8)))
            val |= 1u << i;
    }
    *out = val;
    return true;
}

// ---- Generic CAN rule parsing: iterate the rule table, match the CAN ID, extract data into channels[] ----
static inline void can_apply_rules(const can_rule_t *rules, uint8_t count,
                                   uint16_t can_id, const uint8_t data[8],
                                   float channels[CH_COUNT])
{
    for (uint8_t i = 0; i < count; i++) {
        if (rules[i].can_id != can_id) continue;
        uint32_t raw = 0;
        if (!can_extract_bits_le(data, rules[i].bit_off, rules[i].bit_len, &raw))
            continue;
        float val = (float)raw * rules[i].scale + rules[i].offset;
        if (rules[i].channel < CH_COUNT)
            channels[rules[i].channel] = val;
    }
}

// ---- Generic oil temp formula execution: build the ELM327 command based on the formula type ----
// Returns the command string to send (static buffer); NULL means the SPECIAL type needs external handling
static inline const char *oil_formula_build_cmd(const oil_formula_t *f, char *buf, size_t buflen)
{
    if (!f || !buf) return NULL;
    if (f->type == OIL_STD_PID) {
        snprintf(buf, buflen, "01 %02X\r", f->pid[0]);
        return buf;
    }
    if (f->type == OIL_UDS_22) {
        if (f->pid_len == 2)
            snprintf(buf, buflen, "22 %02X %02X\r", f->pid[0], f->pid[1]);
        else if (f->pid_len == 3)
            snprintf(buf, buflen, "22 %02X %02X %02X\r", f->pid[0], f->pid[1], f->pid[2]);
        else
            return NULL;
        return buf;
    }
    return NULL;  // OIL_SPECIAL: the caller handles it itself
}

// ---- Generic oil temp response parsing: compute the temperature from the ELM327 response bytes ----
// resp_data: the data bytes in the response (excluding the mode/PID header)
// resp_len: number of data bytes
// Returns the computed temperature in °C; -32768 on failure
static inline int16_t oil_formula_parse_resp(const oil_formula_t *f,
                                             const uint32_t *resp_data,
                                             uint8_t resp_len)
{
    if (!f || !resp_data) return -32768;
    if (f->resp_byte >= resp_len) return -32768;

    float raw;
    if (f->resp_bytes == 2 && f->resp_byte + 1 < resp_len) {
        raw = (float)(resp_data[f->resp_byte] * 256 + resp_data[f->resp_byte + 1]);
    } else {
        raw = (float)resp_data[f->resp_byte];
    }
    float temp = raw * f->scale + f->offset;
    if (temp < -40.0f || temp > 215.0f) return -32768;
    return (int16_t)(temp + 0.5f);
}

#ifdef __cplusplus
}
#endif
