/* Production netfs positional writer with reordered ACKs and one lost ACK. */
#include "../../efi/platform/netfs.c"
#include <stdlib.h>
static uint64_t now;
static uint8_t target[100000];
static netfs_hdr_t acknowledgements[64];
static unsigned queued, retransmitted, lost_seq, writes;
uint64_t wyse_ticks_us(void) { now+=100; return now; }
bool net_active(void) { return true; }
bool net_resolve_server(void) { return true; }
void net_send_server(uint16_t s,uint16_t d,const void *p,size_t len) {
    (void)s;(void)d;
    const netfs_hdr_t *h=p;
    if (h->op!=OP_WRITE || h->offset+h->len>sizeof(target) || len!=sizeof(*h)+h->len) abort();
    memcpy(target+h->offset,(const uint8_t *)p+sizeof(*h),h->len);
    writes++;
    if (!lost_seq) { lost_seq=h->seq; return; } /* Write succeeds, first ACK lost. */
    if (h->seq==lost_seq) retransmitted++;
    if (queued>=64) abort();
    acknowledgements[queued++]=(netfs_hdr_t){NETFS_MAGIC,h->seq,OP_WRITE,0,0,h->handle,0};
}
const uint8_t *net_poll(uint16_t p,size_t *n) {
    (void)p;
    if (!queued) return NULL;
    *n=sizeof(netfs_hdr_t);
    return (const uint8_t *)&acknowledgements[--queued]; /* Reverse ordering. */
}
int main(void) {
    static uint8_t source[100000];
    for (unsigned i=0;i<sizeof(source);i++) source[i]=(uint8_t)(i*29+i/97);
    FIL f={0}; f.obj.fs=&netfs_vol; f.obj.handle=3;
    UINT wrote=0;
    if (netfs_write(&f,source,sizeof(source),&wrote)!=FR_OK || wrote!=sizeof(source) ||
        f.fptr!=sizeof(source) || f.obj.objsize!=sizeof(source) ||
        memcmp(source,target,sizeof(source)) || retransmitted!=1) return 1;
    printf("PASS: %u bytes, %u positional writes, reordered ACKs, lost ACK retried once, exact data\n",wrote,writes);
    return 0;
}
