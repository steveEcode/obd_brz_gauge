#include "bsp_obd_dsp/elm327_ble_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool s_connected = true;
static bool s_scanning = false;
static char s_device_name[32] = "SIM-OBDII";

void elm327_ble_scan_only_start(
    int duration_s,
    ble_scan_found_cb_t callback
)
{
    (void)duration_s;

    s_scanning = true;

    fprintf(
        stderr,
        "\n[SIMULATOR] BLE scan started\n"
    );

    if (callback != NULL) {
        ble_scan_result_t device = {
            .name = "SIM-OBDII",
            .addr = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
            .rssi = -42,
        };

        callback(&device, 1);
    }
}

void elm327_ble_scan_only_stop(void)
{
    s_scanning = false;

    fprintf(
        stderr,
        "\n[SIMULATOR] BLE scan stopped\n"
    );
}

void elm327_ble_connect_by_name(const char *name)
{
    if (name != NULL && name[0] != '\0') {
        strncpy(
            s_device_name,
            name,
            sizeof(s_device_name) - 1
        );

        s_device_name[sizeof(s_device_name) - 1] = '\0';
    }

    s_connected = true;
    s_scanning = false;

    fprintf(
        stderr,
        "\n[SIMULATOR] BLE connected: %s\n",
        s_device_name
    );
}

bool elm327_ble_is_connected(void)
{
    return s_connected;
}

void elm327_ble_disconnect(void)
{
    s_connected = false;

    fprintf(
        stderr,
        "\n[SIMULATOR] BLE disconnected\n"
    );
}

const char *elm327_ble_get_connected_name(void)
{
    return s_connected ? s_device_name : "";
}
