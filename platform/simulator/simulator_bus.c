#include "simulator_bus.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIMULATOR_BUS_NAME "/obd_brz_gauge_bus"
#define SIMULATOR_BUS_MAGIC 0x42525A47u
#define SIMULATOR_BUS_VERSION 3u

static int s_bus_fd = -1;
static simulator_bus_state_t *s_bus_state;

static uint32_t simulator_bus_begin_write(void)
{
    uint32_t sequence = __atomic_load_n(
        &s_bus_state->sequence,
        __ATOMIC_SEQ_CST
    );

    /*
     * 如果旧进程异常退出时留下奇数序号，
     * 先恢复到可继续写入的偶数。
     */
    if ((sequence & 1u) != 0u) {
        sequence++;
    }

    __atomic_store_n(
        &s_bus_state->sequence,
        sequence + 1u,
        __ATOMIC_SEQ_CST
    );

    return sequence;
}

static void simulator_bus_end_write(
    uint32_t sequence
)
{
    __atomic_store_n(
        &s_bus_state->sequence,
        sequence + 2u,
        __ATOMIC_SEQ_CST
    );
}

int simulator_bus_open(void)
{
    s_bus_fd = shm_open(
        SIMULATOR_BUS_NAME,
        O_CREAT | O_RDWR,
        0666
    );

    if (s_bus_fd < 0) {
        fprintf(
            stderr,
            "[SIM BUS] shm_open failed: %s\n",
            strerror(errno)
        );
        return -1;
    }

    if (
        ftruncate(
            s_bus_fd,
            (off_t)sizeof(simulator_bus_state_t)
        ) != 0
    ) {
        fprintf(
            stderr,
            "[SIM BUS] ftruncate failed: %s\n",
            strerror(errno)
        );

        close(s_bus_fd);
        s_bus_fd = -1;
        return -1;
    }

    void *mapping = mmap(
        NULL,
        sizeof(simulator_bus_state_t),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        s_bus_fd,
        0
    );

    if (mapping == MAP_FAILED) {
        fprintf(
            stderr,
            "[SIM BUS] mmap failed: %s\n",
            strerror(errno)
        );

        close(s_bus_fd);
        s_bus_fd = -1;
        return -1;
    }

    s_bus_state = mapping;

    if (
        s_bus_state->magic != SIMULATOR_BUS_MAGIC ||
        s_bus_state->version != SIMULATOR_BUS_VERSION
    ) {
        memset(
            s_bus_state,
            0,
            sizeof(*s_bus_state)
        );

        s_bus_state->magic = SIMULATOR_BUS_MAGIC;
        s_bus_state->version = SIMULATOR_BUS_VERSION;
    }

    return 0;
}

void simulator_bus_close(void)
{
    if (s_bus_state != NULL) {
        munmap(
            s_bus_state,
            sizeof(simulator_bus_state_t)
        );

        s_bus_state = NULL;
    }

    if (s_bus_fd >= 0) {
        close(s_bus_fd);
        s_bus_fd = -1;
    }
}

simulator_bus_state_t *simulator_bus_get_state(void)
{
    return s_bus_state;
}

int simulator_bus_publish(
    const espnow_obd_packet_t *packet,
    uint32_t master_pid,
    uint32_t master_session,
    uint64_t heartbeat_ms
)
{
    if (
        s_bus_state == NULL ||
        packet == NULL ||
        master_pid == 0u ||
        master_session == 0u ||
        heartbeat_ms == 0u
    ) {
        return -1;
    }

    if (
        packet->magic != ESPNOW_PROTOCOL_MAGIC ||
        packet->version != ESPNOW_PROTOCOL_VERSION
    ) {
        return -1;
    }

    uint32_t sequence =
        simulator_bus_begin_write();

    s_bus_state->master_pid =
        master_pid;

    s_bus_state->master_session =
        master_session;

    s_bus_state->heartbeat_ms =
        heartbeat_ms;

    memcpy(
        &s_bus_state->packet,
        packet,
        sizeof(*packet)
    );

    simulator_bus_end_write(sequence);

    return 0;
}

int simulator_bus_read(
    simulator_bus_snapshot_t *out_snapshot
)
{
    if (
        s_bus_state == NULL ||
        out_snapshot == NULL
    ) {
        return -1;
    }

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t before = __atomic_load_n(
            &s_bus_state->sequence,
            __ATOMIC_SEQ_CST
        );

        if ((before & 1u) != 0u) {
            continue;
        }

        simulator_bus_snapshot_t snapshot;

        snapshot.master_pid =
            s_bus_state->master_pid;

        snapshot.master_session =
            s_bus_state->master_session;

        snapshot.heartbeat_ms =
            s_bus_state->heartbeat_ms;

        memcpy(
            &snapshot.packet,
            &s_bus_state->packet,
            sizeof(snapshot.packet)
        );

        uint32_t after = __atomic_load_n(
            &s_bus_state->sequence,
            __ATOMIC_SEQ_CST
        );

        if (
            before != after ||
            (after & 1u) != 0u
        ) {
            continue;
        }

        if (
            snapshot.master_pid == 0u ||
            snapshot.master_session == 0u ||
            snapshot.heartbeat_ms == 0u
        ) {
            return 1;
        }

        if (
            snapshot.packet.magic !=
                ESPNOW_PROTOCOL_MAGIC ||
            snapshot.packet.version !=
                ESPNOW_PROTOCOL_VERSION
        ) {
            return 1;
        }

        *out_snapshot = snapshot;
        return 0;
    }

    return -1;
}

void simulator_bus_clear_master(
    uint32_t master_pid,
    uint32_t master_session
)
{
    if (
        s_bus_state == NULL ||
        master_pid == 0u ||
        master_session == 0u
    ) {
        return;
    }

    uint32_t sequence =
        simulator_bus_begin_write();

    if (
        s_bus_state->master_pid ==
            master_pid &&
        s_bus_state->master_session ==
            master_session
    ) {
        s_bus_state->master_pid = 0u;
        s_bus_state->master_session = 0u;
        s_bus_state->heartbeat_ms = 0u;

        memset(
            &s_bus_state->packet,
            0,
            sizeof(s_bus_state->packet)
        );
    }

    simulator_bus_end_write(sequence);
}
