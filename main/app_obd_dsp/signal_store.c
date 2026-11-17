#include "signal_store.h"
#include "signal_delivery.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "signal_store";

#define VEH_KEY_MAX 32

typedef struct {
    char     key[VEH_KEY_MAX];
    int32_t  min;
    int32_t  max;
    uint32_t stale_ms;
} veh_def_t;

typedef struct {
    signal_entry_t e[SIG_SLOT_COUNT];
    veh_def_t      veh[SIG_VEH_MAX];
    int            n_veh;
    uint64_t       hub_mask;     /* external accessory ownership, separate from board sensors */
    uint64_t       local_mask;   /* slots fed by on-board sensors: not gated by obd_link */
} store_t;

_Static_assert(SIG_SLOT_COUNT <= 64, "valid mask is 64 bits");

static store_t     *s_st;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline bool slot_ok(int slot)
{
    return s_st && slot >= 0 && slot < SIG_SLOT_COUNT;
}

static inline bool is_vehicle_slot(int slot)
{
    return (slot >= SIG_OBD_FIRST && slot < SIG_OBD_FIRST + SIG_OBD_COUNT) || slot >= SIG_VEH_BASE;
}

static inline void bounds_of(int slot, int32_t *min, int32_t *max)
{
    if (slot < SIG_STD_COUNT) {
        *min = sig_registry[slot].min;
        *max = sig_registry[slot].max;
    } else {
        const veh_def_t *d = &s_st->veh[slot - SIG_VEH_BASE];
        *min = d->min;
        *max = d->max;
    }
}

static inline uint32_t stale_of(int slot)
{
    return slot < SIG_STD_COUNT ? sig_registry[slot].stale_ms
                                : s_st->veh[slot - SIG_VEH_BASE].stale_ms;
}

typedef struct {
    uint64_t local_mask;
    int      n_veh;
    bool     link_has_data;
} fresh_snapshot_t;

/* Caller holds s_mux. The metadata is shared by every slot in one read. */
static inline fresh_snapshot_t fresh_snapshot_locked(void)
{
    const signal_entry_t *link = &s_st->e[SIG_SYS_OBD_LINK];
    return (fresh_snapshot_t){
        .local_mask = s_st->local_mask | s_st->hub_mask,
        .n_veh = s_st->n_veh,
        .link_has_data = link->valid && link->value >= SIG_LINK_DATA,
    };
}

/* Caller holds s_mux. Single-value reads avoid building a batch snapshot. */
static inline bool fresh_locked(int slot, uint32_t now)
{
    const signal_entry_t *e = &s_st->e[slot];
    if (!e->valid) return false;
    if (slot >= SIG_VEH_BASE && slot - SIG_VEH_BASE >= s_st->n_veh) return false;
    if (is_vehicle_slot(slot) && !((s_st->local_mask | s_st->hub_mask) & (1ULL << slot))) {
        const signal_entry_t *link = &s_st->e[SIG_SYS_OBD_LINK];
        if (!link->valid || link->value < SIG_LINK_DATA) return false;
    }
    const uint32_t stale = stale_of(slot);
    return stale == 0 || (uint32_t)(now - e->ts_ms) < stale;
}

/* Caller holds s_mux. */
static inline bool fresh_snapshot_valid(int slot, uint32_t now,
                                        const fresh_snapshot_t *snapshot)
{
    const signal_entry_t *e = &s_st->e[slot];
    if (!e->valid) return false;
    if (slot >= SIG_VEH_BASE && slot - SIG_VEH_BASE >= snapshot->n_veh) return false;
    if (is_vehicle_slot(slot) && !(snapshot->local_mask & (1ULL << slot)) &&
        !snapshot->link_has_data) return false;
    const uint32_t stale = stale_of(slot);
    return stale == 0 || (uint32_t)(now - e->ts_ms) < stale;
}

void signal_store_init(void)
{
    if (s_st) return;
    s_st = heap_caps_calloc(1, sizeof(store_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_st) {
        ESP_LOGW(TAG, "PSRAM alloc failed, using internal RAM (%u B)", (unsigned)sizeof(store_t));
        s_st = calloc(1, sizeof(store_t));
    }
    if (!s_st) {
        ESP_LOGE(TAG, "out of memory");
    }
}

void signal_store_set_local(int slot, bool local)
{
    if (!slot_ok(slot)) return;
    portENTER_CRITICAL(&s_mux);
    if (local) s_st->local_mask |= 1ULL << slot;
    else       s_st->local_mask &= ~(1ULL << slot);
    portEXIT_CRITICAL(&s_mux);
}

bool signal_store_is_local(int slot)
{
    if (!slot_ok(slot)) return false;
    portENTER_CRITICAL(&s_mux);
    const bool local = ((s_st->local_mask | s_st->hub_mask) & (1ULL << slot)) != 0;
    portEXIT_CRITICAL(&s_mux);
    return local;
}

bool signal_store_set(int slot, int32_t internal)
{
    if (!slot_ok(slot)) return false;
    bool ok;
    portENTER_CRITICAL(&s_mux);
    if (s_st->hub_mask & (1ULL << slot)) {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    uint32_t t = now_ms();
    int32_t mn, mx;
    bounds_of(slot, &mn, &mx);
    ok = internal >= mn && internal <= mx;
    signal_entry_t *e = &s_st->e[slot];
    e->value = internal;
    e->valid = ok;
    if (ok) e->ts_ms = t;
    e->seq++;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void signal_store_set_many(const int *slots, const int32_t *values, size_t count)
{
    if (!slots || !values || count == 0 || !s_st) return;
    portENTER_CRITICAL(&s_mux);
    uint32_t t = now_ms();
    for (size_t i = 0; i < count; i++) {
        int slot = slots[i];
        if (!slot_ok(slot) || ((s_st->local_mask | s_st->hub_mask) & (1ULL << slot))) continue;
        int32_t mn, mx;
        bounds_of(slot, &mn, &mx);
        bool ok = values[i] >= mn && values[i] <= mx;
        signal_entry_t *e = &s_st->e[slot];
        e->value = values[i];
        e->valid = ok;
        if (ok) e->ts_ms = t;
        e->seq++;
    }
    portEXIT_CRITICAL(&s_mux);
}

void signal_store_invalidate(int slot)
{
    if (!slot_ok(slot)) return;
    portENTER_CRITICAL(&s_mux);
    if (s_st->hub_mask & (1ULL << slot)) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    signal_entry_t *e = &s_st->e[slot];
    if (e->valid) {
        e->valid = false;
        e->seq++;
    }
    portEXIT_CRITICAL(&s_mux);
}

void signal_store_invalidate_many(const int *slots, size_t count)
{
    if (!slots || count == 0 || !s_st) return;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < count; i++) {
        int slot = slots[i];
        if (!slot_ok(slot) || ((s_st->local_mask | s_st->hub_mask) & (1ULL << slot))) continue;
        signal_entry_t *e = &s_st->e[slot];
        if (!e->valid) continue;
        e->valid = false;
        e->seq++;
    }
    portEXIT_CRITICAL(&s_mux);
}

void signal_store_invalidate_vehicle(void)
{
    if (!s_st) return;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SIG_SLOT_COUNT; i++) {
        if (!is_vehicle_slot(i) || ((s_st->local_mask | s_st->hub_mask) & (1ULL << i))) continue;
        signal_entry_t *e = &s_st->e[i];
        if (!e->valid) continue;
        e->valid = false;
        e->seq++;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool signal_store_get(int slot, int32_t *out)
{
    if (!slot_ok(slot)) return false;
    bool ok;
    portENTER_CRITICAL(&s_mux);
    uint32_t t = now_ms();
    ok = fresh_locked(slot, t);
    if (ok && out) *out = s_st->e[slot].value;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

uint64_t signal_store_get_many(const int *slots, size_t count, int32_t *values)
{
    if (!slots || !values || count == 0 || count > 64) return 0;
    for (size_t i = 0; i < count; i++) values[i] = 0;
    if (!s_st) return 0;

    uint64_t valid = 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t t = now_ms();
    if (count == 1) {
        const int slot = slots[0];
        if (slot >= 0 && slot < SIG_SLOT_COUNT && fresh_locked(slot, t)) {
            values[0] = s_st->e[slot].value;
            valid = 1;
        }
    } else {
        const fresh_snapshot_t snapshot = fresh_snapshot_locked();
        for (size_t i = 0; i < count; i++) {
            const int slot = slots[i];
            if (slot < 0 || slot >= SIG_SLOT_COUNT || !fresh_snapshot_valid(slot, t, &snapshot)) continue;
            values[i] = s_st->e[slot].value;
            valid |= 1ULL << i;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return valid;
}

void signal_store_entry(int slot, signal_entry_t *out)
{
    if (!out) return;
    if (!slot_ok(slot)) {
        memset(out, 0, sizeof(*out));
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_st->e[slot];
    portEXIT_CRITICAL(&s_mux);
}

uint64_t signal_store_get_delivered_mask(uint64_t requested, double values[SIG_SLOT_COUNT])
{
    if (!values) return 0;
    const uint64_t valid_slots = UINT64_MAX >> (64 - SIG_SLOT_COUNT);
    requested &= valid_slots;
    if (!requested) return 0;

    int32_t raw[SIG_SLOT_COUNT];
    uint64_t valid = 0;
    if (s_st) {
        portENTER_CRITICAL(&s_mux);
        uint32_t t = now_ms();
        if ((requested & (requested - 1u)) == 0) {
            const unsigned slot = (unsigned)__builtin_ctzll(requested);
            if (fresh_locked((int)slot, t)) {
                raw[slot] = s_st->e[slot].value;
                valid = requested;
            }
        } else {
            const fresh_snapshot_t snapshot = fresh_snapshot_locked();
            uint64_t pending = requested;
            while (pending) {
                const unsigned slot = (unsigned)__builtin_ctzll(pending);
                const uint64_t bit = UINT64_C(1) << slot;
                pending &= pending - 1u;
                if (!fresh_snapshot_valid((int)slot, t, &snapshot)) continue;
                raw[slot] = s_st->e[slot].value;
                valid |= bit;
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }

    // Theme pages usually bind only a few of the 49 slots. Walk those bits
    // instead of rescanning the full registry on every LVGL tick.
    uint64_t pending = requested;
    while (pending) {
        const unsigned slot = (unsigned)__builtin_ctzll(pending);
        const uint64_t bit = UINT64_C(1) << slot;
        pending &= pending - 1u;
        values[slot] = (valid & bit) ? signal_delivery_value((int)slot, raw[slot]) : NAN;
    }
    return valid;
}

int signal_store_slot_by_key(const char *key)
{
    if (!key) return SIG_SLOT_NONE;
    for (int i = 0; i < SIG_STD_COUNT; i++) {
        if (strcmp(sig_registry[i].key, key) == 0) return i;
    }
    if (!s_st) return SIG_SLOT_NONE;
    int found = SIG_SLOT_NONE;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_st->n_veh; i++) {
        if (strcmp(s_st->veh[i].key, key) == 0) {
            found = SIG_VEH_BASE + i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

int signal_store_slot_by_id(uint16_t num_id)
{
    const int standard_slot = sig_registry_slot_by_id(num_id);
    if (standard_slot >= 0) return standard_slot;
    if (!s_st || num_id < SIG_VEH_ID_BASE || num_id >= SIG_VEH_ID_BASE + SIG_VEH_MAX) {
        return SIG_SLOT_NONE;
    }
    const int offset = num_id - SIG_VEH_ID_BASE;
    int slot = SIG_SLOT_NONE;
    portENTER_CRITICAL(&s_mux);
    if (offset < s_st->n_veh) slot = SIG_VEH_BASE + offset;
    portEXIT_CRITICAL(&s_mux);
    return slot;
}

bool signal_store_copy_key(int slot, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (slot >= 0 && slot < SIG_STD_COUNT) {
        strlcpy(out, sig_registry[slot].key, out_size);
        return true;
    }
    if (!slot_ok(slot)) return false;
    bool found = false;
    portENTER_CRITICAL(&s_mux);
    const int offset = slot - SIG_VEH_BASE;
    if (offset >= 0 && offset < s_st->n_veh) {
        strlcpy(out, s_st->veh[offset].key, out_size);
        found = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

uint16_t signal_store_num_id(int slot)
{
    if (slot >= 0 && slot < SIG_STD_COUNT) return sig_registry[slot].num_id;
    if (!slot_ok(slot)) return 0;
    uint16_t num_id = 0;
    portENTER_CRITICAL(&s_mux);
    const int offset = slot - SIG_VEH_BASE;
    if (offset >= 0 && offset < s_st->n_veh) {
        num_id = (uint16_t)(SIG_VEH_ID_BASE + offset);
    }
    portEXIT_CRITICAL(&s_mux);
    return num_id;
}

static bool veh_key_ok(const char *key)
{
    /* ^veh\.[a-z0-9_]{1,24}$ */
    if (!key || strncmp(key, "veh.", 4) != 0) return false;
    size_t n = strlen(key + 4);
    if (n < 1 || n > 24) return false;
    for (const char *p = key + 4; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}

int signal_store_veh_register(const char *key, int32_t min, int32_t max, uint32_t stale_ms)
{
    if (!s_st || !veh_key_ok(key) || min > max) return SIG_SLOT_NONE;
    int slot = SIG_SLOT_NONE;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_st->n_veh; i++) {
        if (strcmp(s_st->veh[i].key, key) == 0) {
            slot = SIG_VEH_BASE + i;
            break;
        }
    }
    if (slot == SIG_SLOT_NONE && s_st->n_veh < SIG_VEH_MAX) {
        veh_def_t *d = &s_st->veh[s_st->n_veh];
        strlcpy(d->key, key, sizeof(d->key));
        d->min = min;
        d->max = max;
        d->stale_ms = stale_ms;
        slot = SIG_VEH_BASE + s_st->n_veh;
        s_st->e[slot].valid = false;
        s_st->e[slot].seq++;
        s_st->n_veh++;
    }
    portEXIT_CRITICAL(&s_mux);
    return slot;
}

void signal_store_veh_clear(void)
{
    if (!s_st) return;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SIG_VEH_MAX; i++) {
        s_st->e[SIG_VEH_BASE + i].valid = false;
        s_st->e[SIG_VEH_BASE + i].seq++;
    }
    memset(s_st->veh, 0, sizeof(s_st->veh));
    s_st->n_veh = 0;
    portEXIT_CRITICAL(&s_mux);
}

int signal_store_veh_count(void)
{
    if (!s_st) return 0;
    portENTER_CRITICAL(&s_mux);
    const int count = s_st->n_veh;
    portEXIT_CRITICAL(&s_mux);
    return count;
}

/* Atomically replace accessory ownership and values. The normal producers may
 * neither overwrite nor invalidate an owned slot. Clearing ownership invalidates
 * its old sample before the original producer is allowed to resume. */
void signal_store_hub_apply(uint8_t mask, const int32_t values[3])
{
    static const int slots[3] = {SIG_OBD_COOLANT_TEMP, SIG_OBD_OIL_TEMP, SIG_OBD_OIL_PRESSURE};
    if (!s_st) return;
    portENTER_CRITICAL(&s_mux);
    uint64_t next = 0;
    uint32_t t = now_ms();
    for (unsigned i=0; i<3; i++) {
        int slot=slots[i];
        uint64_t bit=UINT64_C(1)<<slot;
        int32_t mn, mx;
        bounds_of(slot, &mn, &mx);
        /* Hub's calibrated -100..1000 kPa probe can report vacuum. Keep
         * signed samples; the ordinary OBD producer's 0 kPa floor is unchanged. */
        if (slot == SIG_OBD_OIL_PRESSURE) mn = -100;
        signal_entry_t *e=&s_st->e[slot];
        if (values && (mask & (1u<<i)) && values[i]>=mn && values[i]<=mx) {
            next |= bit;
            e->value=values[i]; e->valid=true; e->ts_ms=t; e->seq++;
        } else if (s_st->hub_mask & bit) {
            e->valid=false; e->seq++;
        }
    }
    s_st->hub_mask=next;
    portEXIT_CRITICAL(&s_mux);
}

uint16_t signal_store_hub_obd_mask(void)
{
    if (!s_st) return 0;
    portENTER_CRITICAL(&s_mux);
    uint16_t mask=(uint16_t)s_st->hub_mask;
    portEXIT_CRITICAL(&s_mux);
    return mask;
}


uint8_t signal_store_hub_snapshot(int32_t values[3])
{
    static const int slots[3]={SIG_OBD_COOLANT_TEMP,SIG_OBD_OIL_TEMP,SIG_OBD_OIL_PRESSURE};
    if (!values || !s_st) return 0;
    uint8_t mask=0;
    portENTER_CRITICAL(&s_mux);
    uint32_t t=now_ms();
    for (unsigned i=0;i<3;i++) {
        int slot=slots[i];
        values[i]=0;
        if ((s_st->hub_mask & (UINT64_C(1)<<slot)) && fresh_locked(slot,t)) {
            mask |= (uint8_t)(1u<<i); values[i]=s_st->e[slot].value;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return mask;
}
