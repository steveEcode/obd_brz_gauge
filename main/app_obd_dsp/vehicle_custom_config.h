#pragma once
// ================================================================
//  vehicle_custom_config.h — 车型自定义配置 (数据驱动)
//
//  开发者只需编辑这一个文件即可添加新车型。
//  不在此声明的车型 = 纯 OBD2 标准协议，零配置即用。
//
//  详见 docs/VEHICLE_CONFIG.md
// ================================================================

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 数据通道 ----
enum {
    CH_RPM = 0, CH_SPEED, CH_OIL_TEMP, CH_COOLANT,
    CH_TPS, CH_LOAD, CH_INTAKE, CH_BOOST, CH_GEAR, CH_COUNT
};

// ---- CAN 帧解码规则 ----
// 一条规则 = 从某个 CAN ID 的某个位域提取一个数据通道
typedef struct {
    uint16_t can_id;       // CAN 帧 ID (11-bit)
    uint8_t  bit_off;      // 起始位 (LSB=0)
    uint8_t  bit_len;      // 位长度 (1-32)
    float    scale;        // 乘数
    float    offset;       // 偏移 (乘后加)
    uint8_t  channel;      // CH_RPM / CH_SPEED / ...
} can_rule_t;

// ---- 油温公式类型 ----
enum {
    OIL_STD_PID = 0,   // 标准 Mode 01 PID (单字节, value + offset)
    OIL_UDS_22,        // UDS Mode 22 (1-2 字节, value * scale + offset)
    OIL_SPECIAL,       // 特殊解析 (Toyota Mode21 / Porsche CAN441 等)
};

// ---- 油温公式 ----
typedef struct {
    uint8_t  type;         // OIL_STD_PID / OIL_UDS_22 / OIL_SPECIAL
    uint8_t  pid[3];       // PID 字节 (标准=1字节, UDS=2-3字节)
    uint8_t  pid_len;      // PID 长度
    uint8_t  resp_byte;    // 响应数据第几字节 (0-based)
    uint8_t  resp_bytes;   // 1=单字节, 2=双字节大端
    float    scale;        // 乘数
    float    offset;       // 偏移
    uint8_t  special_id;   // OIL_SPECIAL 时: 0=Toyota21, 1=Porsche441
} oil_formula_t;

// ---- 车型覆盖配置 ----
typedef struct {
    const char          *match_name;     // 匹配 vehicle_profiles 里的 name
    const can_rule_t    *can_rules;      // CAN 解码规则表 (NULL=不用CAN)
    uint8_t              can_rule_count;
    const oil_formula_t *oil_primary;    // 油温主公式 (NULL=标准01 5C)
    const oil_formula_t *oil_secondary;  // 油温备用公式
    uint8_t              forced_protocol;// ELM327 协议 (0=自动)
    bool                 functional_addr;// true=ATSH7DF
    uint8_t              obd_timeout;    // ATST 值 (0=默认)
    bool                 has_boost;      // 涡轮车
    uint8_t              poll_gap_ms;    // 轮询间隔 (0=默认)
} vehicle_override_t;

// ================================================================
//  CAN 规则表 — 每个车型一张
// ================================================================

// BRZ ZC6 Gen1 (2013-2020)
// 参考: https://github.com/timurrrr/ft86/blob/main/can_bus/gen1.md
static const can_rule_t can_rules_zc6[] = {
    { 0x140, 16, 14, 1.0f,        0.0f, CH_RPM },       // 转速
    { 0x140, 48,  8, 100.0f/255,  0.0f, CH_TPS },       // 节气门 byte6
    { 0x360, 16,  8, 1.0f,      -40.0f, CH_OIL_TEMP },  // 油温 byte2
    { 0x360, 24,  8, 1.0f,      -40.0f, CH_COOLANT },   // 水温 byte3
    // 车速走 OBD PID 01 0D, 不走 CAN 0x0D1 (避免静止噪声导致非零爬升)
};

// BMW G-series (G20/G21/G22/G80/G82) PT-CAN
// Ref: racechrono-canbus decoder_bmwg8x.cpp, thesecretingredient.neocities.org/bmw/can/g29/
// PT-CAN 500kbps, 11-bit standard frames.
// 0x0A5 (100Hz): RPM byte5-6 LE (raw×4)
// 0x254 (50Hz):  wheel speeds byte 0/2/4/6 LE (raw×0.015625−511.98 km/h)
// 0x3F9 (1Hz):   coolant byte4 (raw−48), oil byte5 (raw−48), gear byte6 nibble (raw−4)
// 0x0D9 (100Hz): throttle byte2-3 12-bit
static const can_rule_t can_rules_bmw_g[] = {
    { 0x0A5, 40, 16, 0.25f,         0.0f, CH_RPM },       // 转速 byte5-6 LE, raw÷4 (1/min)
    { 0x0A5, 16, 16, 1.0f,          0.0f, CH_TPS },       // 引擎扭矩 (备用, 暂不接入UI)
    { 0x254, 32, 16, 0.015625f, -511.98f, CH_SPEED },     // 左前轮速 byte4-5 LE
    { 0x3F9, 32,  8, 1.0f,        -48.0f, CH_COOLANT },   // 水温 byte4, raw−48
    { 0x3F9, 40,  8, 1.0f,        -48.0f, CH_OIL_TEMP },  // 油温 byte5, raw−48
    { 0x3F9, 48,  4, 1.0f,         -4.0f, CH_GEAR },   // 档位 byte6低4位, raw−4 (0=P, 1=R, 2=N, 3=D, 4+=M1…)
};

// BRZ ZD8 Gen2 (2022+)
// 参考: https://github.com/timurrrr/ft86/blob/main/can_bus/gen2.md
static const can_rule_t can_rules_zd8[] = {
    { 0x040, 16, 14, 1.0f,        0.0f, CH_RPM },       // 转速
    { 0x040, 32,  8, 100.0f/255,  0.0f, CH_TPS },       // 节气门 byte4
    { 0x345, 24,  8, 1.0f,      -40.0f, CH_OIL_TEMP },  // 油温 byte3
    { 0x345, 32,  8, 1.0f,      -40.0f, CH_COOLANT },   // 水温 byte4
};

// ================================================================
//  油温公式 — 每个车型 1-2 个
// ================================================================

// 标准 OBD2 PID 01 5C (°C = A - 40)
static const oil_formula_t oil_std_5c = {
    OIL_STD_PID, {0x5C}, 1, 0, 1, 1.0f, -40.0f, 0
};

// Toyota/Subaru Mode 21 01 (特殊多帧解析)
static const oil_formula_t oil_toyota_21 = {
    OIL_SPECIAL, {0x01}, 1, 0, 1, 1.0f, -40.0f, 0  // special_id=0
};

// Mazda 22 13 10 (双字节: (A*256+B)/100 - 40)
static const oil_formula_t oil_mazda_1310 = {
    OIL_UDS_22, {0x13,0x10}, 2, 0, 2, 0.01f, -40.0f, 0
};

// Mazda 22 11 1F (单字节: A - 50)
static const oil_formula_t oil_mazda_111f = {
    OIL_UDS_22, {0x11,0x1F}, 2, 0, 1, 1.0f, -50.0f, 0
};

// BMW F/G 22 44 02 (单字节: B - 64)
static const oil_formula_t oil_bmw_4402 = {
    OIL_UDS_22, {0x44,0x02}, 2, 1, 1, 1.0f, -64.0f, 0
};

// BMW G 22 44 02 (双字节: (A*256+B)*191.25/255 - 48)
static const oil_formula_t oil_bmw_g_4402 = {
    OIL_UDS_22, {0x44,0x02}, 2, 0, 2, 191.25f/255.0f, -48.0f, 0
};

// BMW 22 D0 02 (双字节: 同 G 4402 公式)
static const oil_formula_t oil_bmw_d002 = {
    OIL_UDS_22, {0xD0,0x02}, 2, 0, 2, 191.25f/255.0f, -48.0f, 0
};

// BMW 22 03 F3 (单字节: A - 40)
static const oil_formula_t oil_bmw_03f3 = {
    OIL_UDS_22, {0x03,0xF3}, 2, 0, 1, 1.0f, -40.0f, 0
};

// BMW/MINI 22 58 22 (单字节: A - 60)
static const oil_formula_t oil_mini_5822 = {
    OIL_UDS_22, {0x58,0x22}, 2, 0, 1, 1.0f, -60.0f, 0
};

// BMW 22 11 1F (单字节: A - 50)
static const oil_formula_t oil_bmw_111f = {
    OIL_UDS_22, {0x11,0x1F}, 2, 0, 1, 1.0f, -50.0f, 0
};

// Porsche CAN 0x441 Gen2 (byte5: x - 60)
static const oil_formula_t oil_porsche_9972 = {
    OIL_SPECIAL, {0}, 0, 5, 1, 1.0f, -60.0f, 1  // special_id=1
};

// Porsche CAN 0x441 Gen1 (byte5: x*3/4 - 48)
static const oil_formula_t oil_porsche_9971 = {
    OIL_SPECIAL, {0}, 0, 5, 1, 0.75f, -48.0f, 1  // special_id=1
};

// ================================================================
//  覆盖表 — 不在这里的车型 = 纯 OBD2 标准协议
// ================================================================
static const vehicle_override_t s_vehicle_overrides[] = {
    {
        .match_name      = "ZC/N6",
        .can_rules       = can_rules_zc6,
        .can_rule_count  = 4,   // 转速/TPS/油温/水温, 车速走 OBD
        .oil_primary     = &oil_toyota_21,
        .forced_protocol = 6,
        .poll_gap_ms     = 1,
    },
    {
        .match_name      = "ZD8 CAN",
        .can_rules       = can_rules_zd8,
        .can_rule_count  = 4,
        .oil_primary     = &oil_std_5c,
        .forced_protocol = 6,
        .poll_gap_ms     = 1,
    },
    {
        // ZD8 标准 OBD 兜底: 不设 can_rules, 转速/水温/油温全走标准 PID
        .match_name      = "ZD8",
        .oil_primary     = &oil_std_5c,
        .forced_protocol = 6,
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
        .match_name      = "BMW G CAN",
        .can_rules       = can_rules_bmw_g,
        .can_rule_count  = 6,
        .oil_primary     = &oil_std_5c,       // CAN 0x3F9 直接提供油温, OBD 兜底用标准 01 5C
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
        .has_boost       = true,
    },
    {
        .match_name      = "JCW F56",
        .oil_primary     = &oil_mini_5822,
        .oil_secondary   = &oil_std_5c,
        .has_boost       = true,
    },
    {
        .match_name      = "POS 997.2",
        .oil_primary     = &oil_porsche_9972,
        .oil_secondary   = &oil_std_5c,
        .forced_protocol = 6,
    },
    {
        .match_name      = "POS 997.1",
        .oil_primary     = &oil_porsche_9971,
        .oil_secondary   = &oil_std_5c,
        .forced_protocol = 6,
    },
    // OBD2 Generic: 不在表里 = 纯标准协议
};

#define VEHICLE_OVERRIDE_COUNT (sizeof(s_vehicle_overrides) / sizeof(s_vehicle_overrides[0]))

// 查找覆盖配置 (找不到 = 纯 OBD2 标准)
static inline const vehicle_override_t *vehicle_find_override(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < (int)VEHICLE_OVERRIDE_COUNT; i++) {
        if (strcmp(s_vehicle_overrides[i].match_name, name) == 0)
            return &s_vehicle_overrides[i];
    }
    return NULL;
}

// ---- 通用 CAN 位域提取 (小端) ----
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

// ---- 通用 CAN 规则解析: 遍历规则表, 匹配 CAN ID, 提取数据写入 channels[] ----
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

// ---- 通用油温公式执行: 根据公式类型构建 ELM327 命令 ----
// 返回需要发送的命令字符串 (静态缓冲), NULL 表示 SPECIAL 类型需外部处理
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
    return NULL;  // OIL_SPECIAL: 调用方自行处理
}

// ---- 通用油温响应解析: 从 ELM327 响应字节计算温度 ----
// resp_data: 响应中的数据字节 (不含 mode/PID 头)
// resp_len: 数据字节数
// 返回计算后的温度 °C, 失败返回 -32768
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
