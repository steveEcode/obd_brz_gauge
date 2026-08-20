#pragma once
#include <stdint.h>
#include <stdbool.h>

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

// Stop ESP-NOW and release WiFi resources (for OTA mode).
// After calling this, espnow_link_start_master/slave can be called again to restart.
void espnow_link_stop(void);

// Slave: whether master data was received within the last ~2s (for the "waiting for master" hint).
bool espnow_link_slave_has_data(void);

// Slave: name of the last master heard (empty string = none yet).
const char *espnow_link_get_master_name(void);

// Slave: MAC of the currently bound master (returns a 6-byte array; all-zero = unbound).
const uint8_t *espnow_link_get_bound_master_mac(void);

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

#ifdef __cplusplus
}
#endif
