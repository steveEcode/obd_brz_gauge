#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default target: name "OBDII", service UUID 0xFFF0, write characteristic 0xFFF1,
// notify characteristic prefers 0xFFF2 (falls back to 0xFFF1 if absent).
     // 010C - engine RPM: 41 0C AA BB      -> RPM = ((AA*256)+BB)/4
        // 010D - vehicle speed: 41 0D AA     -> SPEED = AA km/h
        // 0105 - engine coolant temp: 41 05 AA -> TEMP = AA - 40 °C
        // 010F - intake air temp: 41 0F AA      -> TEMP = AA - 40 °C
        // 010B - manifold absolute pressure: 41 0B AA -> PRESSURE = AA kPa
        // 0111 - throttle position: 41 11 AA    -> POSITION = (AA*100)/255 %
        // 012F - fuel level: 41 2F AA           -> LEVEL = (AA*100)/255 %
        // 0142 - control module voltage: 41 42 AA BB -> VOLTAGE = ((AA*256)+BB)/1000 V

typedef struct {
    void (*on_connected)(void);
    void (*on_disconnected)(void);
    void (*on_raw_notify)(const uint8_t *data, size_t len);
    void (*on_parsed_rpm)(uint16_t rpm);// engine RPM
    void (*on_parsed_speed_kmh)(uint8_t kmh);// vehicle speed
    void (*on_parsed_coolant_temp)(uint32_t coolant_temp);// engine coolant temperature
    void (*on_parsed_intake_temp)(uint32_t intake_temp);// intake air temperature
    void (*on_parsed_oil_temp)(uint32_t oil_temp);  // oil temperature °C (formula already applied)
    void (*on_parsed_load_pct)(uint32_t load_pct);   // engine load 0~100%
    void (*on_parsed_manifold_pressure)(uint32_t manifold_pressure);// manifold absolute pressure
    void (*on_parsed_throttle_position)(uint32_t throttle_position);// throttle position
    void (*on_parsed_gear)(int8_t gear);  // direct gear (CAN broadcast)
    void (*on_parsed_fuel_level)(uint32_t fuel_level);// fuel level
    void (*on_parsed_control_module_voltage)(uint32_t control_module_voltage);// control module voltage
    void (*on_parsed_afr)(uint32_t afr_x100);    // air-fuel ratio AFR, x100 (1470=14.7:1)
    void (*on_parsed_oil_pressure)(uint32_t oil_pressure_hpa);    // engine oil pressure (absolute hPa, Mode 22 DID 4436)
    void (*on_parsed_obd_gear)(uint8_t raw_gear);  // direct gear number from Mode 22 DID (0=N, 1..8=forward); BMW 22 D0 31

} elm327_ble_callbacks_t;

// Initialize the BLE client and start scanning/connecting.
// target_name may be NULL to use the default "OBDII".
void elm327_ble_init_and_start(const char *target_name, const elm327_ble_callbacks_t *cbs);

// Only initialize the BT controller + Bluedroid + GAP/GATTC callbacks (idempotent, safe to call
// repeatedly); does not start any ELM327 scan/connection. For cases that have no OBD device yet
// still need BLE peripheral advertising (e.g. RaceChrono DIY / SkyGauge pairing broadcast).
void elm327_ble_ensure_stack_init(void);

// Send an OBD command (e.g. convert "01 0C\r" to bytes, then call this function).
bool elm327_ble_send_command(const uint8_t *data, size_t len);

// Helper: convert an ASCII command like "01 0C\r" into bytes (spaces optional).
// Returns the number of bytes written; out_buf_len is the capacity of out_buf.
size_t elm327_ble_ascii_cmd_to_bytes(const char *ascii, uint8_t *out_buf, size_t out_buf_len);

// Convenience entry point that starts default logging callbacks and periodic polling (010C/010D).
// If mac is non-NULL and non-zero, connect by exact MAC match (ignoring same-name devices);
// otherwise fall back to fuzzy target_name matching (backward compatible).
void elm327_ble_start_default(const char *target_name, const uint8_t mac[6]);

// ---- BLE scan-mode API ----
#define BLE_SCAN_MAX_DEVICES 20

typedef struct {
    char name[32];
    uint8_t addr[6];
    int rssi;
} ble_scan_result_t;

// Callback when a device is found (invoked on the BT callback thread; make UI updates thread-safe).
typedef void (*ble_scan_found_cb_t)(const ble_scan_result_t *dev, int total_count);

// Start scanning (scan only, do not connect). duration_s: scan duration in seconds. cb: called for each new device.
void elm327_ble_scan_only_start(int duration_s, ble_scan_found_cb_t cb);

// Stop scanning.
void elm327_ble_scan_only_stop(void);

// Connect to the device with the given MAC (call after stopping the scan): exact match, so a
// same-name device cannot hijack the connection. name is only for logging/UI display.
void elm327_ble_connect_by_addr(const uint8_t mac[6], const char *name);

// Query the current connection state.
bool elm327_ble_is_connected(void);
void elm327_ble_disconnect(void);

// WiFi OTA pause/resume: drop the ELM327 link and suppress auto-reconnect +
// polling during OTA, so the SoftAP gets the full 2.4GHz radio; re-arm
// auto-reconnect on exit. Call from the OTA-mode screen, not the BLE OTA path.
void elm327_ble_pause_for_ota(void);
void elm327_ble_resume_after_ota(void);
// Re-enable BLE without auto-connecting. Used when the user opens the manual
// source/device selector after wired CAN had paused the Bluetooth stack.
void elm327_ble_resume_for_source_selection(void);

// Get the currently connected / target device name.
const char *elm327_ble_get_connected_name(void);

// ---- Oil temperature calibration API ----
// Set the oil-temp offset (calibration compensation), in °C.
// Example: actual oil temp is 90°C but the reading shows 92°C -> set offset = -2.
void elm327_oil_temp_set_offset(int8_t offset_c);

// Get the current oil-temp offset.
int8_t elm327_oil_temp_get_offset(void);

// Get oil-temp diagnostics (for UI display or debugging).
typedef struct {
    uint32_t mode0_ok;    // Mode 01 5C success count
    uint32_t mode1_ok;    // Mode 22 10 17 success count
    uint32_t mode2_ok;    // Mode 21 01 success count
    uint32_t mode0_fail;  // failure count
    uint32_t mode1_fail;
    uint32_t mode2_fail;
    int16_t last_raw;     // last raw reading
    int16_t last_filtered; // last filtered reading
    uint8_t current_mode; // current query mode (0-2)
} elm327_oil_diag_t;

void elm327_oil_temp_get_diag(elm327_oil_diag_t *out);

#ifdef __cplusplus
}
#endif
