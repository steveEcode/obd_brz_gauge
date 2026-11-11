#include "espnow_sources.h"
#include <limits.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

size_t espnow_sources_hello_encode(uint8_t position, const espnow_demand_t *d,
                                   uint8_t *out, size_t cap)
{
    if (!d || !out || cap < ESPNOW_SOURCES_HELLO_LEN) return 0;
    if (!espnow_hello_encode_demand(position,
        ESPNOW_CAP_V6_DATA | ESPNOW_CAP_DEMAND_MASK | ESPNOW_CAP_SOURCES, d->obd, out, cap)) return 0;
    wr32(out + 10, d->veh);
    out[14] = d->tpms & 0x0fu;
    return ESPNOW_SOURCES_HELLO_LEN;
}

bool espnow_sources_hello_decode(const uint8_t *buf, size_t len, uint8_t *pos,
                                 uint8_t *caps, espnow_demand_t *d)
{
    uint8_t c;
    espnow_demand_t result = {0};
    if (!espnow_hello_decode_demand(buf, len, pos, &c, &result.obd)) return false;
    if (c & ESPNOW_CAP_SOURCES) {
        if (!(c & ESPNOW_CAP_DEMAND_MASK) || len < ESPNOW_SOURCES_HELLO_LEN || (buf[14] & 0xf0u)) return false;
        result.veh = rd32(buf + 10);
        result.tpms = buf[14];
    }
    if (caps) *caps = c;
    if (d) *d = result;
    return true;
}

static bool encode_id(const char id[7], uint8_t *out)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 6; ++i) {
        unsigned c = (unsigned char)id[i];
        unsigned n = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (n > 15) return false;
        value = (value << 4) | n;
    }
    if (id[6]) return false;
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8); out[2] = (uint8_t)(value >> 16);
    return true;
}

size_t espnow_tpms_encode(uint32_t seq, uint8_t wheels,
                          const tpms_wheel_data_t data[TPMS_WHEEL_COUNT], uint8_t *out, size_t cap)
{
    if (!data || !out || cap < ESPNOW_TPMS_FRAME_LEN || !wheels || (wheels & 0xf0u)) return 0;
    memset(out, 0, ESPNOW_TPMS_FRAME_LEN);
    wr16(out, ESPNOW_MAGIC); out[2] = ESPNOW_V6_VERSION; out[3] = ESPNOW_V6_TYPE_TPMS;
    wr32(out + 4, seq); out[8] = wheels;
    for (unsigned i = 0; i < TPMS_WHEEL_COUNT; ++i) {
        const tpms_wheel_data_t *w = &data[i];
        uint8_t *p = out + 12 + 32 * i;
        if (!(wheels & (1u << i)) || !w->bound) continue;
        if (!encode_id(w->sensor_id, p + 1)) return 0;
        p[0] = 1;
        if (!w->received) continue;
        p[0] |= 2;
        wr32(p + 4, w->sample.data.pressure_pa);
        wr16(p + 8, (uint16_t)w->sample.data.temperature_c);
        wr16(p + 10, w->sample.data.voltage_mv);
        p[12] = w->sample.data.state; p[13] = w->sample.data.version; p[14] = w->sample.data.ble_version_code;
        wr32(p + 15, (uint32_t)w->sample.rssi);
        memcpy(p + 19, w->sample.mac, 6); p[25] = w->sample.addr_type;
        wr32(p + 26, w->age_ms);
    }
    wr16(out + ESPNOW_TPMS_FRAME_LEN - 2, espnow_crc16(out, ESPNOW_TPMS_FRAME_LEN - 2));
    return ESPNOW_TPMS_FRAME_LEN;
}

bool espnow_tpms_decode(const uint8_t *buf, size_t len, uint32_t *seq, uint8_t *wheels,
                        tpms_wheel_data_t data[TPMS_WHEEL_COUNT])
{
    if (!buf || !data || len != ESPNOW_TPMS_FRAME_LEN ||
        espnow_classify(buf, len) != ESPNOW_PKT_V6_TPMS ||
        !buf[8] || (buf[8] & 0xf0u) || buf[9] || buf[10] || buf[11] ||
        rd16(buf + len - 2) != espnow_crc16(buf, len - 2)) return false;
    memset(data, 0, TPMS_WHEEL_COUNT * sizeof(*data));
    for (unsigned i = 0; i < TPMS_WHEEL_COUNT; ++i) {
        const uint8_t *p = buf + 12 + 32 * i;
        tpms_wheel_data_t *w = &data[i];
        if ((p[0] & 0xfcu) || p[30] || p[31] || (p[0] == 2)) return false;
        if (!(buf[8] & (1u << i))) { if (p[0]) return false; continue; }
        w->bound = (p[0] & 1u) != 0;
        if (!w->bound) continue;
        static const char hex[] = "0123456789ABCDEF";
        for (unsigned j = 0; j < 6; ++j) w->sensor_id[j] = hex[(p[3 - j / 2] >> ((1 - j % 2) * 4)) & 15];
        w->received = (p[0] & 2u) != 0;
        if (!w->received) continue;
        memcpy(w->sample.data.sensor_id, w->sensor_id, sizeof(w->sensor_id));
        w->sample.data.pressure_pa = rd32(p + 4);
        w->sample.data.temperature_c = (int16_t)rd16(p + 8);
        w->sample.data.voltage_mv = rd16(p + 10);
        w->sample.data.state = p[12]; w->sample.data.version = p[13]; w->sample.data.ble_version_code = p[14];
        w->sample.rssi = (int32_t)rd32(p + 15);
        memcpy(w->sample.mac, p + 19, 6); w->sample.addr_type = p[25];
        w->age_ms = rd32(p + 26); w->fresh = w->age_ms < TPMS_FRESH_MS;
    }
    if (seq) *seq = rd32(buf + 4);
    if (wheels) *wheels = buf[8];
    return true;
}

bool espnow_tpms_cache_apply(espnow_tpms_cache_t *c, const uint8_t mac[6],
                             const uint8_t *buf, size_t len, int64_t received_us)
{
    uint32_t seq;
    tpms_wheel_data_t data[TPMS_WHEEL_COUNT];
    if (!c || !mac || !espnow_tpms_decode(buf, len, &seq, NULL, data)) return false;
    if (c->valid && !memcmp(c->mac, mac, 6) && received_us - c->received_us < ESPNOW_SOURCE_TIMEOUT_US) {
        uint32_t delta = seq - c->seq;
        if (!delta || delta >= UINT32_C(0x80000000) || received_us < c->received_us) return false;
    }
    c->valid = true; memcpy(c->mac, mac, 6); c->seq = seq; c->received_us = received_us;
    memcpy(c->wheels, data, sizeof(data));
    return true;
}

void espnow_tpms_cache_snapshot(const espnow_tpms_cache_t *c, int64_t now_us,
                                tpms_wheel_data_t out[TPMS_WHEEL_COUNT])
{
    memset(out, 0, TPMS_WHEEL_COUNT * sizeof(*out));
    if (!c || !c->valid || now_us < c->received_us || now_us - c->received_us >= ESPNOW_SOURCE_TIMEOUT_US) return;
    memcpy(out, c->wheels, sizeof(c->wheels));
    uint64_t elapsed_ms = (uint64_t)(now_us - c->received_us) / 1000;
    for (unsigned i = 0; i < TPMS_WHEEL_COUNT; ++i) {
        tpms_wheel_data_t *w = &out[i];
        if (!w->received) continue;
        uint64_t age = (uint64_t)w->age_ms + elapsed_ms;
        w->age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        w->fresh = w->age_ms < TPMS_FRESH_MS;
    }
}
