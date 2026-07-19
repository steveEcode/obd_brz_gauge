#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_PROTOCOL_MAGIC       0x4F42u
#define ESPNOW_PROTOCOL_VERSION     3u
#define ESPNOW_MASTER_NAME_LEN      12u

/*
 * 主表广播给所有从表的 OBD 数据包。
 *
 * 该结构由 ESP32 真机 ESP-NOW 和桌面 Simulator Bus 共同使用。
 * 修改字段时必须同步递增 ESPNOW_PROTOCOL_VERSION。
 */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t flags;

    uint32_t seq;

    uint16_t rpm;
    uint8_t speed;
    uint8_t sweep_step;

    int16_t coolant_temp;
    int16_t intake_temp;
    int16_t oil_temp;
    int16_t oil_pressure_x10;
    int16_t boost_x10;
    int16_t brake_temp_x10;
    int16_t load_pct;
    int16_t tps;

    int32_t bat_mv;

    char name[ESPNOW_MASTER_NAME_LEN];
} espnow_obd_packet_t;

#ifdef __cplusplus
}
#endif
