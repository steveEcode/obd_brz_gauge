#pragma once
/*
 * Internal helpers shared by the vehicle-profile validator and compiler:
 * hex parsing, CAN header/filter whitelists, OBD service whitelist and the
 * ISO-TP single-frame parser for raw frames. Pure C, no allocation.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CAN identifiers in compiled profiles: 29-bit ids carry this flag. */
#define VP_ID_29BIT 0x80000000u
#define VP_ID_MASK  0x1FFFFFFFu

/* Explicit Suzuki KWP initialization headers; never CAN request IDs. */
bool vp_kwp_init_header(const char *cmd);

typedef enum {
    VP_HDR_INVALID = 0,
    VP_HDR_PHYSICAL,     /* 600-7FF, 18DAxxF1 */
    VP_HDR_FUNCTIONAL,   /* 7DF, 18DB33F1 */
} vp_hdr_class_t;

/* One interpretation of a raw frame as an ISO-TP single frame. */
typedef struct {
    bool    ext;          /* first byte is an extended (target) address */
    uint8_t ext_addr;
    uint8_t off;          /* payload offset inside the frame */
    uint8_t len;          /* payload length (= service + parameters) */
} vp_rawframe_t;

/* Parse exactly ndigits hex digits (3 or 8 for CAN ids), no prefix/spaces. */
bool vp_parse_hex_id(const char *s, uint32_t *out, size_t *ndigits);

/*
 * Parse a hex byte string. With allow_spaces, single spaces between bytes
 * are accepted ("18 03 22 DA 2E"); otherwise the string must be compact
 * ("22DA2E"). Fails on odd digit counts, other characters or > cap bytes.
 */
bool vp_parse_hex_bytes(const char *s, bool allow_spaces, uint8_t *out, size_t cap, size_t *n);

/* Transmit header whitelist (ATSH). *id gets VP_ID_29BIT for 8-digit ids. */
vp_hdr_class_t vp_classify_tx_header(const char *s, uint32_t *id);

/* Receive filter whitelist (ATCRA): standard 11-bit response range 600-7FF,
 * and 18DAF1xx for 29-bit ISO-TP. */
bool vp_parse_rx_filter(const char *s, uint32_t *id);

/* CAN monitor id: 3 digits <= 7FF or 8 digits <= 1FFFFFFF. */
bool vp_parse_can_id(const char *s, uint32_t *id);

/* Classify an already-encoded tx header id. */
vp_hdr_class_t vp_classify_tx_id(uint32_t id);

/* Read-only service whitelist for requests; write/reset/security services
 * remain rejected by the profile boundary. */
bool vp_service_allowed(uint8_t svc);

/* Functional addressing is limited to standard broadcast-safe OBD modes. */
bool vp_service_functional_ok(uint8_t svc);

/* Exact request length (service + parameters) for an allowed service. */
bool vp_service_len_ok(uint8_t svc, size_t len);

/*
 * Interpret a raw CAN payload as an ISO-TP single frame, both with normal
 * addressing and with one extended-address byte. Returns the number of
 * structurally valid interpretations (0..2) written to out[].
 */
size_t vp_rawframe_parse(const uint8_t *b, size_t n, vp_rawframe_t out[2]);

/* Id rules: [a-z0-9._-], 1..31 chars. */
bool vp_id_ok(const char *s);

/* Printable ASCII 0x20..0x7E, 1..max chars. */
bool vp_ascii_ok(const char *s, size_t max);

/* Standard CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF). */
uint32_t vp_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
