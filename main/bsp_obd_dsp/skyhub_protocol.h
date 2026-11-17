#pragma once
/* Sky Hub accessory protocol v1. Explicit little-endian wire format; never
 * memcpy a C struct. Broadcast transport, application-addressed to target MAC.
 * MAC binding is not encryption or authentication against MAC spoofing. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SH_CHANNEL 1
#define SH_MAX_FRAME 40u
#define SH_TIMEOUT_US INT64_C(2000000)
#define SH_COOLANT 1u
#define SH_OIL_TEMP 2u
#define SH_OIL_PRESSURE 4u
#define SH_ALL 7u
#define SH_SENSOR_COUNT 3u

enum { SH_DISCOVER=1, SH_ANNOUNCE, SH_PAIR, SH_PAIR_ACK, SH_DATA, SH_UNPAIR, SH_RELAY };
typedef struct {
    uint8_t type, mask, target[6], owner[6];
    uint32_t session, seq, nonce;
    /* coolant C, oil temperature C, oil pressure bar*100 (signed). */
    int32_t values[SH_SENSOR_COUNT];
} sh_frame_t;

static inline bool sh_mac_valid(const uint8_t mac[6])
{
    return mac && !(mac[0] & 1u) &&
        (mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5]) != 0;
}
static inline uint32_t sh_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline void sh_put32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline uint16_t sh_crc(const uint8_t *p, size_t len)
{
    uint16_t crc=0xffff;
    while (len--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i=0;i<8;i++) crc=(uint16_t)((crc<<1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
static inline size_t sh_payload_len(uint8_t type, uint8_t mask)
{
    switch (type) {
    case SH_DISCOVER: return mask ? SIZE_MAX : 0;
    case SH_ANNOUNCE: return 6;
    case SH_PAIR: case SH_PAIR_ACK: case SH_UNPAIR: return mask ? SIZE_MAX : 4;
    case SH_DATA: case SH_RELAY: return 4u*((mask&1u)!=0)+4u*((mask&2u)!=0)+4u*((mask&4u)!=0);
    default: return SIZE_MAX;
    }
}
static inline size_t sh_encode(const sh_frame_t *f, uint8_t *out, size_t cap)
{
    if (!f || !out || !f->session || (f->mask & ~SH_ALL)) return 0;
    size_t payload=sh_payload_len(f->type,f->mask);
    if (payload==SIZE_MAX || cap<22+payload) return 0;
    memset(out,0,22+payload);
    out[0]='S'; out[1]='H'; out[2]=1; out[3]=f->type;
    sh_put32(out+4,f->session); sh_put32(out+8,f->seq);
    memcpy(out+12,f->target,6); out[18]=f->mask;
    size_t pos=20;
    if (f->type==SH_ANNOUNCE) memcpy(out+pos,f->owner,6);
    else if (f->type==SH_PAIR || f->type==SH_PAIR_ACK || f->type==SH_UNPAIR) sh_put32(out+pos,f->nonce);
    else if (f->type==SH_DATA || f->type==SH_RELAY) {
        for (unsigned i=0;i<SH_SENSOR_COUNT;i++) if (f->mask & (1u<<i)) {
            sh_put32(out+pos,(uint32_t)f->values[i]); pos+=4;
        }
    }
    size_t len=22+payload;
    uint16_t crc=sh_crc(out,len-2); out[len-2]=(uint8_t)crc; out[len-1]=(uint8_t)(crc>>8);
    return len;
}
static inline bool sh_decode(const uint8_t *p, size_t len, sh_frame_t *f)
{
    if (!p || !f || len<22 || len>SH_MAX_FRAME || p[0]!='S' || p[1]!='H' ||
        p[2]!=1 || p[19]!=0 || !sh_u32(p+4) || (p[18] & ~SH_ALL)) return false;
    size_t payload=sh_payload_len(p[3],p[18]);
    if (payload==SIZE_MAX || len!=22+payload || sh_crc(p,len-2)!=(uint16_t)(p[len-2]|(uint16_t)p[len-1]<<8)) return false;
    memset(f,0,sizeof(*f)); f->type=p[3]; f->session=sh_u32(p+4); f->seq=sh_u32(p+8);
    f->mask=p[18]; memcpy(f->target,p+12,6);
    if (f->type==SH_ANNOUNCE) memcpy(f->owner,p+20,6);
    else if (f->type==SH_PAIR || f->type==SH_PAIR_ACK || f->type==SH_UNPAIR) f->nonce=sh_u32(p+20);
    else if (f->type==SH_DATA || f->type==SH_RELAY) {
        size_t pos=20;
        for (unsigned i=0;i<SH_SENSOR_COUNT;i++) if (f->mask & (1u<<i)) {
            uint32_t raw=sh_u32(p+pos);
            f->values[i]=(int32_t)(raw & INT32_MAX) - (raw & UINT32_C(0x80000000) ? INT32_MAX : 0) - (raw & UINT32_C(0x80000000) ? 1 : 0);
            pos+=4;
        }
    }
    return true;
}
/* Serial ordering with a boot/session ID; remember the preceding boot so a
 * delayed packet cannot immediately switch back after a peer restarts. */
typedef struct { bool seen; uint32_t session, previous_session, seq; } sh_order_t;
static inline bool sh_order_accept(sh_order_t *s, const sh_frame_t *f)
{
    if (s->seen && s->session==f->session &&
        ((uint32_t)(f->seq-s->seq)==0 || (uint32_t)(f->seq-s->seq)>=UINT32_C(0x80000000))) return false;
    if (s->seen && s->session!=f->session) {
        if (f->session==s->previous_session) return false;
        s->previous_session=s->session;
    }
    s->session=f->session; s->seq=f->seq; s->seen=true; return true;
}
