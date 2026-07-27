#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/espnow_protocol.h"
#include "bsp_obd_dsp/espnow_simulator.h"

#include "app_obd_dsp/obd_data_cache.h"

#include "simulator_bus.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * 扫表同步接口由原始 ui.c 提供。
 * 与真机 espnow_link.c 使用相同接口。
 */
extern int ui_sweep_get_step(void);
extern void ui_sweep_set_step(int step);
extern int ui_intro_get_step(void);
extern void ui_intro_set_step(int step);

#define SIM_ESPNOW_INTERVAL_MS 100u
#define SIM_ESPNOW_TIMEOUT_MS 2000u

typedef enum {
    SIM_ESPNOW_STOPPED = 0,
    SIM_ESPNOW_MASTER,
    SIM_ESPNOW_SLAVE
} simulator_espnow_mode_t;

static simulator_espnow_mode_t s_mode =
    SIM_ESPNOW_STOPPED;

static bool s_bus_ready;
static bool s_have_slave_data;
static bool s_offline_reported;

static uint32_t s_master_session;

static uint32_t s_tx_seq;
static uint32_t s_tx_elapsed_ms;

static uint32_t s_last_rx_session;
static uint32_t s_last_rx_seq;
static uint32_t s_rx_age_ms;

static char s_master_name[
    ESPNOW_MASTER_NAME_LEN
];

static uint64_t simulator_monotonic_ms(void)
{
    struct timespec time_value;

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &time_value
        ) != 0
    ) {
        return 0u;
    }

    return
        (uint64_t)time_value.tv_sec *
            1000u +
        (uint64_t)time_value.tv_nsec /
            1000000u;
}

static uint32_t simulator_generate_session(void)
{
    uint64_t now =
        simulator_monotonic_ms();

    uint32_t session =
        (uint32_t)now ^
        (uint32_t)(now >> 32u) ^
        ((uint32_t)getpid() << 16u);

    if (session == 0u) {
        session = 1u;
    }

    return session;
}

static bool simulator_pid_is_alive(
    uint32_t pid
)
{
    if (pid == 0u) {
        return false;
    }

    if (kill((pid_t)pid, 0) == 0) {
        return true;
    }

    return errno == EPERM;
}

static bool simulator_espnow_open_bus(void)
{
    if (s_bus_ready) {
        return true;
    }

    if (simulator_bus_open() != 0) {
        fprintf(
            stderr,
            "\n[SIMULATOR] ESP-NOW Bus open failed\n"
        );

        return false;
    }

    s_bus_ready = true;
    return true;
}

static void simulator_master_publish(void)
{
    espnow_obd_packet_t packet;

    memset(&packet, 0, sizeof(packet));

    packet.magic = ESPNOW_PROTOCOL_MAGIC;
    packet.version = ESPNOW_PROTOCOL_VERSION;
    packet.flags = 0x01u;
    packet.seq = ++s_tx_seq;

    packet.rpm = obd_data_get_rpm();
    packet.speed = obd_data_get_speed();
    packet.sweep_step =
        (uint8_t)ui_sweep_get_step();
    packet.intro_step =
        (uint8_t)ui_intro_get_step();

    packet.coolant_temp =
        obd_data_get_coolant_temp();

    packet.intake_temp =
        obd_data_get_intake_temp();

    packet.oil_temp =
        obd_data_get_oil_temp();

    packet.oil_pressure_x10 =
        obd_data_get_oil_pressure_x10();

    packet.boost_x10 =
        obd_data_get_boost_x10();

    packet.brake_temp_x10 =
        obd_data_get_brake_temp_x10();

    packet.load_pct =
        obd_data_get_load_pct();

    packet.tps =
        obd_data_get_tps();

    packet.bat_mv =
        obd_data_get_bat_mv();

    strncpy(
        packet.name,
        "SIM-MASTER",
        ESPNOW_MASTER_NAME_LEN
    );

    uint64_t heartbeat_ms =
        simulator_monotonic_ms();

    if (
        simulator_bus_publish(
            &packet,
            (uint32_t)getpid(),
            s_master_session,
            heartbeat_ms
        ) != 0
    ) {
        fprintf(
            stderr,
            "\n[SIM BUS] publish failed\n"
        );

        return;
    }

    if ((packet.seq % 20u) == 0u) {
        fprintf(
            stderr,
            "\n[SIM BUS TX] session=%u seq=%u "
            "rpm=%u speed=%u coolant=%d\n",
            (unsigned int)s_master_session,
            (unsigned int)packet.seq,
            (unsigned int)packet.rpm,
            (unsigned int)packet.speed,
            (int)packet.coolant_temp
        );
    }
}

static void simulator_slave_apply(
    const espnow_obd_packet_t *packet
)
{
    obd_data_set_rpm(packet->rpm);
    obd_data_set_speed(packet->speed);

    obd_data_set_coolant_temp(
        packet->coolant_temp
    );

    obd_data_set_intake_temp(
        packet->intake_temp
    );

    obd_data_set_oil_temp(
        packet->oil_temp
    );

    obd_data_set_oil_pressure_x10(
        packet->oil_pressure_x10
    );

    obd_data_set_boost_x10(
        packet->boost_x10
    );

    obd_data_set_brake_temp_x10(
        packet->brake_temp_x10
    );

    obd_data_set_load_pct(
        packet->load_pct
    );

    obd_data_set_tps(packet->tps);
    obd_data_set_bat_mv(packet->bat_mv);

    ui_sweep_set_step(packet->sweep_step);
    ui_intro_set_step(packet->intro_step);

    memcpy(
        s_master_name,
        packet->name,
        ESPNOW_MASTER_NAME_LEN
    );

    s_master_name[
        ESPNOW_MASTER_NAME_LEN - 1u
    ] = '\0';

    s_have_slave_data = true;
    s_offline_reported = false;
    s_rx_age_ms = 0u;
}

static void simulator_slave_poll(void)
{
    simulator_bus_snapshot_t snapshot;

    int result =
        simulator_bus_read(&snapshot);

    if (result != 0) {
        return;
    }

    uint64_t now =
        simulator_monotonic_ms();

    if (
        now == 0u ||
        snapshot.heartbeat_ms > now ||
        now - snapshot.heartbeat_ms >=
            SIM_ESPNOW_TIMEOUT_MS
    ) {
        return;
    }

    if (
        !simulator_pid_is_alive(
            snapshot.master_pid
        )
    ) {
        return;
    }

    if (
        snapshot.master_session !=
        s_last_rx_session
    ) {
        fprintf(
            stderr,
            "\n[SIM BUS] new master session=%u "
            "pid=%u\n",
            (unsigned int)snapshot.master_session,
            (unsigned int)snapshot.master_pid
        );

        s_last_rx_session =
            snapshot.master_session;

        s_last_rx_seq = 0u;
        s_have_slave_data = false;
    }

    if (
        snapshot.packet.seq ==
        s_last_rx_seq
    ) {
        return;
    }

    s_last_rx_seq =
        snapshot.packet.seq;

    simulator_slave_apply(
        &snapshot.packet
    );

    if (
        (snapshot.packet.seq % 20u) ==
        0u
    ) {
        fprintf(
            stderr,
            "\n[SIM BUS RX] session=%u seq=%u "
            "rpm=%u speed=%u coolant=%d "
            "master=%s\n",
            (unsigned int)s_last_rx_session,
            (unsigned int)snapshot.packet.seq,
            (unsigned int)snapshot.packet.rpm,
            (unsigned int)snapshot.packet.speed,
            (int)snapshot.packet.coolant_temp,
            s_master_name
        );
    }
}

void espnow_link_start_master(void)
{
    if (!simulator_espnow_open_bus()) {
        return;
    }

    s_mode = SIM_ESPNOW_MASTER;
    s_master_session =
        simulator_generate_session();

    s_tx_seq = 0u;
    s_tx_elapsed_ms = 0u;

    fprintf(
        stderr,
        "\n[SIMULATOR] ESP-NOW master started "
        "session=%u pid=%u\n",
        (unsigned int)s_master_session,
        (unsigned int)getpid()
    );

    simulator_master_publish();
}

void espnow_link_start_slave(void)
{
    if (!simulator_espnow_open_bus()) {
        return;
    }

    s_mode = SIM_ESPNOW_SLAVE;
    s_have_slave_data = false;
    s_offline_reported = false;

    s_last_rx_session = 0u;
    s_last_rx_seq = 0u;
    s_rx_age_ms = UINT32_MAX;

    memset(
        s_master_name,
        0,
        sizeof(s_master_name)
    );

    fprintf(
        stderr,
        "\n[SIMULATOR] ESP-NOW slave waiting for master\n"
    );
}

void espnow_link_simulator_update(
    uint32_t elapsed_ms
)
{
    if (s_mode == SIM_ESPNOW_MASTER) {
        s_tx_elapsed_ms += elapsed_ms;

        if (
            s_tx_elapsed_ms >=
            SIM_ESPNOW_INTERVAL_MS
        ) {
            s_tx_elapsed_ms %=
                SIM_ESPNOW_INTERVAL_MS;

            simulator_master_publish();
        }

        return;
    }

    if (s_mode != SIM_ESPNOW_SLAVE) {
        return;
    }

    if (s_have_slave_data) {
        if (
            UINT32_MAX - s_rx_age_ms <
            elapsed_ms
        ) {
            s_rx_age_ms = UINT32_MAX;
        } else {
            s_rx_age_ms += elapsed_ms;
        }
    }

    simulator_slave_poll();

    if (
        s_have_slave_data &&
        s_rx_age_ms >= SIM_ESPNOW_TIMEOUT_MS &&
        !s_offline_reported
    ) {
        s_offline_reported = true;

        fprintf(
            stderr,
            "\n[SIM BUS] master offline "
            "(timeout=%u ms)\n",
            (unsigned int)SIM_ESPNOW_TIMEOUT_MS
        );
    }
}

bool espnow_link_slave_has_data(void)
{
    return
        s_mode == SIM_ESPNOW_SLAVE &&
        s_have_slave_data &&
        s_rx_age_ms < SIM_ESPNOW_TIMEOUT_MS;
}

uint8_t espnow_master_online_slaves(void)
{
    /*
     * 桌面三联模拟器固定启动两个从表。
     * Master 返回 2，使 RACE / AS / ONE 动画能够启动。
     */
    return s_mode == SIM_ESPNOW_MASTER ? 2u : 0u;
}

const char *espnow_link_get_master_name(void)
{
    if (!espnow_link_slave_has_data()) {
        return "";
    }

    return s_master_name;
}

void espnow_link_simulator_shutdown(void)
{
    if (
        s_bus_ready &&
        s_mode == SIM_ESPNOW_MASTER
    ) {
        simulator_bus_clear_master(
            (uint32_t)getpid(),
            s_master_session
        );
    }

    if (s_bus_ready) {
        simulator_bus_close();
    }

    s_bus_ready = false;
    s_mode = SIM_ESPNOW_STOPPED;
    s_master_session = 0u;
}
