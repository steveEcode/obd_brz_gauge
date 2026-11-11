#include "espnow_sources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define CHECK(x) assert(x)
static void crc(uint8_t *b) {
    uint16_t c = espnow_crc16(b, ESPNOW_TPMS_FRAME_LEN - 2);
    b[ESPNOW_TPMS_FRAME_LEN - 2] = (uint8_t)c;
    b[ESPNOW_TPMS_FRAME_LEN - 1] = (uint8_t)(c >> 8);
}
int main(void)
{
    uint8_t b[250], legacy[10], pos, caps;
    espnow_demand_t d = {.obd = 0x8011, .veh = UINT32_C(0x80000001), .tpms = 15}, decoded;
    CHECK(espnow_sources_hello_encode(2, &d, b, 14) == 0);
    size_t len = espnow_sources_hello_encode(2, &d, b, sizeof(b));
    CHECK(len == 15 && b[8] == 0x11 && b[9] == 0x80 && b[10] == 1 && b[13] == 0x80 && b[14] == 15);
    CHECK(espnow_sources_hello_decode(b, len, &pos, &caps, &decoded));
    CHECK(pos == 2 && caps == 7 && decoded.obd == d.obd && decoded.veh == d.veh && decoded.tpms == 15);
    uint16_t oldmask = 0;
    CHECK(espnow_hello_decode_demand(b, len, &pos, &caps, &oldmask) && oldmask == d.obd);
    for (size_t n = 8; n < 15; ++n) CHECK(!espnow_sources_hello_decode(b, n, NULL, NULL, &decoded));
    b[14] = 0x80; CHECK(!espnow_sources_hello_decode(b, len, NULL, NULL, &decoded));
    len = espnow_hello_encode_demand(1, 3, 0x321, legacy, sizeof(legacy));
    CHECK(espnow_sources_hello_decode(legacy, len, NULL, NULL, &decoded));
    CHECK(decoded.obd == 0x321 && !decoded.veh && !decoded.tpms);
    len = espnow_hello_encode(1, 1, legacy, sizeof(legacy));
    CHECK(espnow_sources_hello_decode(legacy, len, NULL, NULL, &decoded) && !decoded.obd);

    tpms_wheel_data_t w[4] = {0}, out[4];
    const char *ids[] = {"18010F", "000001", "ABCDEF", "000000"};
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(w[i].sensor_id, ids[i], 7); w[i].bound = true; w[i].received = i != 1;
        w[i].sample.data.pressure_pa = 210000 + i;
        w[i].sample.data.temperature_c = (int16_t)(-20 + (int)i);
        w[i].sample.data.voltage_mv = (uint16_t)(2800 + i);
        w[i].sample.data.state = (uint8_t)i;
        w[i].sample.data.version = (uint8_t)(20 + i);
        w[i].sample.data.ble_version_code = 14;
        w[i].sample.rssi = -90 + (int)i; w[i].sample.mac[0] = (uint8_t)(0xf0 + i);
        w[i].sample.mac[5] = 0x19; w[i].sample.addr_type = 1;
        w[i].age_ms = i == 3 ? TPMS_FRESH_MS : 119500;
    }
    uint32_t seq; uint8_t wheels;
    CHECK(espnow_tpms_encode(0x12345678, 15, w, b, sizeof(b)) == ESPNOW_TPMS_FRAME_LEN);
    CHECK(b[4] == 0x78 && b[7] == 0x12 && b[8] == 15 && b[13] == 0x0f && b[15] == 0x18);
    CHECK(espnow_tpms_decode(b, ESPNOW_TPMS_FRAME_LEN, &seq, &wheels, out));
    CHECK(seq == 0x12345678 && wheels == 15);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(!strcmp(out[i].sensor_id, ids[i]) && out[i].bound && out[i].received == w[i].received);
        if (!w[i].received) continue;
        CHECK(out[i].sample.data.pressure_pa == w[i].sample.data.pressure_pa);
        CHECK(out[i].sample.data.temperature_c == w[i].sample.data.temperature_c);
        CHECK(out[i].sample.data.voltage_mv == w[i].sample.data.voltage_mv);
        CHECK(out[i].sample.data.state == w[i].sample.data.state);
        CHECK(out[i].sample.data.version == w[i].sample.data.version && out[i].sample.data.ble_version_code == 14);
        CHECK(out[i].sample.rssi == w[i].sample.rssi && out[i].sample.addr_type == 1);
        CHECK(!memcmp(out[i].sample.mac, w[i].sample.mac, 6));
        CHECK(out[i].fresh == (i != 3) && out[i].age_ms == w[i].age_ms);
    }
    for (size_t n = 0; n < ESPNOW_TPMS_FRAME_LEN; ++n) CHECK(!espnow_tpms_decode(b, n, NULL, NULL, out));
    b[30] ^= 1; CHECK(!espnow_tpms_decode(b, ESPNOW_TPMS_FRAME_LEN, NULL, NULL, out)); b[30] ^= 1;
    b[9] = 1; crc(b); CHECK(!espnow_tpms_decode(b, ESPNOW_TPMS_FRAME_LEN, NULL, NULL, out)); b[9] = 0;
    b[12] = 2; crc(b); CHECK(!espnow_tpms_decode(b, ESPNOW_TPMS_FRAME_LEN, NULL, NULL, out));
    CHECK(!espnow_tpms_encode(0, 0, w, b, sizeof(b)) && !espnow_tpms_encode(0, 16, w, b, sizeof(b)));
    w[0].sensor_id[0] = 'x'; CHECK(!espnow_tpms_encode(0, 15, w, b, sizeof(b))); w[0].sensor_id[0] = '1';

    espnow_tpms_cache_t cache = {0}; uint8_t mac[6] = {2};
    espnow_tpms_cache_snapshot(&cache, 0, out); CHECK(!out[0].received);
    espnow_tpms_encode(UINT32_MAX, 15, w, b, sizeof(b));
    CHECK(espnow_tpms_cache_apply(&cache, mac, b, ESPNOW_TPMS_FRAME_LEN, 1000000));
    CHECK(!espnow_tpms_cache_apply(&cache, mac, b, ESPNOW_TPMS_FRAME_LEN, 1100000)); // duplicate
    espnow_tpms_cache_snapshot(&cache, 1499999, out); CHECK(out[0].fresh && out[0].age_ms == 119999);
    espnow_tpms_cache_snapshot(&cache, 1500000, out); CHECK(!out[0].fresh && out[0].age_ms == 120000);
    // Wrap is forward; a subsequent heartbeat carries the sensor's true age.
    w[0].age_ms = 120000;
    espnow_tpms_encode(0, 15, w, b, sizeof(b));
    CHECK(espnow_tpms_cache_apply(&cache, mac, b, ESPNOW_TPMS_FRAME_LEN, 1500000));
    espnow_tpms_cache_snapshot(&cache, 1500000, out); CHECK(!out[0].fresh);
    espnow_tpms_encode(UINT32_MAX, 15, w, b, sizeof(b));
    CHECK(!espnow_tpms_cache_apply(&cache, mac, b, ESPNOW_TPMS_FRAME_LEN, 1600000)); // reordered
    espnow_tpms_cache_snapshot(&cache, 3500000, out); CHECK(!out[0].bound && !out[0].received); // exact timeout
    // Master reboot after silence is allowed, subscriptions replace missing wheels.
    espnow_tpms_encode(1, 1, w, b, sizeof(b));
    CHECK(espnow_tpms_cache_apply(&cache, mac, b, ESPNOW_TPMS_FRAME_LEN, 3500000));
    espnow_tpms_cache_snapshot(&cache, 3500000, out); CHECK(out[0].bound && !out[2].bound);
    w[0].bound = false;
    espnow_tpms_encode(2, 1, w, b, sizeof(b));
    CHECK(espnow_tpms_cache_apply(&cache, mac, b, ESPNOW_TPMS_FRAME_LEN, 3600000));
    espnow_tpms_cache_snapshot(&cache, 3600000, out); CHECK(!out[0].bound && !out[0].received);
    w[0].bound = true; w[0].age_ms = UINT32_MAX - 10;
    espnow_tpms_encode(3, 1, w, b, sizeof(b));
    CHECK(espnow_tpms_cache_apply(&cache, mac, b, ESPNOW_TPMS_FRAME_LEN, 3700000));
    espnow_tpms_cache_snapshot(&cache, 4700000, out); CHECK(out[0].age_ms == UINT32_MAX && !out[0].fresh);
    puts("source HELLO compatibility, TPMS raw codec, freshness, ordering and expiry passed");
}
