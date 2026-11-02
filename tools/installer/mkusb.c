// mkusb: build the wyseDOS installation USB image on the development PC.
//
//   mkusb <image> <staging-dir> [size-MB]
//
// The image is a whole disk: an MBR with one FAT partition (type 0x0C, from
// 1 MB) holding everything under the staging directory - EFI/BOOT/BOOTX64.EFI,
// install.ini and 386/. Write it to a stick with Rufus (DD mode), balenaEtcher
// or dd. Built with the same FatFs as the Wyse side (efi/fatfs), natively.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <io.h>
#include "ff.h"
#include "diskio.h"

static FILE *img;
static uint64_t part_first, part_count;     // in 512-byte sectors

DSTATUS disk_status(BYTE pdrv) { return img ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (sector + count > part_count) return RES_PARERR;
    _fseeki64(img, (long long)(part_first + sector) * 512, SEEK_SET);
    size_t got = fread(buff, 512, count, img);
    if (got < count) memset(buff + got * 512, 0, (count - got) * 512);   // past the end written so far
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (sector + count > part_count) return RES_PARERR;
    _fseeki64(img, (long long)(part_first + sector) * 512, SEEK_SET);
    return fwrite(buff, 512, count, img) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    switch (cmd) {
    case CTRL_SYNC: fflush(img); return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)part_count; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 2048; return RES_OK;
    default: return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    return (DWORD)(t->tm_year - 80) << 25 | (DWORD)(t->tm_mon + 1) << 21 | (DWORD)t->tm_mday << 16 |
           (DWORD)t->tm_hour << 11 | (DWORD)t->tm_min << 5 | (DWORD)t->tm_sec / 2;
}
void *ff_memalloc(UINT n) { return malloc(n); }
void ff_memfree(void *p) { free(p); }

static uint64_t tree_bytes(const char *dir) {
    char pat[600];
    snprintf(pat, sizeof(pat), "%s/*", dir);
    struct __finddata64_t fd;
    intptr_t h = _findfirst64(pat, &fd);
    uint64_t sum = 0;
    if (h == -1) return 0;
    do {
        if (!strcmp(fd.name, ".") || !strcmp(fd.name, "..")) continue;
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", dir, fd.name);
        sum += (fd.attrib & _A_SUBDIR) ? tree_bytes(p) + 65536 : (uint64_t)fd.size + 32768;
    } while (_findnext64(h, &fd) == 0);
    _findclose(h);
    return sum;
}

static int copy_in(const char *from, const char *to) {
    static unsigned char buf[1 << 20];
    FILE *in = fopen(from, "rb");
    if (!in) { fprintf(stderr, "cannot read %s\n", from); return 0; }
    _fseeki64(in, 0, SEEK_END);
    uint64_t size = (uint64_t)_ftelli64(in);
    _fseeki64(in, 0, SEEK_SET);
    FIL f;
    if (f_open(&f, to, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { fclose(in); fprintf(stderr, "cannot create %s\n", to); return 0; }
    if (size >= (1u << 20) && f_expand(&f, (FSIZE_t)size, 1) != FR_OK) {
        fprintf(stderr, "no room for %s\n", to);
        fclose(in);
        f_close(&f);
        return 0;
    }
    int ok = 1;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (!n) break;
        UINT put;
        if (f_write(&f, buf, (UINT)n, &put) != FR_OK || put != n) { ok = 0; break; }
    }
    fclose(in);
    if (f_close(&f) != FR_OK) ok = 0;
    printf("  %-40s %12llu\n", to, (unsigned long long)size);
    return ok;
}

static int copy_tree(const char *from, const char *to) {
    char pat[600];
    snprintf(pat, sizeof(pat), "%s/*", from);
    struct __finddata64_t fd;
    intptr_t h = _findfirst64(pat, &fd);
    if (h == -1) return 1;
    int ok = 1;
    do {
        if (!strcmp(fd.name, ".") || !strcmp(fd.name, "..")) continue;
        char a[600], b[600];
        snprintf(a, sizeof(a), "%s/%s", from, fd.name);
        snprintf(b, sizeof(b), "%s%s%s", to, to[0] ? "/" : "", fd.name);
        if (fd.attrib & _A_SUBDIR) {
            f_mkdir(b);
            ok = copy_tree(a, b);
        } else {
            ok = copy_in(a, b);
        }
    } while (ok && _findnext64(h, &fd) == 0);
    _findclose(h);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mkusb <image> <source>=<path-in-image>... [--size MB]\n");
        return 2;
    }
    uint64_t need = 0, mb = 0;
    int floppy = 0;     // --floppy: a 1.44 MB diskette image, FAT12, no partition table
    int dos = 0;        // --dos: a hard disk for MS-DOS: FAT16 from sector 63, 16 heads, 63 sectors
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--floppy")) { floppy = 1; continue; }
        if (!strcmp(argv[i], "--dos")) { dos = 1; continue; }
        if (!strcmp(argv[i], "--size") && i + 1 < argc) { mb = strtoull(argv[++i], NULL, 10); continue; }
        char src[600];
        snprintf(src, sizeof(src), "%s", argv[i]);
        char *eq = strchr(src, '=');
        if (eq) *eq = 0;
        struct _stat64 st;
        if (_stat64(src, &st)) { fprintf(stderr, "no such source: %s\n", src); return 1; }
        need += (st.st_mode & _S_IFDIR) ? tree_bytes(src) : (uint64_t)st.st_size + 32768;
    }
    if (!mb) mb = (need >> 20) + (need >> 24) + 64;
    if (mb < 64) mb = 64;
    uint64_t total = floppy ? 2880 : mb << 11;  // sectors
    part_first = floppy ? 0 : 2048;
    if (dos) {
        // whole cylinders of 16 heads and 63 sectors, under the 504 MB that
        // needs no BIOS translation, and the partition where DOS's FDISK puts it
        uint64_t cyl = total / (16 * 63);
        if (cyl > 1024) cyl = 1024;
        total = cyl * 16 * 63;
        part_first = 63;
    }
    part_count = total - part_first;

    img = fopen(argv[1], "w+b");
    if (!img) { fprintf(stderr, "cannot create %s\n", argv[1]); return 1; }
    // the whole size at once, so readers see a disk of the right length
    _fseeki64(img, (long long)total * 512 - 1, SEEK_SET);
    fputc(0, img);

    if (!floppy) {
    unsigned char mbr[512] = {0};
    unsigned char *e = mbr + 446;
    e[0] = 0x80;                                // active
    e[1] = 0xfe; e[2] = 0xff; e[3] = 0xff;      // CHS: use LBA
    e[4] = 0x0c;                                // FAT32 LBA
    e[5] = 0xfe; e[6] = 0xff; e[7] = 0xff;
    if (dos) {
        // a data disk for DOS: FAT16, real CHS fields, not the boot partition
        uint32_t last = (uint32_t)(total - 1), c = last / (16 * 63), h = last / 63 % 16, sct = last % 63 + 1;
        e[0] = 0;
        e[1] = 1; e[2] = 1; e[3] = 0;
        e[4] = 0x06;
        e[5] = (unsigned char)h; e[6] = (unsigned char)(sct | (c >> 8) << 6); e[7] = (unsigned char)c;
    }
    uint32_t first = (uint32_t)part_first, count = (uint32_t)part_count;
    memcpy(e + 8, &first, 4);
    memcpy(e + 12, &count, 4);
    srand((unsigned)time(NULL));
    uint32_t id = (uint32_t)rand() << 16 ^ (uint32_t)rand();
    memcpy(mbr + 440, &id, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    _fseeki64(img, 0, SEEK_SET);
    fwrite(mbr, 1, 512, img);
    }
    uint32_t first = (uint32_t)part_first;

    static BYTE work[32768];
    MKFS_PARM opt = {FM_FAT | FM_FAT32 | FM_SFD, 2, 0, 0, part_count >= 0x400000 ? 32768 : 0};
    if (floppy) {
        MKFS_PARM f = {FM_FAT | FM_SFD, 2, 1, 224, 512};
        opt = f;
    }
    if (dos) {
        MKFS_PARM f = {FM_FAT | FM_SFD, 2, 1, 512, 0};
        opt = f;
    }
    FATFS fs;
    if (f_mkfs("0:", &opt, work, sizeof(work)) != FR_OK) {
        fprintf(stderr, "formatting failed\n");
        return 1;
    }
    if (floppy) {
        // what a 1.44 MB diskette says about itself: the BIOS and DOS go by
        // these, and FatFs writes a hard disk's 63 sectors and 255 heads
        unsigned char bs[512];
        _fseeki64(img, 0, SEEK_SET);
        fread(bs, 1, 512, img);
        bs[21] = 0xf0;
        bs[24] = 18; bs[25] = 0;
        bs[26] = 2; bs[27] = 0;
        _fseeki64(img, 0, SEEK_SET);
        fwrite(bs, 1, 512, img);
        unsigned rsvd = bs[14] | bs[15] << 8, fatsz = bs[22] | bs[23] << 8;
        for (int k = 0; k < bs[16]; k++) {
            _fseeki64(img, (long long)(rsvd + k * fatsz) * 512, SEEK_SET);
            fputc(0xf0, img);
        }
    }
    if (f_mount(&fs, "0:", 1) != FR_OK) {
        fprintf(stderr, "formatting failed\n");
        return 1;
    }
    f_setlabel(dos ? "0:GAMES" : "0:WYSEDOS");
    // FatFs wrote the volume as if it started at sector 0; FAT readers want
    // the partition's offset in the boot sector's hidden sectors field
    unsigned char vbr[512];
    _fseeki64(img, (long long)part_first * 512, SEEK_SET);
    fread(vbr, 1, 512, img);
    memcpy(vbr + 28, &first, 4);     // (0 on a diskette)
    if (dos) {
        // the geometry DOS reaches it through, and a hard disk's drive number
        vbr[21] = 0xf8;
        vbr[24] = 63; vbr[25] = 0;
        vbr[26] = 16; vbr[27] = 0;
        vbr[36] = 0x80;
    }
    _fseeki64(img, (long long)part_first * 512, SEEK_SET);
    fwrite(vbr, 1, 512, img);
    if (vbr[82] == 'F' && vbr[83] == 'A' && vbr[84] == 'T' && vbr[85] == '3') {
        // FAT32 keeps a backup boot sector at sector 6
        _fseeki64(img, (long long)(part_first + 6) * 512, SEEK_SET);
        fwrite(vbr, 1, 512, img);
    }

    printf("%s: %llu MB\n", argv[1], (unsigned long long)mb);
    int ok = 1;
    for (int i = 2; ok && i < argc; i++) {
        if (!strcmp(argv[i], "--size")) { i++; continue; }
        if (!strcmp(argv[i], "--floppy") || !strcmp(argv[i], "--dos")) continue;
        char src[600];
        snprintf(src, sizeof(src), "%s", argv[i]);
        char *eq = strchr(src, '='), *dst = eq ? eq + 1 : "";
        if (eq) *eq = 0;
        // the directories on the way to the destination
        char dir[600];
        for (char *s = strchr(dst, '/'); s; s = strchr(s + 1, '/')) {
            snprintf(dir, sizeof(dir), "%.*s", (int)(s - dst), dst);
            f_mkdir(dir);
        }
        struct _stat64 st;
        _stat64(src, &st);
        if (st.st_mode & _S_IFDIR) {
            if (dst[0]) f_mkdir(dst);
            ok = copy_tree(src, dst);
        } else {
            ok = copy_in(src, dst);
        }
    }
    f_mount(NULL, "0:", 0);
    fclose(img);
    if (!ok) { fprintf(stderr, "failed\n"); return 1; }
    printf("done\n");
    return 0;
}
