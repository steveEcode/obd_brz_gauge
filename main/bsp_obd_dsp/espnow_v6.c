/*
 * espnow_v6.c - ESP-NOW three-gauge wire codec (v5 legacy + v6 TLV).
 *
 * Portable C (ROM CRC on ESP32, no FreeRTOS or globals): built into the firmware and
 * into the host tests under test/host/espnow. Frame layouts: espnow_v6.h.
 */
#include "espnow_v6.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#endif

/* v5 DATA field offsets (packed little-endian struct, 47 bytes). */
#define V5_OFS_FLAGS      3
#define V5_OFS_SEQ        4
#define V5_OFS_RPM        8
#define V5_OFS_SPEED      10
#define V5_OFS_SWEEP      11
#define V5_OFS_INTRO      12
#define V5_OFS_COOLANT    13
#define V5_OFS_INTAKE     15
#define V5_OFS_OIL_TEMP   17
#define V5_OFS_OIL_P      19
#define V5_OFS_BOOST      21
/*      ESPNOW_V5_RETIRED_OFS 23: retired brake temperature, int16 = -1000 */
#define V5_OFS_LOAD       25
#define V5_OFS_TPS        27
#define V5_OFS_BAT        29
#define V5_OFS_AFR        33
#define V5_OFS_NAME       35

/* v6 DATA header offsets. */
#define V6_OFS_TYPE       3
#define V6_OFS_FLAGS      4
#define V6_OFS_FRAG       5
#define V6_OFS_NREC       6
#define V6_OFS_GEAR       7
#define V6_OFS_SEQ        8
#define V6_OFS_HASH       12
#define V6_OFS_STD_VALID  16
#define V6_OFS_VEH_VALID  20
#define V6_OFS_SWEEP      24
#define V6_OFS_INTRO      25
#define V6_OFS_NAME       26

/* v6 HELLO offsets. */
#define HELLO_OFS_POS     4
#define HELLO_OFS_CAPS    5

/* Legacy (v5 / obd_data_cache) sentinels. */
#define V5_TEMP_INVALID   (-40)    /* coolant / intake */
#define V5_OIL_T_INVALID  (-100)
#define V5_OIL_T_MIN      (-20)    /* obd_data_set_oil_temp(): outside -> hold */
#define V5_OIL_T_MAX      255
#define V5_BOOST_INVALID  (-32768)

#define MASK_BITS         32

_Static_assert(ESPNOW_V6_FRAME_LEN(ESPNOW_V6_MAX_REC) <= ESPNOW_FRAME_MAX,
               "largest v6 frame must fit one ESP-NOW payload");
_Static_assert(V6_OFS_NAME + ESPNOW_NAME_LEN == ESPNOW_V6_HDR_LEN, "v6 header layout");
_Static_assert(V5_OFS_NAME + ESPNOW_NAME_LEN == ESPNOW_V5_DATA_LEN, "v5 data layout");
_Static_assert(ESPNOW_V6_MAX_REC * ESPNOW_V6_MAX_FRAGS >= ESPNOW_SNAP_MAX_REC,
               "a full snapshot must fit the fragment count");

/* ---------------------------------------------------------------- helpers */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static void put_i16(uint8_t *p, int32_t v)
{
    if (v < INT16_MIN) v = INT16_MIN;
    if (v > INT16_MAX) v = INT16_MAX;
    put_u16(p, (uint16_t)(int16_t)v);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t get_i16(const uint8_t *p)
{
    return (int16_t)get_u16(p);
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Same rounding as obd_data_get_snapshot() (internal 0.01 -> legacy 0.1). */
static int32_t div10_half_up(int32_t v)
{
    return v >= 0 ? (v + 5) / 10 : -((-v + 5) / 10);
}

static bool id_is_sys(uint16_t id)
{
    return id >= ESPNOW_ID_SYS_FIRST && id <= ESPNOW_ID_SYS_LAST;
}

static bool id_is_veh(uint16_t id)
{
    return id >= ESPNOW_ID_VEH_FIRST;   /* profile-defined: meaning depends on the profile */
}

/* Bit of id in std_valid / veh_valid; false when the id has no mask bit. */
static bool id_mask_bit(uint16_t id, bool *is_veh, uint32_t *bit)
{
    if (id >= ESPNOW_ID_STD_FIRST && id < ESPNOW_ID_STD_FIRST + MASK_BITS) {
        *is_veh = false;
        *bit = 1u << (id - ESPNOW_ID_STD_FIRST);
        return true;
    }
    if (id >= ESPNOW_ID_VEH_FIRST && id < ESPNOW_ID_VEH_FIRST + MASK_BITS) {
        *is_veh = true;
        *bit = 1u << (id - ESPNOW_ID_VEH_FIRST);
        return true;
    }
    return false;
}

static void put_name(uint8_t *dst, const char *name)
{
    memset(dst, 0, ESPNOW_NAME_LEN);
    size_t n = 0;
    while (n < ESPNOW_NAME_LEN && name[n] != '\0') n++;
    memcpy(dst, name, n);
}

static void get_name(char *dst, const uint8_t *src)
{
    memcpy(dst, src, ESPNOW_NAME_LEN);
    dst[ESPNOW_NAME_LEN] = '\0';
}

uint16_t espnow_crc16(const uint8_t *p, size_t n)
{
#ifdef ESP_PLATFORM
    /* SDK CRC state is complemented. Seed 0 and complement the result for
     * CRC-16/CCITT-FALSE (init 0xFFFF, xorout 0). Chunk on wider host size_t. */
    uint16_t crc = 0;
    while (n) {
        uint32_t len = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
        crc = esp_rom_crc16_be(crc, p, len);
        p += len;
        n -= len;
    }
    return (uint16_t)~crc;
#else
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)(p[i] << 8));
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
#endif
}

uint32_t espnow_profile_hash(const char *id, uint16_t rev)
{
    uint32_t h = 0x811C9DC5u;
    if (id) {
        for (const uint8_t *p = (const uint8_t *)id; *p; p++) {
            h = (h ^ *p) * 0x01000193u;
        }
    }
    const uint8_t tail[3] = { 0x00, (uint8_t)(rev & 0xFFu), (uint8_t)(rev >> 8) };
    for (size_t i = 0; i < sizeof(tail); i++) {
        h = (h ^ tail[i]) * 0x01000193u;
    }
    return h;
}

/* --------------------------------------------------------------- snapshot */

void espnow_snapshot_init(espnow_snapshot_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->gear = ESPNOW_GEAR_INVALID;
}

bool espnow_snapshot_put(espnow_snapshot_t *s, uint16_t num_id, int32_t value)
{
    if (!s || num_id == 0 || id_is_sys(num_id)) return false;

    if (num_id == ESPNOW_ID_GEAR) {
        if (value < INT8_MIN || value >= ESPNOW_GEAR_INVALID) return false;
        s->gear = (int8_t)value;
        s->std_valid |= 1u << (ESPNOW_ID_GEAR - ESPNOW_ID_STD_FIRST);
        return true;
    }

    int i = 0;
    while (i < s->n_rec && s->rec[i].num_id != num_id) i++;
    if (i == s->n_rec) {
        if (s->n_rec >= ESPNOW_SNAP_MAX_REC) return false;
        s->n_rec++;
        s->rec[i].num_id = num_id;
    }
    s->rec[i].value = value;

    bool is_veh;
    uint32_t bit;
    if (id_mask_bit(num_id, &is_veh, &bit)) {
        if (is_veh) s->veh_valid |= bit;
        else        s->std_valid |= bit;
    }
    return true;
}

/* The caller builds a fresh snapshot from disjoint, unique signal IDs. */
bool espnow_snapshot_append_unique(espnow_snapshot_t *s, uint16_t num_id, int32_t value)
{
    if (!s) return false;
    if (num_id == ESPNOW_ID_GEAR) {
        if (value < INT8_MIN || value >= ESPNOW_GEAR_INVALID) return false;
        s->gear = (int8_t)value;
        s->std_valid |= 1u << (ESPNOW_ID_GEAR - ESPNOW_ID_STD_FIRST);
        return true;
    }
    if (num_id == 0 || id_is_sys(num_id) || s->n_rec >= ESPNOW_SNAP_MAX_REC) return false;

    const int i = s->n_rec++;
    s->rec[i].num_id = num_id;
    s->rec[i].value = value;
    bool is_veh;
    uint32_t bit;
    if (id_mask_bit(num_id, &is_veh, &bit)) {
        if (is_veh) s->veh_valid |= bit;
        else        s->std_valid |= bit;
    }
    return true;
}

bool espnow_snapshot_get(const espnow_snapshot_t *s, uint16_t num_id, int32_t *out)
{
    if (!s || !out) return false;
    if (num_id == ESPNOW_ID_GEAR) {
        if (s->gear == ESPNOW_GEAR_INVALID) return false;
        *out = s->gear;
        return true;
    }
    for (int i = 0; i < s->n_rec; i++) {
        if (s->rec[i].num_id == num_id) {
            *out = s->rec[i].value;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- encoders */

size_t espnow_v5_encode(const espnow_snapshot_t *s, uint8_t *out, size_t cap)
{
    if (!s || !out || cap < ESPNOW_V5_DATA_LEN) return 0;
    memset(out, 0, ESPNOW_V5_DATA_LEN);

    int32_t v = 0;
    put_u16(out, ESPNOW_MAGIC);
    out[2] = ESPNOW_V5_VERSION;
    out[V5_OFS_FLAGS] = s->flags;
    put_u32(out + V5_OFS_SEQ, s->seq);

    v = espnow_snapshot_get(s, ESPNOW_ID_RPM, &v) ? clamp32(v, 0, UINT16_MAX) : 0;
    put_u16(out + V5_OFS_RPM, (uint16_t)v);
    v = espnow_snapshot_get(s, ESPNOW_ID_SPEED, &v) ? clamp32(v, 0, UINT8_MAX) : 0;
    out[V5_OFS_SPEED] = (uint8_t)v;
    out[V5_OFS_SWEEP] = s->sweep_step;
    out[V5_OFS_INTRO] = s->intro_step;

    put_i16(out + V5_OFS_COOLANT,
            espnow_snapshot_get(s, ESPNOW_ID_COOLANT_TEMP, &v) ? v : V5_TEMP_INVALID);
    put_i16(out + V5_OFS_INTAKE,
            espnow_snapshot_get(s, ESPNOW_ID_INTAKE_TEMP, &v) ? v : V5_TEMP_INVALID);
    put_i16(out + V5_OFS_OIL_TEMP,
            espnow_snapshot_get(s, ESPNOW_ID_OIL_TEMP, &v) ? v : V5_OIL_T_INVALID);
    put_i16(out + V5_OFS_OIL_P,
            espnow_snapshot_get(s, ESPNOW_ID_OIL_PRESSURE, &v)
                ? clamp32(div10_half_up(v), 0, INT16_MAX) : -1);
    put_i16(out + V5_OFS_BOOST,
            espnow_snapshot_get(s, ESPNOW_ID_BOOST, &v)
                ? clamp32(div10_half_up(v), V5_BOOST_INVALID + 1, INT16_MAX) : V5_BOOST_INVALID);
    put_i16(out + ESPNOW_V5_RETIRED_OFS, ESPNOW_V5_RETIRED_VALUE);   /* retired: never reuse */
    put_i16(out + V5_OFS_LOAD,
            espnow_snapshot_get(s, ESPNOW_ID_LOAD, &v) ? clamp32(v, 0, INT16_MAX) : -1);
    put_i16(out + V5_OFS_TPS,
            espnow_snapshot_get(s, ESPNOW_ID_THROTTLE, &v) ? clamp32(v, 0, INT16_MAX) : -1);
    v = espnow_snapshot_get(s, ESPNOW_ID_BATTERY_VOLTAGE, &v) ? clamp32(v, 0, INT32_MAX) : -1;
    put_u32(out + V5_OFS_BAT, (uint32_t)v);
    put_i16(out + V5_OFS_AFR,
            espnow_snapshot_get(s, ESPNOW_ID_AFR, &v) ? clamp32(v, 0, INT16_MAX) : -1);
    put_name(out + V5_OFS_NAME, s->name);
    return ESPNOW_V5_DATA_LEN;
}

int espnow_v6_frame_count(const espnow_snapshot_t *s)
{
    if (!s || s->n_rec == 0) return 1;
    return (s->n_rec + ESPNOW_V6_MAX_REC - 1) / ESPNOW_V6_MAX_REC;
}

size_t espnow_v6_encode(const espnow_snapshot_t *s, int frag, uint8_t *out, size_t cap)
{
    if (!s || !out) return 0;
    int count = espnow_v6_frame_count(s);
    if (frag < 0 || frag >= count) return 0;

    int first = frag * ESPNOW_V6_MAX_REC;
    int n = s->n_rec - first;
    if (n > ESPNOW_V6_MAX_REC) n = ESPNOW_V6_MAX_REC;
    if (n < 0) n = 0;
    size_t len = (size_t)ESPNOW_V6_FRAME_LEN(n);
    if (cap < len) return 0;

    memset(out, 0, ESPNOW_V6_HDR_LEN);
    put_u16(out, ESPNOW_MAGIC);
    out[2] = ESPNOW_V6_VERSION;
    out[V6_OFS_TYPE] = ESPNOW_V6_TYPE_DATA;
    out[V6_OFS_FLAGS] = s->flags;
    out[V6_OFS_FRAG] = (uint8_t)((frag << 4) | count);
    out[V6_OFS_NREC] = (uint8_t)n;
    out[V6_OFS_GEAR] = (uint8_t)s->gear;
    put_u32(out + V6_OFS_SEQ, s->seq);
    put_u32(out + V6_OFS_HASH, s->profile_hash);
    put_u32(out + V6_OFS_STD_VALID, s->std_valid);
    put_u32(out + V6_OFS_VEH_VALID, s->veh_valid);
    out[V6_OFS_SWEEP] = s->sweep_step;
    out[V6_OFS_INTRO] = s->intro_step;
    put_name(out + V6_OFS_NAME, s->name);

    uint8_t *r = out + ESPNOW_V6_HDR_LEN;
    for (int i = 0; i < n; i++, r += ESPNOW_V6_REC_LEN) {
        put_u16(r, s->rec[first + i].num_id);
        put_u32(r + 2, (uint32_t)s->rec[first + i].value);
    }
    put_u16(r, espnow_crc16(out, len - ESPNOW_V6_CRC_LEN));
    return len;
}

size_t espnow_hello_encode(uint8_t position, uint8_t caps, uint8_t *out, size_t cap)
{
    return espnow_hello_encode_demand(position, caps, 0, out, cap);
}

size_t espnow_hello_encode_demand(uint8_t position, uint8_t caps, uint16_t demand_mask,
                                  uint8_t *out, size_t cap)
{
    const size_t len = (caps & ESPNOW_CAP_DEMAND_MASK) ? ESPNOW_V6_HELLO_LEN + 2u : ESPNOW_V6_HELLO_LEN;
    if (!out || cap < len) return 0;
    memset(out, 0, len);
    put_u16(out, ESPNOW_MAGIC);
    out[2] = ESPNOW_V6_VERSION;
    out[V6_OFS_TYPE] = ESPNOW_V6_TYPE_HELLO;
    out[HELLO_OFS_POS] = position;
    out[HELLO_OFS_CAPS] = caps;
    if (caps & ESPNOW_CAP_DEMAND_MASK) put_u16(out + ESPNOW_V6_HELLO_LEN, demand_mask);
    return len;
}

/* ---------------------------------------------------------------- decoders */

espnow_pkt_kind_t espnow_classify(const uint8_t *buf, size_t len)
{
    if (!buf || len < 3 || get_u16(buf) != ESPNOW_MAGIC) return ESPNOW_PKT_UNKNOWN;
    if (buf[2] == ESPNOW_V5_VERSION) {
        switch (len) {
        case ESPNOW_V5_DATA_LEN:     return ESPNOW_PKT_V5_DATA;
        case ESPNOW_V5_PRESENCE_LEN: return ESPNOW_PKT_V5_PRESENCE;
        case ESPNOW_V5_CTRL_LEN:     return ESPNOW_PKT_V5_CTRL;
        default:                     return ESPNOW_PKT_UNKNOWN;
        }
    }
    if (buf[2] == ESPNOW_V6_VERSION && len > V6_OFS_TYPE) {
        if (buf[V6_OFS_TYPE] == ESPNOW_V6_TYPE_DATA)  return ESPNOW_PKT_V6_DATA;
        if (buf[V6_OFS_TYPE] == ESPNOW_V6_TYPE_HELLO) return ESPNOW_PKT_V6_HELLO;
        if (buf[V6_OFS_TYPE] == ESPNOW_V6_TYPE_TPMS) return ESPNOW_PKT_V6_TPMS;
    }
    return ESPNOW_PKT_UNKNOWN;
}

static void v5_add(espnow_rx_frame_t *f, uint16_t id, int32_t value)
{
    uint8_t *r = f->v5_rec + (size_t)f->n_rec * ESPNOW_V6_REC_LEN;
    put_u16(r, id);
    put_u32(r + 2, (uint32_t)value);
    f->n_rec++;
    f->std_valid |= 1u << (id - ESPNOW_ID_STD_FIRST);
}

/* Legacy value: invalid -> bit stays clear (slave invalidates), else record. */
static void v5_add_if(espnow_rx_frame_t *f, uint16_t id, bool invalid, int32_t value)
{
    if (!invalid) v5_add(f, id, value);
}

bool espnow_v5_decode(const uint8_t *buf, size_t len, espnow_rx_frame_t *f)
{
    if (!buf || !f || len != ESPNOW_V5_DATA_LEN) return false;
    if (get_u16(buf) != ESPNOW_MAGIC || buf[2] != ESPNOW_V5_VERSION) return false;

    memset(f, 0, sizeof(*f));
    f->version = ESPNOW_V5_VERSION;
    f->flags = buf[V5_OFS_FLAGS];
    f->frag_count = 1;
    f->seq = get_u32(buf + V5_OFS_SEQ);
    f->gear = ESPNOW_GEAR_INVALID;          /* v5 carries no gear */
    f->sweep_step = buf[V5_OFS_SWEEP];
    f->intro_step = buf[V5_OFS_INTRO];
    get_name(f->name, buf + V5_OFS_NAME);

    /* Same conversions and sentinels as the obd_data_set_*() legacy setters. */
    v5_add(f, ESPNOW_ID_RPM, get_u16(buf + V5_OFS_RPM));
    v5_add(f, ESPNOW_ID_SPEED, buf[V5_OFS_SPEED]);
    int16_t t = get_i16(buf + V5_OFS_COOLANT);
    v5_add_if(f, ESPNOW_ID_COOLANT_TEMP, t == V5_TEMP_INVALID, t);
    t = get_i16(buf + V5_OFS_INTAKE);
    v5_add_if(f, ESPNOW_ID_INTAKE_TEMP, t == V5_TEMP_INVALID, t);
    t = get_i16(buf + V5_OFS_OIL_TEMP);
    if (t >= V5_OIL_T_MIN && t <= V5_OIL_T_MAX) {
        v5_add(f, ESPNOW_ID_OIL_TEMP, t);
    } else if (t != V5_OIL_T_INVALID) {
        /* setter "hold" rule: bit set without a record leaves the value untouched */
        f->std_valid |= 1u << (ESPNOW_ID_OIL_TEMP - ESPNOW_ID_STD_FIRST);
    }
    t = get_i16(buf + V5_OFS_OIL_P);
    v5_add_if(f, ESPNOW_ID_OIL_PRESSURE, t < 0, (int32_t)t * 10);
    t = get_i16(buf + V5_OFS_BOOST);
    v5_add_if(f, ESPNOW_ID_BOOST, t == V5_BOOST_INVALID, (int32_t)t * 10);
    /* ESPNOW_V5_RETIRED_OFS: retired brake temperature, ignored */
    t = get_i16(buf + V5_OFS_LOAD);
    v5_add_if(f, ESPNOW_ID_LOAD, t < 0, t);
    t = get_i16(buf + V5_OFS_TPS);
    v5_add_if(f, ESPNOW_ID_THROTTLE, t < 0, t);
    int32_t mv = (int32_t)get_u32(buf + V5_OFS_BAT);
    v5_add_if(f, ESPNOW_ID_BATTERY_VOLTAGE, mv < 0, mv);
    t = get_i16(buf + V5_OFS_AFR);
    v5_add_if(f, ESPNOW_ID_AFR, t < 0, t);
    return true;
}

bool espnow_v6_decode(const uint8_t *buf, size_t len, espnow_rx_frame_t *f)
{
    if (!buf || !f || len < (size_t)ESPNOW_V6_FRAME_LEN(0) || len > ESPNOW_FRAME_MAX) return false;
    if (get_u16(buf) != ESPNOW_MAGIC || buf[2] != ESPNOW_V6_VERSION ||
        buf[V6_OFS_TYPE] != ESPNOW_V6_TYPE_DATA) return false;

    uint8_t count = buf[V6_OFS_FRAG] & 0x0Fu;
    uint8_t index = (uint8_t)(buf[V6_OFS_FRAG] >> 4);
    uint8_t n = buf[V6_OFS_NREC];
    if (count == 0 || index >= count || n > ESPNOW_V6_MAX_REC) return false;
    if (len != (size_t)ESPNOW_V6_FRAME_LEN(n)) return false;
    if (get_u16(buf + len - ESPNOW_V6_CRC_LEN) != espnow_crc16(buf, len - ESPNOW_V6_CRC_LEN)) return false;

    memset(f, 0, sizeof(*f));
    f->version = ESPNOW_V6_VERSION;
    f->flags = buf[V6_OFS_FLAGS];
    f->frag_index = index;
    f->frag_count = count;
    f->seq = get_u32(buf + V6_OFS_SEQ);
    f->profile_hash = get_u32(buf + V6_OFS_HASH);
    f->std_valid = get_u32(buf + V6_OFS_STD_VALID);
    f->veh_valid = get_u32(buf + V6_OFS_VEH_VALID);
    f->gear = (int8_t)buf[V6_OFS_GEAR];
    f->sweep_step = buf[V6_OFS_SWEEP];
    f->intro_step = buf[V6_OFS_INTRO];
    f->n_rec = n;
    get_name(f->name, buf + V6_OFS_NAME);
    f->v6_rec = buf + ESPNOW_V6_HDR_LEN;

    /* The gear byte is authoritative for obd.gear. */
    const uint32_t gear_bit = 1u << (ESPNOW_ID_GEAR - ESPNOW_ID_STD_FIRST);
    if (f->gear == ESPNOW_GEAR_INVALID) f->std_valid &= ~gear_bit;
    else                                f->std_valid |= gear_bit;
    return true;
}

bool espnow_hello_decode(const uint8_t *buf, size_t len, uint8_t *position, uint8_t *caps)
{
    return espnow_hello_decode_demand(buf, len, position, caps, NULL);
}

bool espnow_hello_decode_demand(const uint8_t *buf, size_t len, uint8_t *position,
                                uint8_t *caps, uint16_t *demand_mask)
{
    /* >= : later firmware may append fields; this one only reads the first 8 bytes */
    if (!buf || len < ESPNOW_V6_HELLO_LEN || len > ESPNOW_FRAME_MAX) return false;
    if (get_u16(buf) != ESPNOW_MAGIC || buf[2] != ESPNOW_V6_VERSION ||
        buf[V6_OFS_TYPE] != ESPNOW_V6_TYPE_HELLO) return false;
    if (position) *position = buf[HELLO_OFS_POS];
    if (caps) *caps = buf[HELLO_OFS_CAPS];
    if (demand_mask) *demand_mask = ((buf[HELLO_OFS_CAPS] & ESPNOW_CAP_DEMAND_MASK) &&
                                    len >= ESPNOW_V6_HELLO_LEN + 2u)
        ? get_u16(buf + ESPNOW_V6_HELLO_LEN) : 0;
    return true;
}

bool espnow_rx_record(const espnow_rx_frame_t *f, int i, uint16_t *num_id, int32_t *value)
{
    if (!f || i < 0 || i >= f->n_rec || !num_id || !value) return false;
    const uint8_t *base = (f->version == ESPNOW_V6_VERSION) ? f->v6_rec : f->v5_rec;
    if (!base) return false;
    const uint8_t *r = base + (size_t)i * ESPNOW_V6_REC_LEN;
    *num_id = get_u16(r);
    *value = (int32_t)get_u32(r + 2);
    return true;
}

/* ------------------------------------------------------------------- apply */

static void invalidate_clear_bits(uint16_t first_id, uint32_t mask, const espnow_apply_ops_t *ops)
{
    int slots[MASK_BITS];
    size_t count = 0;
    for (int i = 0; i < MASK_BITS; i++) {
        uint16_t id = (uint16_t)(first_id + i);
        if ((mask & (1u << i)) || id == ESPNOW_ID_GEAR) continue;
        int slot = ops->slot_by_id(ops->ctx, id);
        if (slot >= 0) slots[count++] = slot;
    }
    if (ops->invalidate_many) {
        if (count) ops->invalidate_many(ops->ctx, slots, count);
    } else {
        for (size_t i = 0; i < count; i++) ops->invalidate(ops->ctx, slots[i]);
    }
}

int espnow_rx_apply(const espnow_rx_frame_t *f, bool veh_ok, const espnow_apply_ops_t *ops)
{
    if (!f || f->n_rec > ESPNOW_V6_MAX_REC || !ops || !ops->slot_by_id ||
        (!ops->set && !ops->set_many) || !ops->invalidate) return 0;
    int written = 0;

    invalidate_clear_bits(ESPNOW_ID_STD_FIRST, f->std_valid, ops);
    if (veh_ok) invalidate_clear_bits(ESPNOW_ID_VEH_FIRST, f->veh_valid, ops);

    int slot = ops->slot_by_id(ops->ctx, ESPNOW_ID_GEAR);
    if (slot >= 0) {
        if (f->gear != ESPNOW_GEAR_INVALID) {
            ops->set(ops->ctx, slot, f->gear);
            written++;
        } else {
            ops->invalidate(ops->ctx, slot);
        }
    }

    int batch_slots[ESPNOW_V6_MAX_REC];
    int32_t batch_values[ESPNOW_V6_MAX_REC];
    size_t batch_count = 0;
    for (int i = 0; i < f->n_rec; i++) {
        uint16_t id;
        int32_t value;
        if (!espnow_rx_record(f, i, &id, &value)) break;
        if (id == 0 || id == ESPNOW_ID_GEAR || id_is_sys(id)) continue;
        if (id_is_veh(id) && !veh_ok) continue;
        bool is_veh;
        uint32_t bit;
        if (id_mask_bit(id, &is_veh, &bit) && !((is_veh ? f->veh_valid : f->std_valid) & bit)) {
            continue;   /* record contradicts its valid bit: ignore */
        }
        slot = ops->slot_by_id(ops->ctx, id);
        if (slot < 0) continue;   /* unknown on this firmware: ignore */
        if (ops->set_many) {
            batch_slots[batch_count] = slot;
            batch_values[batch_count] = value;
            batch_count++;
        } else if (ops->set) {
            ops->set(ops->ctx, slot, value);
        }
        written++;
    }
    if (batch_count) ops->set_many(ops->ctx, batch_slots, batch_values, batch_count);
    return written;
}
