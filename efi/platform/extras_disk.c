#include "lib.h"
#include "ff.h"
#include "fatvol.h"
#include "native_disk.h"
#include "installer.h"
#include "extras_disk.h"
#include "extras_ntfs.h"

enum { MAX_PARTITIONS = 32 };
typedef struct {
    uint64_t start, count;
    uint8_t type;
    bool ntfs;
    char name[16];
} Partition;
static BlockDev disk, view;
static Partition parts[MAX_PARTITIONS];
static unsigned count, mounted;
static bool esp;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool extended(uint8_t type) { return type == 0x05 || type == 0x0f || type == 0x85; }

static bool fat(uint8_t type) {
    switch (type) {
    case 0x01: case 0x04: case 0x06: case 0x0b: case 0x0c: case 0x0e:
    case 0x11: case 0x14: case 0x16: case 0x1b: case 0x1c: case 0x1e:
        return true;
    default: return false;
    }
}

static bool add_part(uint64_t start, uint64_t sectors, uint8_t type) {
    if (!start || !sectors || count == MAX_PARTITIONS) return false;
    for (unsigned i = 0; i < count; i++)
        if (start < parts[i].start + parts[i].count && parts[i].start < start + sectors) return false;
    Partition *p = &parts[count++];
    *p = (Partition){.start = start, .count = sectors, .type = type};
    return true;
}

static bool read_table(uint64_t lba, uint8_t *buf) {
    return blk_pread(&disk, lba * 512, buf, 512) == 512 && buf[510] == 0x55 && buf[511] == 0xaa;
}

static bool scan_partitions(void) {
    uint8_t buf[512];
    if (!read_table(0, buf)) return false;
    uint64_t size = blk_size(&disk) / 512, chain = 0, limit = 0;
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *entry = buf + 446 + i * 16;
        uint8_t type = entry[4];
        uint64_t start = read32(entry + 8), sectors = read32(entry + 12);
        if (!type) continue;
        if (!start || start >= size || !sectors || sectors > size - start || type == 0xee) return false;
        if (extended(type)) {
            if (chain) return false;
            chain = start;
            limit = chain + sectors;
        } else if (!add_part(start, sectors, type)) return false;
    }
    if (!chain) return true;
    for (unsigned i = 0; i < count; i++)
        if (parts[i].start < limit && chain < parts[i].start + parts[i].count) return false;
    uint64_t next = chain, occupied_until = chain;
    for (unsigned i = 0; i < MAX_PARTITIONS; i++) {
        if (next < occupied_until || next >= limit || !read_table(next, buf)) return false;
        const uint8_t *logical = buf + 446, *link = logical + 16;
        if (logical[4]) {
            uint64_t start = next + read32(logical + 8), sectors = read32(logical + 12);
            if (extended(logical[4]) || start <= next || start >= limit ||
                sectors > limit - start || !add_part(start, sectors, logical[4])) return false;
            occupied_until = start + sectors;
        } else occupied_until = next + 1;
        if (buf[482] || buf[498]) return false;
        if (!link[4]) return true;
        if (!extended(link[4])) return false;
        next = chain + read32(link + 8);
    }
    return false;
}

static uint64_t view_size(BlockDev *dev) { return ((Partition *)dev->ctx)->count * 512; }
static int view_seek(BlockDev *dev, uint64_t pos) {
    if (pos > view_size(dev) || pos % 512) return -1;
    dev->pos = pos;
    return 0;
}

static int view_io(BlockDev *dev, void *data, unsigned len, bool write) {
    Partition *p = dev->ctx;
    if (len % 512 || len > view_size(dev) - dev->pos) return -1;
    uint64_t off = p->start * 512 + dev->pos;
    int result = write ? blk_pwrite(&disk, off, data, len) : blk_pread(&disk, off, data, len);
    if (result > 0) dev->pos += result;
    return result;
}
static int view_read(BlockDev *d, void *buf, unsigned len) { return view_io(d, buf, len, false); }
static int view_write(BlockDev *d, const void *buf, unsigned len) { return view_io(d, (void *)buf, len, true); }
static int view_sync(BlockDev *d) { return blk_sync(&disk); }

static void unmount(void) {
    if (mounted) {
        if (parts[mounted - 1].ntfs) extras_ntfs_unmount();
        else fatvol_detach(FATVOL_EXTRAS);
    }
    mounted = 0;
}

static bool mount(unsigned i) {
    if (mounted == i + 1) return true;
    unmount();
    view = (BlockDev){.ctx = &parts[i], .read = view_read, .write = view_write, .seek = view_seek,
        .size = view_size, .sync = view_sync, .readonly = disk.readonly};
    if (parts[i].ntfs) {
        if (!extras_ntfs_mount(&view)) return false;
    } else if (fatvol_attach_block(FATVOL_EXTRAS, &view) != FR_OK) {
        fatvol_detach(FATVOL_EXTRAS);
        return false;
    }
    mounted = i + 1;
    return true;
}

void extras_disk_open(void) {
    esp = installer_mount_existing_esp();
    count = 0;
    if (!native_disk_bind(&disk) || !scan_partitions()) {
        count = 0;
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        uint8_t sector[512];
        if (parts[i].type == 0x07 || parts[i].type == 0x17)
            parts[i].ntfs = read_table(parts[i].start, sector) && !memcmp(sector + 3, "NTFS    ", 8);
        if ((parts[i].ntfs || fat(parts[i].type)) && mount(i))
            snprintf(parts[i].name, sizeof(parts[i].name), "disk%u", i + 1);
    }
}

void extras_disk_close(void) {
    unmount();
    extras_ntfs_shutdown();
    if (esp) fatvol_detach(FATVOL_TARGET_ESP);
    blk_sync(&disk);
    mounted = count = 0;
    esp = false;
}

const char *extras_disk_name(unsigned i) {
    if (!i) return esp ? "wysedos" : "";
    if (--i >= count) return NULL;
    return parts[i].name;
}

bool extras_disk_resolve(const char *cwd, const char *arg, char *out) {
    if (strlen(arg) + (*arg == '/' ? 0 : strlen(cwd) + 1) >= FTP_PATH) return false;
    char joined[FTP_PATH];
    if (*arg == '/') strcpy(joined, arg);
    else snprintf(joined, sizeof(joined), "%s/%s", cwd, arg);
    size_t n = 0;
    const char *s = joined;
    while (*s) {
        while (*s == '/') s++;
        const char *start = s;
        while (*s && *s != '/') {
            unsigned char c = *s++;
            if (c < 32 || c == 127 || strchr("\\:*?\"<>|", c)) return false;
        }
        size_t length = s - start;
        if (!length || (length == 1 && *start == '.')) continue;
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            while (n && out[n - 1] != '/') n--;
            if (n) n--;
            continue;
        }
        if (start[length - 1] == ' ' || start[length - 1] == '.' || *start == ' ') return false;
        out[n++] = '/';
        memcpy(out + n, start, length);
        n += length;
    }
    if (!n) out[n++] = '/';
    out[n] = 0;
    return true;
}

static bool private_config(const char *name) {
    return !strncasecmp(name, "EXTRAS.", 7) &&
        (!strcasecmp(name + 7, "CFG") || !strcasecmp(name + 7, "NEW") || !strcasecmp(name + 7, "BAK"));
}

bool extras_disk_path(const char *path, bool write, bool esp_write, char *out) {
    if (!path[1]) return false;
    const char *tail = strchr(path + 1, '/');
    size_t len = tail ? (size_t)(tail - path - 1) : strlen(path + 1);
    if (!tail) tail = "/";
    unsigned vol;
    if (esp && len == 7 && !strncasecmp(path + 1, "wysedos", len)) {
        if ((write && !esp_write) || private_config(tail + 1)) return false;
        vol = FATVOL_TARGET_ESP;
    } else {
        unsigned i = 0;
        for (; i < count; i++)
            if (strlen(parts[i].name) == len && !strncasecmp(path + 1, parts[i].name, len)) break;
        if (i == count || (write && disk.readonly) || !mount(i)) return false;
        if (parts[i].ntfs && write &&
            (!extras_ntfs_writable() || !strcasecmp(tail + 1, "hiberfil.sys") ||
             tail[1] == '$')) return false;
        vol = parts[i].ntfs ? EXTRAS_NTFS_VOLUME : FATVOL_EXTRAS;
    }
    return snprintf(out, FTP_PATH, "%u:%s", vol, tail) < FTP_PATH;
}
