#pragma once
#include "espnow_v6.h"
#include "tpms_data.h"

/* Page subscriptions, replaced (not accumulated) on navigation. Vehicle data
 * keeps the existing all-valid broadcast; TPMS fields share one raw wheel
 * snapshot so all 16 derived fields and the diagnostics remain coherent. */
typedef struct {
    uint16_t obd;
    uint32_t veh;
    uint8_t tpms;                      /* bit 0..3: FL, FR, RL, RR */
} espnow_demand_t;
#define ESPNOW_CAP_SOURCES 0x04u
#define ESPNOW_SOURCES_HELLO_LEN 15u
#define ESPNOW_TPMS_FRAME_LEN 142u     /* 12 header + 4 * 32 wheel + CRC16 */
#define ESPNOW_SOURCE_TIMEOUT_US INT64_C(2000000)

size_t espnow_sources_hello_encode(uint8_t position, const espnow_demand_t *demand,
                                   uint8_t *out, size_t cap);
bool espnow_sources_hello_decode(const uint8_t *buf, size_t len, uint8_t *position,
                                 uint8_t *caps, espnow_demand_t *demand);
size_t espnow_tpms_encode(uint32_t seq, uint8_t wheels,
                          const tpms_wheel_data_t data[TPMS_WHEEL_COUNT],
                          uint8_t *out, size_t cap);
bool espnow_tpms_decode(const uint8_t *buf, size_t len, uint32_t *seq, uint8_t *wheels,
                        tpms_wheel_data_t data[TPMS_WHEEL_COUNT]);

/* Single worker writes; caller provides synchronization for display readers.
 * Frame reception time, not display time, anchors age. Repeated forwarding
 * never makes a sensor sample younger. Sequence ordering resets after silence. */
typedef struct {
    bool valid;
    uint8_t mac[6];
    uint32_t seq;
    int64_t received_us;
    tpms_wheel_data_t wheels[TPMS_WHEEL_COUNT];
} espnow_tpms_cache_t;
bool espnow_tpms_cache_apply(espnow_tpms_cache_t *cache, const uint8_t mac[6],
                             const uint8_t *buf, size_t len, int64_t received_us);
void espnow_tpms_cache_snapshot(const espnow_tpms_cache_t *cache, int64_t now_us,
                                tpms_wheel_data_t out[TPMS_WHEEL_COUNT]);
