#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "signal_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * signal_store - the single runtime value table (contracts/signals.json).
 *
 * Slots:
 *   0 .. SIG_STD_COUNT-1          obd.* and sys.* (SIG_* from signal_registry.h)
 *   SIG_VEH_BASE .. +SIG_VEH_MAX  veh.* registered by the active vehicle profile
 *
 * Values are int32 internal fixed point (registry `scale`). A sample outside the
 * signal's [min, max] is stored as invalid. A value reads as valid only when the
 * valid bit is set, it is not stale (now - ts < stale_ms) and, for obd.* and
 * veh.*, sys.obd_link == 2.
 *
 * The table lives in PSRAM; every call is safe from any task.
 * -------------------------------------------------------------------------- */

#define SIG_VEH_BASE     SIG_STD_COUNT
#define SIG_VEH_MAX      32
#define SIG_SLOT_COUNT   (SIG_VEH_BASE + SIG_VEH_MAX)
#define SIG_VEH_ID_BASE  0x0100
#define SIG_SLOT_NONE    (-1)

#define SIG_LINK_NONE    0   /* sys.obd_link values */
#define SIG_LINK_NO_DATA 1
#define SIG_LINK_DATA    2

typedef struct {
    int32_t  value;   /* last written value (kept when invalidated) */
    uint32_t ts_ms;   /* esp_timer ms of the last valid write */
    uint16_t seq;     /* bumped on each write and effective invalidation */
    bool     valid;   /* raw valid bit (not stale-checked) */
} signal_entry_t;

/* Allocate the table (PSRAM) and mark every slot invalid. Call once, early. */
void signal_store_init(void);

/* Write an internal value. Out of [min, max] stores invalid; returns validity. */
bool signal_store_set(int slot, int32_t internal);
/* Apply ordered writes under one timestamp and critical section; local slots are skipped. */
void signal_store_set_many(const int *slots, const int32_t *values, size_t count);
/* Invalidate only if currently valid; repeated invalidations are no-ops. */
void signal_store_invalidate(int slot);
/* Invalidate ordered slots atomically, skipping local or already-invalid entries. */
void signal_store_invalidate_many(const int *slots, size_t count);
/* Invalidate every obd.* and veh.* slot (link loss, profile switch). */
void signal_store_invalidate_vehicle(void);
/* Mark an obd.* slot as fed by an on-board sensor (ADS1115 oil pressure): it
 * stays valid without an OBD link and survives invalidate_vehicle(). */
void signal_store_set_local(int slot, bool local);
bool signal_store_is_local(int slot);

/* Fresh valid value in internal units; false (out untouched) otherwise. */
bool signal_store_get(int slot, int32_t *out);
/* Read up to 64 slots against one timestamp and under one lock. Returns bit i
 * for each fresh valid slot; invalid values are written as zero. */
uint64_t signal_store_get_many(const int *slots, size_t count, int32_t *values);
/* Read/unit-convert requested slots with one timestamp/critical section.
 * `values` is indexed by slot; requested stale/invalid slots receive NAN. */
uint64_t signal_store_get_delivered_mask(uint64_t requested, double values[SIG_SLOT_COUNT]);
/* Raw entry, no freshness checks. */
void signal_store_entry(int slot, signal_entry_t *out);
/* Lookup by key ("obd.rpm", "veh.knock") or numeric id; SIG_SLOT_NONE if absent. */
int         signal_store_slot_by_key(const char *key);
int         signal_store_slot_by_id(uint16_t num_id);
bool        signal_store_copy_key(int slot, char *out, size_t out_size);
uint16_t    signal_store_num_id(int slot);

/* veh.* registration (profile load). num_id = SIG_VEH_ID_BASE + index, assigned
 * in call order. Re-registering an existing key returns its slot. Returns the
 * slot or SIG_SLOT_NONE when full / key invalid. */
int  signal_store_veh_register(const char *key, int32_t min, int32_t max, uint32_t stale_ms);
void signal_store_veh_clear(void);
int  signal_store_veh_count(void);

/* Sky Hub owns only present, range-valid coolant/oil-temperature/oil-pressure
 * fields. Hub oil pressure accepts -100..2000 (bar*100), preserving vacuum;
 * ordinary OBD bounds are unchanged. values order: C, C, bar*100.
 * mask bits: 0,1,2 respectively. */
void signal_store_hub_apply(uint8_t mask, const int32_t values[3]);
uint16_t signal_store_hub_obd_mask(void);
/* Atomic snapshot for master relay: fresh owned fields only, same bit order. */
uint8_t signal_store_hub_snapshot(int32_t values[3]);

#ifdef __cplusplus
}
#endif
