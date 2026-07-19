#ifndef SIMULATOR_BUS_H
#define SIMULATOR_BUS_H

#include <stdint.h>

#include "bsp_obd_dsp/espnow_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;

    uint32_t master_pid;
    uint32_t master_session;
    uint64_t heartbeat_ms;

    /*
     * 与真机 ESP-NOW 广播完全相同的数据包。
     */
    espnow_obd_packet_t packet;
} simulator_bus_state_t;

typedef struct {
    uint32_t master_pid;
    uint32_t master_session;
    uint64_t heartbeat_ms;

    espnow_obd_packet_t packet;
} simulator_bus_snapshot_t;

int simulator_bus_open(void);

void simulator_bus_close(void);

simulator_bus_state_t *
simulator_bus_get_state(void);

/*
 * Master 原子发布 PID、会话、心跳和数据包。
 */
int simulator_bus_publish(
    const espnow_obd_packet_t *packet,
    uint32_t master_pid,
    uint32_t master_session,
    uint64_t heartbeat_ms
);

/*
 * Slave 原子读取完整快照。
 *
 * 返回：
 *   0  读取成功
 *   1  尚无有效数据
 *  -1  参数错误或连续读取失败
 */
int simulator_bus_read(
    simulator_bus_snapshot_t *out_snapshot
);

/*
 * Master 正常退出时清除自己的 Bus 所有权。
 */
void simulator_bus_clear_master(
    uint32_t master_pid,
    uint32_t master_session
);

#ifdef __cplusplus
}
#endif

#endif
