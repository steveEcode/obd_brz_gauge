#include "skyhub_protocol.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint8_t buf[SH_MAX_FRAME];
    for (unsigned mask=0;mask<=SH_ALL;mask++) {
        sh_frame_t f={.type=SH_DATA,.mask=(uint8_t)mask,.session=123,.seq=456,
                      .target={2,3,4,5,6,7},.values={-20,125,250}};
        size_t n=sh_encode(&f,buf,sizeof(buf));
        assert(n==22+4u*((mask&1)!=0)+4u*((mask&2)!=0)+4u*((mask&4)!=0));
        sh_frame_t decoded;
        assert(sh_decode(buf,n,&decoded));
        assert(decoded.mask==mask && !memcmp(decoded.target,f.target,6));
        for (unsigned i=0;i<3;i++) assert(decoded.values[i]==((mask & (1u<<i)) ? f.values[i] : 0));
        for (size_t i=0;i<n;i++) {
            buf[i]^=1; assert(!sh_decode(buf,n,&decoded)); buf[i]^=1;
        }
        assert(!sh_decode(buf,n-1,&decoded)); assert(!sh_decode(buf,n+1,&decoded));
    }
    sh_frame_t f={.type=SH_DATA,.mask=4,.session=1,.seq=UINT32_MAX,.values={0,0,INT32_MIN}};
    size_t n=sh_encode(&f,buf,sizeof(buf)); sh_frame_t decoded;
    assert(n==26 && sh_decode(buf,n,&decoded) && decoded.values[2]==INT32_MIN);
    f.values[2]=INT32_MAX; n=sh_encode(&f,buf,sizeof(buf));
    assert(sh_decode(buf,n,&decoded) && decoded.values[2]==INT32_MAX);
    f.mask=8; assert(!sh_encode(&f,buf,sizeof(buf))); f.mask=0;
    f.type=99; assert(!sh_encode(&f,buf,sizeof(buf)));
    f.type=SH_PAIR_ACK; f.nonce=0xaabbccdd; n=sh_encode(&f,buf,sizeof(buf));
    assert(n==26 && sh_decode(buf,n,&decoded) && decoded.nonce==f.nonce);
    f.type=SH_ANNOUNCE; memcpy(f.owner,"abcdef",6); n=sh_encode(&f,buf,sizeof(buf));
    assert(n==28 && sh_decode(buf,n,&decoded) && !memcmp(f.owner,decoded.owner,6));
    sh_order_t order={0};
    f.session=1; f.seq=UINT32_MAX; assert(sh_order_accept(&order,&f));
    assert(!sh_order_accept(&order,&f));
    f.seq=0; assert(sh_order_accept(&order,&f));
    f.seq=UINT32_MAX-1; assert(!sh_order_accept(&order,&f));
    f.session=2; f.seq=1; assert(sh_order_accept(&order,&f));
    f.session=1; f.seq=2; assert(!sh_order_accept(&order,&f));
    const uint8_t valid[]={0,1,2,3,4,5}, invalid[]={1,2,3,4,5,6}, empty[6]={0};
    assert(sh_mac_valid(valid) && !sh_mac_valid(invalid) && !sh_mac_valid(empty));
    puts("skyhub protocol: masks, signed units, lengths, CRC, order/session: OK");
}
