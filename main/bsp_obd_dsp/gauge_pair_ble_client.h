#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Triple-gauge BLE pairing protocol constants: shared between the master gauge (a separate
// pairing-service attribute table in racechrono_ble_diy.c, sharing the same advertising as
// the RaceChrono service but each being an independent GATT Primary Service) and this file
// (the slave-gauge client).
#define GAUGE_PAIR_SERVICE_UUID 0x1FF9
#define GAUGE_PAIR_CHAR_MAC     0x0003

#define GAUGE_PAIR_SCAN_MAX_DEVICES 16

typedef struct {
    char    name[32];
    uint8_t addr[6];
    int     rssi;
} gauge_pair_scan_result_t;

// Called once for each newly discovered device (advertising name starts with "SkyGauge")
typedef void (*gauge_pair_scan_cb_t)(const gauge_pair_scan_result_t *dev, int total_count);

// Pairing result callback; name/mac are valid when success=true
typedef void (*gauge_pair_result_cb_t)(bool success, const char *name, const uint8_t mac[6]);

// Slave gauge: scan for nearby master gauge devices advertising the "SkyGauge" prefix (for duration_s seconds)
void gauge_pair_ble_scan_start(int duration_s, gauge_pair_scan_cb_t cb);
void gauge_pair_ble_scan_stop(void);

// Slave gauge: connect to the selected master gauge device, read its pairing characteristic
// (this device's ESP-NOW MAC), and disconnect automatically after reading.
// The result (success/failure) is reported once via cb; the caller handles NVS persistence
// and UI navigation itself.
void gauge_pair_ble_connect(const uint8_t addr[6], const char *name, gauge_pair_result_cb_t cb);

#ifdef __cplusplus
}
#endif
