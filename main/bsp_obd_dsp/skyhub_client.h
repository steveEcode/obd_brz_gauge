#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "skyhub_protocol.h"
#define SKYHUB_SCAN_MAX 8

typedef struct { uint8_t mac[6], owner[6], mask; bool busy; } skyhub_candidate_t;
typedef struct {
    bool available, scanning, pairing, online;
    uint8_t bound_mac[6], active_mask;
    size_t count;
    skyhub_candidate_t candidates[SKYHUB_SCAN_MAX];
    char status[48];
} skyhub_status_t;

/* ESP-NOW must already be initialized with the broadcast peer on channel 1.
 * The shared radio callback ONLY enqueues; this worker owns all NVS/data writes. */
void skyhub_client_start(void);
bool skyhub_client_pause(void);  /* request stop; true only after worker exits */
bool skyhub_client_receive(const uint8_t mac[6], const uint8_t *data, size_t len);
bool skyhub_client_scan(void);
bool skyhub_client_bind(const uint8_t mac[6]);
bool skyhub_client_unbind(void);
void skyhub_client_snapshot(skyhub_status_t *out);
