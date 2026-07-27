// ZC6 CAN 广播帧 ATMA 监听解码器 (移植自 hokori_vehicle_gauge)
// 在 ELM327 ATMA + ATH1 + ATCM/ATCF 持续监听模式下, 逐行解析 CAN 帧:
//   0x140 (100Hz): 转速(bits16-29, 14bit LE), 节气门(byte6/2.55), 油门踏板(byte0/2.55)
//   0x0D1 (50Hz):  车速(bytes0-1 LE × 0.015694)
// 参考: https://github.com/timurrrr/ft86/blob/main/can_bus/gen1.md
#ifndef ZC6_CAN_MONITOR_DECODE_H
#define ZC6_CAN_MONITOR_DECODE_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// ---- 通用: 从 ATMA 监听行中提取指定 CAN ID 的 payload ----

static inline bool zc6_can_hex_nibble(char ch, uint8_t *out)
{
    if (ch >= '0' && ch <= '9') { *out = (uint8_t)(ch - '0'); return true; }
    if (ch >= 'A' && ch <= 'F') { *out = (uint8_t)(ch - 'A' + 10); return true; }
    if (ch >= 'a' && ch <= 'f') { *out = (uint8_t)(ch - 'a' + 10); return true; }
    return false;
}

// 紧凑格式解析 (无空格: "140AABBCCDD...")
static inline bool zc6_can_extract_compact(const char *line, uint16_t can_id,
                                           uint8_t len, uint8_t out[8])
{
    uint8_t nib[40];
    size_t nc = 0, ps;
    uint16_t hdr;

    if (!line || !out || len == 0 || len > 8) return false;
    for (const char *p = line; *p && nc < sizeof(nib); ++p) {
        uint8_t v;
        if (zc6_can_hex_nibble(*p, &v)) nib[nc++] = v;
    }
    if (nc < (size_t)(3 + len * 2)) return false;
    hdr = (uint16_t)((nib[0] << 8) | (nib[1] << 4) | nib[2]);
    if (hdr != can_id) return false;
    ps = 3;
    // 跳过可能的 DLC 字节
    if (nc >= (size_t)(5 + len * 2)) {
        uint8_t dlc = (uint8_t)((nib[3] << 4) | nib[4]);
        if (dlc <= len) ps = 5;
    }
    if ((nc - ps) < (size_t)(len * 2)) return false;
    for (uint8_t i = 0; i < len; i++) {
        size_t pos = ps + i * 2;
        out[i] = (uint8_t)((nib[pos] << 4) | nib[pos + 1]);
    }
    return true;
}

// 空格分隔格式解析 ("140 AA BB CC DD EE FF GG HH")
static inline bool zc6_can_extract_payload(const char *line, uint16_t can_id,
                                           uint8_t len, uint8_t out[8])
{
    uint32_t tok[16];
    uint8_t tlens[16], tcount = 0;
    size_t bstart = 0, bcount = 0;
    int hdr_idx = -1;
    const char *p = line;

    if (!line || !out || len == 0 || len > 8) return false;

    while (*p && tcount < 16) {
        char tb[5] = {0};
        size_t tl = 0;
        if (!isxdigit((unsigned char)*p)) { ++p; continue; }
        while (isxdigit((unsigned char)*p) && tl < 4) tb[tl++] = *p++;
        while (isxdigit((unsigned char)*p)) ++p;
        if (!tl) continue;
        tok[tcount] = (uint32_t)strtoul(tb, NULL, 16);
        tlens[tcount] = (uint8_t)tl;
        ++tcount;
    }

    for (size_t i = 0; i < tcount; i++) {
        if (tlens[i] == 3 && tok[i] == can_id) { hdr_idx = (int)i; break; }
    }
    if (hdr_idx < 0) return zc6_can_extract_compact(line, can_id, len, out);

    bstart = (size_t)hdr_idx + 1;
    for (size_t i = bstart; i < tcount; i++) {
        if (tlens[i] <= 2) ++bcount;
    }
    // 跳过 DLC 字节 (ATMA/ATH1 常在 ID 后带 08)
    if (bcount > len && bstart < tcount && tlens[bstart] <= 2 && tok[bstart] <= len) {
        ++bstart; --bcount;
    }
    if (bcount < len) return zc6_can_extract_compact(line, can_id, len, out);

    bcount = 0;
    for (size_t i = bstart; i < tcount && bcount < len; i++) {
        if (tlens[i] <= 2) out[bcount++] = (uint8_t)tok[i];
    }
    return bcount >= len;
}

// 位域提取 (小端)
static inline bool zc6_can_bits_le(const uint8_t p[8], uint8_t bit_off,
                                   uint8_t bit_len, uint32_t *out)
{
    uint32_t v = 0;
    if (!p || !out || bit_len == 0 || bit_len > 32 ||
        (uint16_t)bit_off + bit_len > 64) return false;
    for (uint8_t i = 0; i < bit_len; i++) {
        uint8_t bit = bit_off + i;
        if (p[bit / 8] & (1u << (bit % 8))) v |= 1u << i;
    }
    *out = v;
    return true;
}

// ---- 0x140 解码: 转速 + 节气门 ----

static inline bool zc6_can_decode_140(const char *line, uint16_t *rpm, uint8_t *tps_pct)
{
    uint8_t p[8] = {0};
    uint32_t v;
    if (!zc6_can_extract_payload(line, 0x140, 8, p)) return false;
    if (rpm) {
        if (!zc6_can_bits_le(p, 16, 14, &v)) return false;
        *rpm = (uint16_t)v;
    }
    if (tps_pct) {
        *tps_pct = (uint8_t)((uint32_t)p[6] * 100 / 255);
    }
    return true;
}

// ---- 0x0D1 解码: 车速 (Gen1) ----

static inline bool zc6_can_decode_0d1(const char *line, uint8_t *speed_kmh)
{
    uint8_t p[8] = {0};
    if (!zc6_can_extract_payload(line, 0x0D1, 4, p)) return false;
    if (speed_kmh) {
        uint32_t raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        // 0.015694 ≈ 157/10000, 四舍五入
        *speed_kmh = (uint8_t)((raw * 157 + 5000) / 10000);
    }
    return true;
}

// ---- ZD8 Gen2: 0x40 解码 (转速 + 节气门, 同 Gen1 0x140 公式) ----

static inline bool zd8_can_decode_40(const char *line, uint16_t *rpm, uint8_t *tps_pct)
{
    uint8_t p[8] = {0};
    uint32_t v;
    if (!zc6_can_extract_payload(line, 0x40, 8, p)) return false;
    if (rpm) {
        if (!zc6_can_bits_le(p, 16, 14, &v)) return false;
        *rpm = (uint16_t)v;
    }
    if (tps_pct) {
        *tps_pct = (uint8_t)((uint32_t)p[4] * 100 / 255);  // byte E (index 4)
    }
    return true;
}

// ---- ZD8 Gen2: 0x345 解码 (油温 byte3-40, 水温 byte4-40, 10Hz) ----

static inline bool zd8_can_decode_345(const char *line, int16_t *oil_c, int16_t *clt_c)
{
    uint8_t p[8] = {0};
    if (!zc6_can_extract_payload(line, 0x345, 8, p)) return false;
    if (oil_c) *oil_c = (int16_t)((int)p[3] - 40);
    if (clt_c) *clt_c = (int16_t)((int)p[4] - 40);
    return true;
}

#endif // ZC6_CAN_MONITOR_DECODE_H
