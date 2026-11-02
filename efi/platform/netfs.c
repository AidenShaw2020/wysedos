// FatFs API over UDP to tools/rig/netfs.py on the development PC (as netfs_*,
// fs.c picks it per path).
//
// Metadata uses request/response. Reads and positional writes use batches.
// Lost replies are retried with the same sequence and offset; rewriting
// identical bytes at an offset is idempotent.

#include <string.h>
#include <stdio.h>
#include "ff.h"
#include "../src/net.h"

uint64_t wyse_ticks_us(void);

#define NETFS_PORT      5556
#define NETFS_MAGIC     0x31534657u     // "WFS1"
#define NETFS_CHUNK     1400

enum { OP_OPEN = 1, OP_CLOSE, OP_READ, OP_WRITE, OP_SYNC, OP_OPENDIR, OP_READDIR, OP_CLOSEDIR,
       OP_STAT, OP_UNLINK, OP_TRUNCATE, OP_MKDIR, OP_RENAME, OP_READB };

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint8_t op;
    uint8_t status;
    uint16_t len;
    uint32_t handle;
    uint64_t offset;
} netfs_hdr_t;

static uint32_t seq;
static uint8_t pkt[sizeof(netfs_hdr_t) + NETFS_CHUNK + 64];
static uint8_t reply[sizeof(netfs_hdr_t) + NETFS_CHUNK + 64];
static FATFS netfs_vol;     // marks open objects
uint32_t g_netfs_retries, g_netfs_requests;

// Send a request, wait for its answer. Returns the reply header (payload follows) or NULL.
static netfs_hdr_t *call(uint8_t op, uint32_t handle, uint64_t offset, const void *payload, uint16_t plen, uint16_t want) {
    static bool resolved;
    if (!net_active()) return NULL;
    if (!resolved && !(resolved = net_resolve_server())) return NULL;

    netfs_hdr_t *h = (netfs_hdr_t *)pkt;
    h->magic = NETFS_MAGIC;
    h->seq = ++seq;
    h->op = op;
    h->status = 0;
    h->len = payload ? plen : want;
    h->handle = handle;
    h->offset = offset;
    if (payload && plen) memcpy(pkt + sizeof(*h), payload, plen);
    size_t total = sizeof(*h) + (payload ? plen : 0);
    g_netfs_requests++;

    for (int attempt = 0; attempt < 200; attempt++) {
        if (attempt) g_netfs_retries++;
        net_send_server(NETFS_PORT, NETFS_PORT, pkt, total);
        uint64_t t0 = wyse_ticks_us();
        uint64_t timeout = attempt < 5 ? 20000 : 200000;
        while (wyse_ticks_us() - t0 < timeout) {
            size_t len;
            const uint8_t *p = net_poll(NETFS_PORT, &len);
            if (!p || len < sizeof(netfs_hdr_t)) continue;
            const netfs_hdr_t *r = (const netfs_hdr_t *)p;
            if (r->magic != NETFS_MAGIC || r->seq != h->seq) continue;
            if (len > sizeof(reply)) len = sizeof(reply);
            memcpy(reply, p, len);
            return (netfs_hdr_t *)reply;
        }
    }
    printf("netfs: no answer to op %u\n", op);
    return NULL;
}

static const char *strip(const TCHAR *path) {
    if (!strncmp(path, "SD:/", 4) || !strncmp(path, "0:/", 3)) path = strchr(path, '/') + 1;
    while (*path == '/') path++;
    return path;
}

static FRESULT path_call(uint8_t op, const TCHAR *path, uint8_t mode, netfs_hdr_t **out) {
    uint8_t buf[300];
    const char *p = strip(path);
    size_t l = strlen(p);
    if (l > sizeof(buf) - 2) return FR_INVALID_NAME;
    buf[0] = mode;
    memcpy(buf + 1, p, l + 1);
    netfs_hdr_t *r = call(op, 0, 0, buf, (uint16_t)(l + 2), 0);
    if (!r) return FR_NOT_READY;
    if (out) *out = r;
    return (FRESULT)r->status;
}

FRESULT netfs_open(FIL *fp, const TCHAR *path, BYTE mode) {
    netfs_hdr_t *r;
    memset(fp, 0, sizeof(*fp));
    FRESULT res = path_call(OP_OPEN, path, mode, &r);
    if (res != FR_OK) return res;
    fp->obj.fs = &netfs_vol;
    fp->obj.handle = r->handle;
    fp->obj.objsize = r->offset;
    fp->flag = mode;
    fp->fptr = (mode & FA_OPEN_APPEND) == FA_OPEN_APPEND ? r->offset : 0;
    return FR_OK;
}

FRESULT netfs_close(FIL *fp) {
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    netfs_hdr_t *r = call(OP_CLOSE, fp->obj.handle, 0, NULL, 0, 0);
    fp->obj.fs = NULL;
    return r ? (FRESULT)r->status : FR_NOT_READY;
}

// Up to BULK_CHUNKS chunks in one request: the server sends them back to back,
// each saying which it is. Returns the bytes placed at dst, or -1 when pieces
// went missing (the caller then reads the rest a chunk at a time).
#define BULK_CHUNKS 32
static int bulk_read(FIL *fp, uint8_t *dst, UINT want) {
    unsigned chunks = (want + NETFS_CHUNK - 1) / NETFS_CHUNK;
    if (chunks > BULK_CHUNKS) chunks = BULK_CHUNKS;
    netfs_hdr_t *h = (netfs_hdr_t *)pkt;
    h->magic = NETFS_MAGIC;
    h->seq = ++seq;
    h->op = OP_READB;
    h->status = 0;
    h->len = (uint16_t)chunks;
    h->handle = fp->obj.handle;
    h->offset = fp->fptr;
    g_netfs_requests++;
    net_send_server(NETFS_PORT, NETFS_PORT, pkt, sizeof(*h));
    uint64_t have = 0;              // bit per piece
    unsigned total = 0, got = 0;
    int bytes = 0;
    uint64_t last = wyse_ticks_us();
    while (wyse_ticks_us() - last < 20000) {
        size_t len;
        const uint8_t *p = net_poll(NETFS_PORT, &len);
        if (!p || len < sizeof(netfs_hdr_t)) continue;
        const netfs_hdr_t *r = (const netfs_hdr_t *)p;
        if (r->magic != NETFS_MAGIC || r->seq != h->seq || r->op != OP_READB) continue;
        if (r->status) return -1;
        unsigned idx = r->handle >> 16;
        total = r->handle & 0xffff;
        last = wyse_ticks_us();
        if (idx >= chunks || (have >> idx & 1) || r->len > len - sizeof(*r)) continue;
        uint64_t at = r->offset - fp->fptr;
        if (at + r->len > want) continue;
        memcpy(dst + at, p + sizeof(*r), r->len);
        have |= 1ull << idx;
        bytes += r->len;
        if (++got == total) return bytes;
    }
    g_netfs_retries++;
    return -1;
}

FRESULT netfs_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    uint8_t *dst = buff;
    *br = 0;
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    while (btr >= 2 * NETFS_CHUNK) {
        UINT want = btr < BULK_CHUNKS * NETFS_CHUNK ? btr : BULK_CHUNKS * NETFS_CHUNK;
        int got = bulk_read(fp, dst, want);
        if (got < 0) break;         // the chunk-at-a-time path below finishes the job
        dst += got;
        fp->fptr += got;
        *br += got;
        btr -= got;
        if ((UINT)got < want) return FR_OK;     // end of file
    }
    while (btr) {
        uint16_t want = btr > NETFS_CHUNK ? NETFS_CHUNK : (uint16_t)btr;
        netfs_hdr_t *r = call(OP_READ, fp->obj.handle, fp->fptr, NULL, 0, want);
        if (!r) return FR_DISK_ERR;
        if (r->status) return (FRESULT)r->status;
        uint16_t got = r->len > want ? want : r->len;
        memcpy(dst, reply + sizeof(netfs_hdr_t), got);
        dst += got;
        fp->fptr += got;
        *br += got;
        btr -= got;
        if (got < want) break;      // end of file
    }
    return FR_OK;
}

/* Pipeline independent positional writes. Retrying a lost acknowledgement is
 * safe: the same bytes are rewritten at the same offset, never appended twice.
 * The existing server's OP_WRITE remains the on-wire protocol. */
FRESULT netfs_write(FIL *fp,const void *buff,UINT btw,UINT *bw) {
    const uint8_t *src=buff; *bw=0;
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    while (btw) {
        unsigned chunks=(btw+NETFS_CHUNK-1)/NETFS_CHUNK;
        if (chunks>32) chunks=32;
        uint32_t first=seq+1; seq+=chunks;
        uint32_t have=0, all=chunks==32?~0u:(1u<<chunks)-1;
        unsigned bytes=btw<chunks*NETFS_CHUNK?btw:chunks*NETFS_CHUNK;
        for (unsigned attempt=0;attempt<200 && have!=all;attempt++) {
            for (unsigned i=0;i<chunks;i++) if (!(have&(1u<<i))) {
                unsigned off=i*NETFS_CHUNK, len=bytes-off<NETFS_CHUNK?bytes-off:NETFS_CHUNK;
                netfs_hdr_t *h=(netfs_hdr_t *)pkt;
                *h=(netfs_hdr_t){NETFS_MAGIC,first+i,OP_WRITE,0,(uint16_t)len,fp->obj.handle,fp->fptr+off};
                memcpy(pkt+sizeof(*h),src+off,len);
                net_send_server(NETFS_PORT,NETFS_PORT,pkt,sizeof(*h)+len);
                g_netfs_requests++; if (attempt) g_netfs_retries++;
            }
            uint64_t until=wyse_ticks_us()+(attempt<5?20000:200000);
            while (have!=all && wyse_ticks_us()<until) {
                size_t len; const uint8_t *p=net_poll(NETFS_PORT,&len);
                if (!p || len<sizeof(netfs_hdr_t)) continue;
                const netfs_hdr_t *r=(const netfs_hdr_t *)p;
                uint32_t i=r->seq-first;
                if (r->magic!=NETFS_MAGIC || r->op!=OP_WRITE || i>=chunks) continue;
                if (r->status) return (FRESULT)r->status;
                have|=1u<<i;
            }
        }
        if (have!=all) return FR_DISK_ERR;
        fp->fptr+=bytes; *bw+=bytes; btw-=bytes; src+=bytes;
        if (fp->fptr>fp->obj.objsize) fp->obj.objsize=fp->fptr;
    }
    return FR_OK;
}

FRESULT netfs_lseek(FIL *fp, FSIZE_t ofs) {
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    if (ofs == CREATE_LINKMAP) return FR_OK;
    fp->fptr = ofs;
    return FR_OK;
}

FRESULT netfs_sync(FIL *fp) {
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    netfs_hdr_t *r = call(OP_SYNC, fp->obj.handle, 0, NULL, 0, 0);
    return r ? (FRESULT)r->status : FR_NOT_READY;
}

FRESULT netfs_truncate(FIL *fp) {
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    netfs_hdr_t *r = call(OP_TRUNCATE, fp->obj.handle, fp->fptr, NULL, 0, 0);
    if (r && !r->status) fp->obj.objsize = fp->fptr;
    return r ? (FRESULT)r->status : FR_NOT_READY;
}

FRESULT netfs_opendir(DIR *dp, const TCHAR *path) {
    netfs_hdr_t *r;
    memset(dp, 0, sizeof(*dp));
    FRESULT res = path_call(OP_OPENDIR, path, 0, &r);
    if (res != FR_OK) return res;
    dp->obj.fs = &netfs_vol;
    dp->obj.handle = r->handle;
    return FR_OK;
}

FRESULT netfs_readdir(DIR *dp, FILINFO *fno) {
    memset(fno, 0, sizeof(*fno));
    if (!dp->obj.fs) return FR_INVALID_OBJECT;
    netfs_hdr_t *r = call(OP_READDIR, dp->obj.handle, 0, NULL, 0, 0);
    if (!r) return FR_DISK_ERR;
    if (r->status) return (FRESULT)r->status;
    if (!r->len) return FR_OK;      // end of directory: empty fname
    const uint8_t *p = reply + sizeof(netfs_hdr_t);
    fno->fattrib = p[0];
    memcpy(&fno->fsize, p + 1, 8);
    size_t l = r->len - 9;
    if (l > sizeof(fno->fname) - 1) l = sizeof(fno->fname) - 1;
    memcpy(fno->fname, p + 9, l);
    fno->fname[l] = 0;
    return FR_OK;
}

FRESULT netfs_closedir(DIR *dp) {
    if (!dp->obj.fs) return FR_INVALID_OBJECT;
    netfs_hdr_t *r = call(OP_CLOSEDIR, dp->obj.handle, 0, NULL, 0, 0);
    dp->obj.fs = NULL;
    return r ? (FRESULT)r->status : FR_NOT_READY;
}

FRESULT netfs_stat(const TCHAR *path, FILINFO *fno) {
    netfs_hdr_t *r;
    FRESULT res = path_call(OP_STAT, path, 0, &r);
    if (res != FR_OK) return res;
    if (fno) {
        memset(fno, 0, sizeof(*fno));
        fno->fsize = r->offset;
        fno->fattrib = (uint8_t)r->handle;
        const char *p = strip(path), *s = strrchr(p, '/');
        strncpy(fno->fname, s ? s + 1 : p, sizeof(fno->fname) - 1);
    }
    return FR_OK;
}

FRESULT netfs_unlink(const TCHAR *path) { return path_call(OP_UNLINK, path, 0, NULL); }
FRESULT netfs_mkdir(const TCHAR *path) { return path_call(OP_MKDIR, path, 0, NULL); }

FRESULT netfs_rename(const TCHAR *old, const TCHAR *new_) {
    uint8_t buf[600];
    const char *a = strip(old), *b = strip(new_);
    size_t la = strlen(a), lb = strlen(b);
    if (la + lb + 3 > sizeof(buf)) return FR_INVALID_NAME;
    buf[0] = 0;
    memcpy(buf + 1, a, la + 1);
    memcpy(buf + 2 + la, b, lb + 1);
    netfs_hdr_t *r = call(OP_RENAME, 0, 0, buf, (uint16_t)(la + lb + 3), 0);
    return r ? (FRESULT)r->status : FR_NOT_READY;
}
