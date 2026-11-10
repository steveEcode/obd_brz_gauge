#pragma once
/*
 * elm327_ble_client - BLE link to an ELM327-compatible OBD adapter.
 *
 * Owns the BLE transport (GATTC app BLE_HUB_APP_ELM327 via ble_hub: scan,
 * connect, service discovery, notify subscription) and the single "obd_poll"
 * task that drives vp_engine with the active vehicle profile
 * (vp_manager_active()). Adapter service preference: 0xFFF0 > 0x18F0 >
 * 0xFF12 > any characteristic with WRITE / NOTIFY.
 *
 * Auto-connect is by exact MAC only (no name matching): a bound adapter is
 * rescanned for and reconnected after every drop. Scan callbacks run on the
 * Bluedroid task; UI code must take the LVGL lock itself.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register with the BLE stack (ble_hub) without scanning or connecting.
// Idempotent. Lets the scan page work before any adapter is bound.
void elm327_ble_ensure_stack_init(void);

// Start the OBD client: bind the adapter MAC (all-zero / NULL = none bound,
// no auto-connect), start the poll task and auto-connect.
// target_name is for display only (NULL or "" keeps "OBDII").
void elm327_ble_start_default(const char *target_name, const uint8_t mac[6]);

// ---- BLE scan-mode API ----
#define BLE_SCAN_MAX_DEVICES 20

typedef struct {
    char name[32];
    uint8_t addr[6];
    int rssi;
} ble_scan_result_t;

// Called for each new named device (BT callback thread; make UI updates thread-safe).
typedef void (*ble_scan_found_cb_t)(const ble_scan_result_t *dev, int total_count);

// List nearby named devices for duration_s seconds (no connect).
void elm327_ble_scan_only_start(int duration_s, ble_scan_found_cb_t cb);

// Temporarily yield reconnection scans while another pairing page uses BLE.
// Keeps an established OBD connection and polling alive.
void elm327_ble_yield_scanner(bool yield);

// Stop the device-list scan.
void elm327_ble_scan_only_stop(void);

// Bind mac as the adapter and connect to it (exact MAC match, so a same-name
// device cannot hijack the connection). name is for display only.
void elm327_ble_connect_by_addr(const uint8_t mac[6], const char *name);

// Adapter link state.
bool elm327_ble_is_connected(void);

// Limit standard OBD polling to signals currently required by the UI. RPM is
// always polled by the engine; profile veh.* signals remain enabled for slaves.
void elm327_ble_set_obd_demand_mask(uint16_t mask);

// Unbind the adapter and drop the link (no auto-reconnect until the next
// elm327_ble_connect_by_addr() or reboot).
void elm327_ble_disconnect(void);

// WiFi OTA: drop the adapter link and stop scanning / polling so the SoftAP
// gets the whole 2.4 GHz radio. One-way: OTA mode ends with a reboot.
void elm327_ble_pause_for_ota(void);

// Copy a synchronized, NUL-terminated adapter name ("OBDII" by default).
void elm327_ble_copy_connected_name(char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
