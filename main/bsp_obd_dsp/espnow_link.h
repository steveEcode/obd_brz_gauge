#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "espnow_sources.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_ROLE_MASTER    0   // reads ELM327 + broadcasts to slaves over ESP-NOW (WiFi on)
#define ESPNOW_ROLE_SLAVE     1   // only receives the master's ESP-NOW data and displays it (WiFi on)
#define ESPNOW_ROLE_STANDALONE 2  // standalone: full ELM327 features but no WiFi/ESP-NOW (default for new devices)

// Master: init WiFi + ESP-NOW and periodically broadcast the OBD data cache to slaves.
// Runs alongside BLE (ELM327 link); ESP32-S3 single-radio time-shares (software coexistence enabled in sdkconfig).
void espnow_link_start_master(void);

// Slave: init WiFi + ESP-NOW, receive the master's broadcast and write it into the local OBD data cache (no ELM327 link).
void espnow_link_start_slave(void);

// Replace all shareable source subscriptions for the local page. System sources
// remain device-local. Online slave demands expire after two seconds.
void espnow_link_set_data_demand(const espnow_demand_t *demand);
espnow_demand_t espnow_link_remote_data_demand(void);
// Delivered four-wheel data: local on master/standalone, master snapshot on
// slave (no local fallback, no slave NVS pairing changes).
void espnow_link_tpms_snapshot(tpms_wheel_data_t out[TPMS_WHEEL_COUNT]);

// Publish the local page's OBD demand mask. Slaves advertise it in HELLO;
// masters retain their own mask and merge masks from online slaves.
void espnow_link_set_demand_mask(uint16_t mask);
// Demand bits requested by online slaves (master only); zero on a standalone
// or slave gauge. The caller can OR this into the local ELM demand mask.
uint16_t espnow_link_remote_demand_mask(void);

// Pause ESP-NOW traffic for OTA, but keep the WiFi driver initialized so the
// OTA SoftAP can reuse it. The normal OTA flow reboots after completion/exit.
// false: worker is still exiting or deinit failed; retry before starting OTA.
bool espnow_link_pause_for_ota(void);

// Slave: whether master data was received within the last ~2s (for the "waiting for master" hint).
bool espnow_link_slave_has_data(void);

// Slave: copy a synchronized, NUL-terminated name (empty = none heard yet).
void espnow_link_copy_master_name(char *out, size_t capacity);

// Slave: copy the currently bound master MAC to out (all-zero = unbound).
void espnow_link_get_bound_master_mac(uint8_t out[6]);

// Slave: bind to a specific master MAC (the ESP-NOW MAC read during BLE pairing).
void espnow_link_bind_master(const uint8_t mac[6]);

// Slave: unbind the master (return to accept-any mode).
void espnow_link_unbind_master(void);

// Master: number of slaves currently online (used to detect multi-gauge / whether to play the boot animation).
uint8_t espnow_master_online_slaves(void);

// ---- Multi-gauge linked-flash sync ----
// Trigger the linked test: the master injects a simulated RPM ramp into the RPM override layer
// (the broadcast carries it to the slaves); a slave sends a TEST request to the master, which
// drives it centrally so all gauges stay in sync.
void espnow_link_trigger_linked_test(void);

// The local user changed the RPM threshold -> broadcast it to the other gauges
// (relayed through the master so every slave is reached).
void espnow_link_broadcast_threshold(uint16_t thresh);

// Apply a threshold synced from another gauge: write it to local NVS; the master additionally
// relays it to the other slaves. Called by the UI task on APP_EVT_ESPNOW_THRESH_SYNC
// (avoids writing flash from inside the recv callback).
void espnow_link_apply_synced_threshold(uint16_t thresh);

// Whether a linked test is in progress (master = ramping, slave = flag received from the master).
// During TEST every gauge is forced to take part in the rendering.
bool espnow_link_linktest_active(void);

// Whether rpm_warn_linked_en is active: master = local NVS, slave = mirrored from broadcast flags bit2.
// Slaves use this so the gradient fires without needing manual NVS config on each slave unit.
bool espnow_link_linked_en(void);

// Same query with the caller's already-read local setting. This avoids an extra
// NVS snapshot in high-frequency UI paths; the legacy wrapper remains for
// callers that do not already have a configuration snapshot.
bool espnow_link_linked_en_with_local(bool local_enabled);

#ifdef __cplusplus
}
#endif
