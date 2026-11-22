#include "lib.h"
#include "extras_disk.h"
#include "extras_ntfs.h"

typedef struct EfiFile EfiFile;
struct EfiFile {
    uint64_t revision;
    EFI_STATUS (*open)(EfiFile *, EfiFile **, const CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (*close)(EfiFile *);
    EFI_STATUS (*delete)(EfiFile *);
    EFI_STATUS (*read)(EfiFile *, uint64_t *, void *);
    EFI_STATUS (*write)(EfiFile *, uint64_t *, const void *);
    void *position;
    void *seek;
    EFI_STATUS (*get_info)(EfiFile *, const EFI_GUID *, uint64_t *, void *);
    EFI_STATUS (*set_info)(EfiFile *, const EFI_GUID *, uint64_t, const void *);
    EFI_STATUS (*flush)(EfiFile *);
};
typedef struct SimpleFs SimpleFs;
struct SimpleFs {
    uint64_t revision;
    EFI_STATUS (*open_volume)(SimpleFs *, EfiFile **);
};
typedef struct {
    uint64_t record_size, file_size, physical_size;
    EFI_TIME created, accessed, modified;
    uint64_t attributes;
    CHAR16 name[FTP_PATH];
} FileInfo;
typedef struct {
    uint64_t record_size;
    bool readonly;
    uint64_t volume_size, free_space;
    uint32_t block_size;
    CHAR16 label[FTP_PATH];
} FsInfo;
typedef struct DiskIo DiskIo;
struct DiskIo {
    uint64_t revision;
    EFI_STATUS (*read)(DiskIo *, uint32_t, uint64_t, uint64_t, void *);
    EFI_STATUS (*write)(DiskIo *, uint32_t, uint64_t, uint64_t, const void *);
};

static const EFI_GUID block_id = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID path_id = EFI_DEVICE_PATH_PROTOCOL_GUID;
static const EFI_GUID disk_id = {0xce345171,0xba0b,0x11d2,{0x8e,0x4f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const EFI_GUID fs_id = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const EFI_GUID file_id = {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const EFI_GUID fsinfo_id = {0x09576e93,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const struct __attribute__((packed)) {
    EFI_DEVICE_PATH vendor;
    EFI_GUID id;
    EFI_DEVICE_PATH end;
} partition_path = {
    {DP_HARDWARE, 4, 20}, {0x3902812f,0x987d,0x4eb1,{0x95,0xf6,0x15,0x0e,0x5a,0x93,0x73,0xab}},
    {DP_END, 0xff, 4}
};

static EFI_HANDLE driver, handle;
static EFI_BLOCK_IO_MEDIA media;
static BlockDev *disk;
static EfiFile *root;
static bool writable;
static enum { DRIVER_UNTRIED, DRIVER_ACTIVE, DRIVER_UNAVAILABLE } driver_state;
static unsigned installed;

static EFI_STATUS disk_transfer(uint32_t id, uint64_t offset, uint64_t size, void *buffer, bool write) {
    uint64_t bytes = blk_size(disk);
    if (!disk || !media.MediaPresent || id != media.MediaId) return EFI_MEDIA_CHANGED;
    if (offset > bytes || size > bytes - offset || (!buffer && size)) return EFI_INVALID_PARAMETER;
    if (write && media.ReadOnly) return EFI_WRITE_PROTECTED;
    uint8_t *p = buffer;
    while (size) {
        if ((offset & 511) || size < 512) {
            uint8_t sector[512];
            unsigned start = offset & 511, n = 512 - start;
            if (n > size) n = size;
            if (blk_pread(disk, offset - start, sector, sizeof(sector)) != sizeof(sector))
                return EFI_DEVICE_ERROR;
            if (write) {
                memcpy(sector + start, p, n);
                if (blk_pwrite(disk, offset - start, sector, sizeof(sector)) != sizeof(sector))
                    return EFI_DEVICE_ERROR;
            } else memcpy(p, sector + start, n);
            offset += n;
            p += n;
            size -= n;
        } else {
            unsigned n = size >= 1024 * 1024 ? 1024 * 1024 : (unsigned)size & ~511u;
            int done = write ? blk_pwrite(disk, offset, p, n) : blk_pread(disk, offset, p, n);
            if (done != (int)n) return EFI_DEVICE_ERROR;
            offset += n;
            p += n;
            size -= n;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS disk_read(DiskIo *self, uint32_t id, uint64_t offset, uint64_t size, void *buf) {
    return disk_transfer(id, offset, size, buf, false);
}
static EFI_STATUS disk_write(DiskIo *self, uint32_t id, uint64_t offset, uint64_t size, const void *buf) {
    return disk_transfer(id, offset, size, (void *)buf, true);
}
static EFI_STATUS block_reset(EFI_BLOCK_IO_PROTOCOL *self, bool verify) {
    return EFI_SUCCESS;
}
static EFI_STATUS block_read(EFI_BLOCK_IO_PROTOCOL *self, uint32_t id, uint64_t lba, uint64_t size, void *buf) {
    if (lba > media.LastBlock || (size & 511)) return EFI_INVALID_PARAMETER;
    return disk_transfer(id, lba * 512, size, buf, false);
}
static EFI_STATUS block_write(EFI_BLOCK_IO_PROTOCOL *self, uint32_t id, uint64_t lba, uint64_t size, const void *buf) {
    if (lba > media.LastBlock || (size & 511)) return EFI_INVALID_PARAMETER;
    return disk_transfer(id, lba * 512, size, (void *)buf, true);
}
static EFI_STATUS block_flush(EFI_BLOCK_IO_PROTOCOL *self) {
    return !media.ReadOnly && blk_sync(disk) < 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_BLOCK_IO_PROTOCOL block = {0x10000, &media, block_reset, block_read, block_write, block_flush};
static DiskIo disk_io = {0x10000, disk_read, disk_write};

static bool load_driver(void) {
    if (driver_state != DRIVER_UNTRIED) return driver_state == DRIVER_ACTIVE;
    driver_state = DRIVER_UNAVAILABLE;
    FIL file;
    if (f_open(&file, "386/ntfs.efi", FA_READ) != FR_OK) return false;
    uint64_t size = f_size(&file);
    void *image = size && size <= 2 * 1024 * 1024 ? malloc(size) : NULL;
    unsigned got;
    bool ok = image && f_read(&file, image, (unsigned)size, &got) == FR_OK && got == size;
    if (f_close(&file) != FR_OK) ok = false;
    typedef EFI_STATUS (*LoadImage)(bool, EFI_HANDLE, void *, void *, uint64_t, EFI_HANDLE *);
    EFI_STATUS status = ok ? ((LoadImage)gBS->LoadImage)(false, gImage, NULL, image, size, &driver) : EFI_LOAD_ERROR;
    free(image);
    if (!EFI_ERROR(status)) {
        typedef EFI_STATUS (*StartImage)(EFI_HANDLE, uint64_t *, CHAR16 **);
        status = ((StartImage)gBS->StartImage)(driver, NULL, NULL);
    }
    if (EFI_ERROR(status)) {
        printf("extras: NTFS driver load failed (%llx)\n", status);
        extras_ntfs_shutdown();
        driver_state = DRIVER_UNAVAILABLE;
        return false;
    }
    driver_state = DRIVER_ACTIVE;
    return true;
}

void extras_ntfs_unmount(void) {
    if (root) root->close(root);
    root = NULL;
    writable = false;
    if (!handle) return;
    typedef EFI_STATUS (*Disconnect)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE);
    ((Disconnect)gBS->DisconnectController)(handle, driver, NULL);
    if (disk && !media.ReadOnly) blk_sync(disk);
    typedef EFI_STATUS (*Uninstall)(EFI_HANDLE, const EFI_GUID *, void *);
    Uninstall uninstall = (Uninstall)gBS->UninstallProtocolInterface;
    disk = NULL;
    if ((installed & 4) && !EFI_ERROR(uninstall(handle, &disk_id, &disk_io))) installed &= ~4u;
    if ((installed & 2) && !EFI_ERROR(uninstall(handle, &block_id, &block))) installed &= ~2u;
    if ((installed & 1) && !EFI_ERROR(uninstall(handle, &path_id, (void *)&partition_path))) installed &= ~1u;
    if (!installed) handle = NULL;
}

void extras_ntfs_shutdown(void) {
    extras_ntfs_unmount();
    if (handle) return; // A firmware consumer still owns this handle.
    if (driver) {
        typedef EFI_STATUS (*Unload)(EFI_HANDLE);
        if (EFI_ERROR(((Unload)gBS->UnloadImage)(driver))) return;
    }
    driver = NULL;
    driver_state = DRIVER_UNTRIED;
}

static bool bind_partition(BlockDev *partition, bool readonly) {
    if (handle) return false;
    disk = partition;
    media = (EFI_BLOCK_IO_MEDIA){.MediaId = 1, .MediaPresent = true, .LogicalPartition = true,
        .ReadOnly = readonly, .BlockSize = 512, .IoAlign = 1, .LastBlock = blk_size(partition) / 512 - 1};
    if (EFI_ERROR(gBS->InstallProtocolInterface(&handle, &path_id, EFI_NATIVE_INTERFACE, (void *)&partition_path))) goto fail;
    installed |= 1;
    if (EFI_ERROR(gBS->InstallProtocolInterface(&handle, &block_id, EFI_NATIVE_INTERFACE, &block))) goto fail;
    installed |= 2;
    if (EFI_ERROR(gBS->InstallProtocolInterface(&handle, &disk_id, EFI_NATIVE_INTERFACE, &disk_io))) goto fail;
    installed |= 4;
    EFI_HANDLE drivers[] = {driver, NULL};
    SimpleFs *fs;
    if (EFI_ERROR(gBS->ConnectController(handle, drivers, NULL, false)) ||
        EFI_ERROR(gBS->HandleProtocol(handle, &fs_id, (void **)&fs)) ||
        EFI_ERROR(fs->open_volume(fs, &root))) goto fail;
    FsInfo info;
    uint64_t size = sizeof(info);
    writable = !readonly && !EFI_ERROR(root->get_info(root, &fsinfo_id, &size, &info)) && !info.readonly;
    return true;
fail:
    extras_ntfs_unmount();
    return false;
}

/* The upstream UEFI binary checks the NTFS log but skips hiberfil.sys.
 * Probe through a write-protected block view before allowing writes. */
static bool clean_hibernation_header(void) {
    static const CHAR16 path[] = L"\\hiberfil.sys";
    EfiFile *file;
    EFI_STATUS status = root->open(root, &file, path, 1, 0);
    if (status == EFI_NOT_FOUND) return true;
    if (EFI_ERROR(status)) return false;
    uint8_t header[4096];
    uint64_t size = sizeof(header);
    status = file->read(file, &size, header);
    file->close(file);
    if (EFI_ERROR(status) || size != sizeof(header)) return false;
    for (unsigned i = 0; i < sizeof(header); i++) if (header[i]) return false;
    return true;
}

static uint16_t get16(const uint8_t *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}
static uint32_t get32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

/* Volume information is a resident attribute of system MFT record 3.
 * Check both the main and mirrored records without writing to either. */
static bool clean_volume_record(uint64_t at, unsigned bytes, unsigned sector) {
    uint8_t record[4096];
    if (EFI_ERROR(disk_transfer(media.MediaId, at, bytes, record, false)) ||
        memcmp(record, "FILE", 4)) return false;
    unsigned fixup = get16(record + 4), words = get16(record + 6);
    if (words != 1 + bytes / sector || fixup < 8 || fixup + 2 * words > sector - 2) return false;
    for (unsigned i = 1; i < words; i++) {
        unsigned tail = i * sector - 2;
        if (get16(record + tail) != get16(record + fixup)) return false;
        memcpy(record + tail, record + fixup + i * 2, 2);
    }
    unsigned end = get32(record + 24), attr = get16(record + 20);
    if (end > bytes || attr < fixup + 2 * words || attr >= end) return false;
    while (end - attr >= 24) {
        const uint8_t *p = record + attr;
        unsigned size = get32(p + 4);
        if (size < 24 || size > end - attr) return false;
        if (get32(p) == 0x70) {
            unsigned value = get16(p + 20), length = get32(p + 16);
            return !p[8] && value >= 24 && value <= size && length >= 12 &&
                length <= size - value && !(get16(p + value + 10) & 1);
        }
        attr += size;
    }
    return false;
}

static bool clean_volume_flags(void) {
    uint8_t boot[512];
    if (EFI_ERROR(disk_transfer(media.MediaId, 0, sizeof(boot), boot, false))) return false;
    unsigned sector = get16(boot + 11), per_cluster = boot[13];
    if (sector < 512 || sector > 4096 || (sector & (sector - 1)) ||
        !per_cluster || (per_cluster & (per_cluster - 1))) return false;
    unsigned cluster = sector * per_cluster;
    int code = (int8_t)boot[64];
    unsigned bytes = code < 0 ? (code >= -12 ? 1u << -code : 0) : cluster * code;
    if (bytes < sector || bytes > 4096 || (bytes & (bytes - 1))) return false;
    for (unsigned field = 48; field <= 56; field += 8) {
        uint64_t lcn = get64(boot + field);
        if (!lcn || lcn >= blk_size(disk) / cluster ||
            !clean_volume_record(lcn * cluster + 3 * bytes, bytes, sector)) return false;
    }
    return true;
}

bool extras_ntfs_mount(BlockDev *partition) {
    if (!load_driver() || !bind_partition(partition, true)) return false;
    if (partition->readonly || !clean_volume_flags() || !clean_hibernation_header()) return true;
    extras_ntfs_unmount();
    return bind_partition(partition, false) || bind_partition(partition, true);
}

bool extras_ntfs_writable(void) { return writable; }

static FRESULT result(EFI_STATUS status) {
    switch (status) {
    case EFI_SUCCESS: return FR_OK;
    case EFI_NOT_FOUND: return FR_NO_FILE;
    case EFI_INVALID_PARAMETER: return FR_INVALID_NAME;
    case EFI_WRITE_PROTECTED: return FR_WRITE_PROTECTED;
    case EFI_ERR(15): return FR_DENIED;
    case EFI_ERR(9): return FR_NOT_ENOUGH_CORE;
    default: return FR_DISK_ERR;
    }
}

static bool wide_path(const char *path, CHAR16 *wide) {
    const unsigned char *src = (const unsigned char *)path;
    unsigned n = 0;
    while (*src) {
        uint32_t c = *src++;
        if (c >= 0x80) {
            unsigned extra;
            uint32_t min;
            if (c < 0xc2 || c > 0xf4) return false;
            if (c < 0xe0) { c &= 31; extra = 1; min = 0x80; }
            else if (c < 0xf0) { c &= 15; extra = 2; min = 0x800; }
            else { c &= 7; extra = 3; min = 0x10000; }
            while (extra--) {
                if ((*src & 0xc0) != 0x80) return false;
                c = (c << 6) | (*src++ & 63);
            }
            if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        }
        if (n + 2 >= FTP_PATH) return false;
        if (c > 0xffff) {
            c -= 0x10000;
            wide[n++] = 0xd800 + (c >> 10);
            wide[n++] = 0xdc00 + (c & 1023);
        } else wide[n++] = c == '/' ? '\\' : c;
    }
    wide[n] = 0;
    return true;
}

static bool utf8_name(char *name, size_t cap, const CHAR16 *wide) {
    size_t n = 0;
    while (*wide) {
        uint32_t c = *wide++;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (*wide < 0xdc00 || *wide > 0xdfff) return false;
            c = 0x10000 + ((c - 0xd800) << 10) + (*wide++ - 0xdc00);
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
        unsigned count = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n + count >= cap) return false;
        for (unsigned i = count - 1; i > 0; i--) {
            name[n+i] = 0x80 | (c & 63);
            c >>= 6;
        }
        static const uint8_t lead[] = {0, 0, 0xc0, 0xe0, 0xf0};
        name[n] = lead[count] | c;
        n += count;
    }
    name[n] = 0;
    return true;
}

static FRESULT open_file(const char *path, EfiFile **file, uint64_t mode, uint64_t attributes) {
    if (!root) return FR_NOT_READY;
    if (mode != 1 && !writable) return FR_WRITE_PROTECTED;
    CHAR16 wide[FTP_PATH];
    if (!wide_path(path, wide)) return FR_INVALID_NAME;
    return result(root->open(root, file, wide, mode, attributes));
}

static FRESULT file_info(EfiFile *file, FileInfo *info) {
    uint64_t size = sizeof(*info);
    return result(file->get_info(file, &file_id, &size, info));
}

static FRESULT to_info(const FileInfo *in, FILINFO *out) {
    if (!out) return FR_OK;
    memset(out, 0, sizeof(*out));
    out->fsize = in->file_size;
    out->fattrib = (uint8_t)in->attributes;
    return utf8_name(out->fname, sizeof(out->fname), in->name) ? FR_OK : FR_INVALID_NAME;
}

FRESULT extras_ntfs_stat(const char *path, FILINFO *info) {
    EfiFile *file;
    FRESULT r = open_file(path, &file, 1, 0);
    if (r) return r;
    FileInfo metadata;
    r = file_info(file, &metadata);
    file->close(file);
    return r ? r : to_info(&metadata, info);
}

FRESULT extras_ntfs_open(FIL *fp, const char *path, BYTE mode) {
    const uint64_t create = 1ull << 63;
    if (mode != FA_READ && mode != (FA_CREATE_NEW | FA_WRITE)) return FR_INVALID_PARAMETER;
    if (mode & FA_CREATE_NEW) {
        FRESULT exists = extras_ntfs_stat(path, NULL);
        if (exists != FR_NO_FILE) return exists == FR_OK ? FR_EXIST : exists;
    }
    EfiFile *file;
    FRESULT r = open_file(path, &file, mode & FA_WRITE ? create | 3 : 1, 0);
    if (r) return r;
    FileInfo info;
    r = file_info(file, &info);
    if (r || (info.attributes & AM_DIR)) {
        file->close(file);
        return r ? r : FR_INVALID_OBJECT;
    }
    memset(fp, 0, sizeof(*fp));
    fp->backend = FS_NTFS;
    fp->obj.fs = file;
    fp->obj.objsize = info.file_size;
    return FR_OK;
}

FRESULT extras_ntfs_close(FIL *fp) {
    EfiFile *file = fp->obj.fs;
    fp->obj.fs = NULL;
    return result(file->close(file));
}
FRESULT extras_ntfs_read(FIL *fp, void *buf, UINT size, UINT *got) {
    EfiFile *file = fp->obj.fs;
    uint64_t count = size;
    FRESULT r = result(file->read(file, &count, buf));
    *got = r ? 0 : (unsigned)count;
    return r;
}
FRESULT extras_ntfs_write(FIL *fp, const void *buf, UINT size, UINT *put) {
    EfiFile *file = fp->obj.fs;
    uint64_t count = size;
    FRESULT r = result(file->write(file, &count, buf));
    *put = r ? 0 : (unsigned)count;
    return r;
}
FRESULT extras_ntfs_sync(FIL *fp) {
    EfiFile *file = fp->obj.fs;
    return result(file->flush(file));
}
FRESULT extras_ntfs_opendir(DIR *dir, const char *path) {
    EfiFile *file;
    FRESULT r = open_file(path, &file, 1, 0);
    if (r) return r;
    FileInfo info;
    r = file_info(file, &info);
    if (r || !(info.attributes & AM_DIR)) {
        file->close(file);
        return r ? r : FR_NO_PATH;
    }
    memset(dir, 0, sizeof(*dir));
    dir->backend = FS_NTFS;
    dir->obj.fs = file;
    return FR_OK;
}
FRESULT extras_ntfs_readdir(DIR *dir, FILINFO *info) {
    EfiFile *file = dir->obj.fs;
    for (;;) {
        FileInfo metadata = {0};
        uint64_t size = sizeof(metadata);
        FRESULT r = result(file->read(file, &size, &metadata));
        if (r) return r;
        r = to_info(&metadata, info);
        if (r == FR_INVALID_NAME || !strcmp(info->fname, ".") || !strcmp(info->fname, "..")) continue;
        return r;
    }
}
FRESULT extras_ntfs_closedir(DIR *dir) {
    EfiFile *file = dir->obj.fs;
    dir->obj.fs = NULL;
    return result(file->close(file));
}
static FRESULT flush_mutation(FRESULT r) {
    return !r && blk_sync(disk) < 0 ? FR_DISK_ERR : r;
}
FRESULT extras_ntfs_unlink(const char *path) {
    EfiFile *file;
    FRESULT r = open_file(path, &file, 3, 0);
    return r ? r : flush_mutation(result(file->delete(file)));
}
FRESULT extras_ntfs_rename(const char *from, const char *to) {
    FRESULT r = extras_ntfs_stat(to, NULL);
    if (r != FR_NO_FILE) return r ? r : FR_EXIST;
    EfiFile *file;
    r = open_file(from, &file, 3, 0);
    if (r) return r;
    FileInfo info;
    r = file_info(file, &info);
    if (!r) {
        if (!wide_path(to, info.name)) r = FR_INVALID_NAME;
        else {
            unsigned n = 0;
            while (info.name[n]) n++;
            info.record_size = offsetof(FileInfo, name) + (n + 1) * sizeof(CHAR16);
            r = result(file->set_info(file, &file_id, info.record_size, &info));
        }
    }
    FRESULT closed = result(file->close(file));
    return flush_mutation(r ? r : closed);
}
FRESULT extras_ntfs_mkdir(const char *path) {
    FRESULT r = extras_ntfs_stat(path, NULL);
    if (r != FR_NO_FILE) return r ? r : FR_EXIST;
    EfiFile *file;
    r = open_file(path, &file, (1ull << 63) | 3, AM_DIR);
    return r ? r : flush_mutation(result(file->close(file)));
}
