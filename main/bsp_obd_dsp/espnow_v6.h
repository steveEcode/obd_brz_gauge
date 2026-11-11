#pragma once
/*
 * espnow_v6.h - ESP-NOW three-gauge wire protocol (pure C, no ESP-IDF deps).
 *
 * Two data formats live side by side during the v5 -> v6 transition
 * (REFACTOR_PLAN.md P3 / section 9):
 *
 *   v5  fixed 47-byte packed struct (legacy units). Offset 23 is the retired
 *       brake-temperature slot: always written as -1000, never read, never reused.
 *   v6  header + TLV records {u16 num_id, i32 value} in registry internal units
 *       (contracts/signals.json), valid masks, explicit gear byte and profile
 *       hash; split across fragments so every frame fits in 250 bytes.
 *
 * Every multi-byte field is little-endian. All frames start with the 16-bit
 * magic 0x4F42 and a version byte, so old firmware (which dispatches on exact
 * length and then checks version == 5) silently drops every v6 frame.
 *
 * Capability handshake: a v6-capable slave keeps sending the 4-byte v5
 * presence (old masters count it) and additionally an 8-byte base v6 HELLO
 * (10 bytes with OBD demand, 15 with source subscriptions; espnow_sources.h) with ESPNOW_CAP_V6_DATA. A master switches to v6 DATA only while every online
 * slave has a fresh HELLO; otherwise it keeps sending v5. Slaves accept both.
 *
 * v6 DATA frame (38 + 6*n + 2 bytes):
 *   off len field
 *    0   2  magic        0x4F42
 *    2   1  version      6
 *    3   1  type         1 (DATA)
 *    4   1  flags        ESPNOW_FLAG_*
 *    5   1  frag         (index << 4) | count, 1 <= count <= 15, index < count
 *    6   1  n_rec        records in this fragment (<= ESPNOW_V6_MAX_REC)
 *    7   1  gear         int8: -1 R, 0 N, 1..8; 127 = invalid (obd.gear is never a record)
 *    8   4  seq          snapshot sequence, shared by all fragments of one snapshot
 *   12   4  profile_hash espnow_profile_hash() of the master's active profile
 *   16   4  std_valid    bit i: signal num_id 0x0001+i is valid at the master
 *   20   4  veh_valid    bit i: signal num_id 0x0100+i is valid at the master
 *   24   1  sweep_step   master sweep progress (0 = none)
 *   25   1  intro_step   master boot-animation progress (0 = none, 255 = done)
 *   26  12  name         master name, NUL padded
 *   38 6*n  records      {u16 num_id, i32 value}
 *  +0   2   crc16        CRC-16/CCITT-FALSE over every preceding byte
 *
 * The header (masks, gear, flags) is repeated in every fragment, so each
 * fragment is applied on its own: no reassembly buffer on the receiver.
 * -------------------------------------------------------------------------- */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_MAGIC              0x4F42u
#define ESPNOW_FRAME_MAX          250   /* ESP_NOW_MAX_DATA_LEN (v1 payload limit) */
#define ESPNOW_NAME_LEN           12

/* Shared flag bits (same positions in v5 and v6 DATA). */
#define ESPNOW_FLAG_ELM           0x01  /* master connected to ELM327 */
#define ESPNOW_FLAG_LINKTEST      0x02  /* linked RPM test in progress */
#define ESPNOW_FLAG_LINKED_EN     0x04  /* master rpm_warn_linked_en */

#define ESPNOW_GEAR_INVALID       127

/* ---- v5 (legacy, packed) ---- */
#define ESPNOW_V5_VERSION         5
#define ESPNOW_V5_DATA_LEN        47
#define ESPNOW_V5_PRESENCE_LEN    4     /* magic, version, position */
#define ESPNOW_V5_CTRL_LEN        6     /* magic, version, cmd, u16 arg */
#define ESPNOW_V5_RETIRED_OFS     23    /* former brake_temp_x10 (int16) - never reuse */
#define ESPNOW_V5_RETIRED_VALUE   (-1000)
#define ESPNOW_V5_N_REC           11    /* values a v5 DATA packet can carry */

/* ---- v6 ---- */
#define ESPNOW_V6_VERSION         6
#define ESPNOW_V6_TYPE_DATA       1
#define ESPNOW_V6_TYPE_HELLO      2
#define ESPNOW_V6_TYPE_TPMS       3
#define ESPNOW_V6_HDR_LEN         38
#define ESPNOW_V6_REC_LEN         6
#define ESPNOW_V6_CRC_LEN         2
#define ESPNOW_V6_MAX_REC         35    /* largest frame = 38 + 35*6 + 2 = 250 B */
#define ESPNOW_V6_MAX_FRAGS       15
#define ESPNOW_V6_FRAME_LEN(n)    (ESPNOW_V6_HDR_LEN + (n) * ESPNOW_V6_REC_LEN + ESPNOW_V6_CRC_LEN)
#define ESPNOW_V6_HELLO_LEN       8     /* base: magic, version, type, position, caps, u16 reserved */
#define ESPNOW_CAP_V6_DATA        0x01  /* HELLO caps: slave decodes v6 DATA */
#define ESPNOW_CAP_DEMAND_MASK    0x02  /* HELLO carries a 16-bit local OBD demand mask */

/* Registry numeric ids (contracts/signals.json; static, never renumbered). */
#define ESPNOW_ID_RPM             0x0001
#define ESPNOW_ID_SPEED           0x0002
#define ESPNOW_ID_BOOST           0x0003
#define ESPNOW_ID_COOLANT_TEMP    0x0004
#define ESPNOW_ID_OIL_PRESSURE    0x0005
#define ESPNOW_ID_GEAR            0x0006
#define ESPNOW_ID_BATTERY_VOLTAGE 0x0007
#define ESPNOW_ID_OIL_TEMP        0x0008
#define ESPNOW_ID_AFR             0x0009
#define ESPNOW_ID_THROTTLE        0x000A
#define ESPNOW_ID_INTAKE_TEMP     0x000B
#define ESPNOW_ID_LOAD            0x000C
#define ESPNOW_ID_STD_FIRST       0x0001  /* std_valid bit 0 */
#define ESPNOW_ID_SYS_FIRST       0x0080  /* sys.*: device-local, never transported */
#define ESPNOW_ID_SYS_LAST        0x00BF
#define ESPNOW_ID_VEH_FIRST       0x0100  /* veh_valid bit 0 */

typedef enum {
    ESPNOW_PKT_UNKNOWN = 0,
    ESPNOW_PKT_V5_DATA,
    ESPNOW_PKT_V5_PRESENCE,
    ESPNOW_PKT_V5_CTRL,
    ESPNOW_PKT_V6_DATA,
    ESPNOW_PKT_V6_HELLO,
    ESPNOW_PKT_V6_TPMS,
} espnow_pkt_kind_t;

typedef struct {
    uint16_t num_id;
    int32_t  value;     /* registry internal units */
} espnow_rec_t;

/* ---- Master side: one snapshot, encoded as one v5 packet or N v6 fragments ---- */
#define ESPNOW_SNAP_MAX_REC       64

typedef struct {
    uint32_t     seq;
    uint32_t     profile_hash;
    uint32_t     std_valid;
    uint32_t     veh_valid;
    uint8_t      flags;
    int8_t       gear;          /* ESPNOW_GEAR_INVALID when obd.gear is not valid */
    uint8_t      sweep_step;
    uint8_t      intro_step;
    char         name[ESPNOW_NAME_LEN];
    uint8_t      n_rec;
    espnow_rec_t rec[ESPNOW_SNAP_MAX_REC];
} espnow_snapshot_t;

/* Zero the snapshot and mark gear invalid. */
void espnow_snapshot_init(espnow_snapshot_t *s);
/* Add (or replace) a valid value. obd.gear goes to the gear byte; std_valid /
 * veh_valid bits are set from num_id. Rejects id 0, sys.* ids, gear outside
 * int8 / equal to 127, and a full snapshot. */
bool espnow_snapshot_put(espnow_snapshot_t *s, uint16_t num_id, int32_t value);
/* Append without scanning for duplicates. The caller MUST guarantee that a
 * non-gear num_id is absent from rec[]; use put() when replacing a value.
 * The same invalid-ID, gear and capacity checks apply. Gear updates its byte
 * even in a full snapshot. Record order and mask behavior match put(). */
bool espnow_snapshot_append_unique(espnow_snapshot_t *s, uint16_t num_id, int32_t value);
bool espnow_snapshot_get(const espnow_snapshot_t *s, uint16_t num_id, int32_t *out);

/* v5: legacy units and sentinels, retired offset 23 = -1000. Returns 47 or 0. */
size_t espnow_v5_encode(const espnow_snapshot_t *s, uint8_t *out, size_t cap);
/* v6: number of fragments (>= 1) and encoder for fragment frag. Returns the
 * frame length or 0 (bad frag / cap too small). */
int    espnow_v6_frame_count(const espnow_snapshot_t *s);
size_t espnow_v6_encode(const espnow_snapshot_t *s, int frag, uint8_t *out, size_t cap);

/* ---- Slave side: a decoded DATA frame (v5 or v6) ---- */
typedef struct {
    uint8_t        version;      /* 5 or 6 */
    uint8_t        flags;
    uint8_t        frag_index;   /* v5: 0 */
    uint8_t        frag_count;   /* v5: 1 */
    uint32_t       seq;
    uint32_t       profile_hash; /* v5: 0 (unknown) */
    uint32_t       std_valid;    /* gear bit mirrors the gear byte */
    uint32_t       veh_valid;    /* v5: 0 */
    int8_t         gear;
    uint8_t        sweep_step;
    uint8_t        intro_step;
    uint8_t        n_rec;
    char           name[ESPNOW_NAME_LEN + 1];
    const uint8_t *v6_rec;       /* v6: points into the received buffer */
    uint8_t        v5_rec[ESPNOW_V5_N_REC * ESPNOW_V6_REC_LEN];
} espnow_rx_frame_t;

espnow_pkt_kind_t espnow_classify(const uint8_t *buf, size_t len);
/* Full validation (length, magic, version, structure, CRC for v6). On success
 * f->v6_rec references buf, so buf must outlive any espnow_rx_record() call. */
bool espnow_v5_decode(const uint8_t *buf, size_t len, espnow_rx_frame_t *f);
bool espnow_v6_decode(const uint8_t *buf, size_t len, espnow_rx_frame_t *f);
bool espnow_rx_record(const espnow_rx_frame_t *f, int i, uint16_t *num_id, int32_t *value);

/* Apply a decoded frame to a signal table through callbacks:
 *  - std ids 0x0001..0x0020 whose std_valid bit is clear are invalidated;
 *  - veh ids are touched only when veh_ok (profile hashes match), invalidated
 *    by veh_valid the same way;
 *  - records are written when the id is known locally (slot_by_id >= 0), the
 *    id is not sys.*, and (for masked ids) its valid bit is set; unknown ids
 *    are ignored; gear comes only from the gear byte.
 * Returns the number of values written. */
typedef struct {
    void *ctx;
    int  (*slot_by_id)(void *ctx, uint16_t num_id);   /* < 0: unknown here */
    void (*set)(void *ctx, int slot, int32_t value);
    void (*invalidate)(void *ctx, int slot);
    /* Optional ordered batch path; when present, used for non-gear records. */
    void (*set_many)(void *ctx, const int *slots, const int32_t *values, size_t count);
    /* Optional atomic path for invalidating all clear valid-mask bits. */
    void (*invalidate_many)(void *ctx, const int *slots, size_t count);
} espnow_apply_ops_t;

int espnow_rx_apply(const espnow_rx_frame_t *f, bool veh_ok, const espnow_apply_ops_t *ops);

/* ---- v6 HELLO (slave -> master capability announcement) ---- */
size_t espnow_hello_encode(uint8_t position, uint8_t caps, uint8_t *out, size_t cap);
bool   espnow_hello_decode(const uint8_t *buf, size_t len, uint8_t *position, uint8_t *caps);
/* Extended HELLO used by new three-gauge firmware. Older masters accept the
 * first 8 bytes and ignore the appended demand mask. */
size_t espnow_hello_encode_demand(uint8_t position, uint8_t caps, uint16_t demand_mask,
                                  uint8_t *out, size_t cap);
bool   espnow_hello_decode_demand(const uint8_t *buf, size_t len, uint8_t *position,
                                  uint8_t *caps, uint16_t *demand_mask);

/* ---- helpers ---- */
/* FNV-1a 32 over id bytes, a 0x00 separator and rev (u16 LE). id NULL = "". */
uint32_t espnow_profile_hash(const char *id, uint16_t rev);
/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). */
uint16_t espnow_crc16(const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif
