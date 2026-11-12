#include <assert.h>
#include "../../efi/platform/bio.c"

EFI_BOOT_SERVICES *gBS;
static unsigned reads;
static unsigned writes;

static EFI_STATUS mock_read(EFI_BLOCK_IO_PROTOCOL *bio, uint32_t id,
                            uint64_t lba, uint64_t bytes, void *buffer) {
    reads++;
    memset(buffer, 0x5a, bytes);
    return EFI_SUCCESS;
}

static EFI_STATUS mock_write(EFI_BLOCK_IO_PROTOCOL *bio, uint32_t id,
                             uint64_t lba, uint64_t bytes, const void *buffer) {
    writes++;
    return EFI_DEVICE_ERROR;
}

int main(void) {
    EFI_BLOCK_IO_MEDIA media = {0};
    media.BlockSize = 512;
    media.IoAlign = 1;
    EFI_BLOCK_IO_PROTOCOL device = {0};
    device.Media = &media;
    device.ReadBlocks = mock_read;
    device.WriteBlocks = mock_write;
    uint8_t buffer[512];
    assert(bio_read(&device, 0, 1, buffer) == EFI_SUCCESS);
    assert(reads == 1 && buffer[0] == 0x5a);
    assert(bio_write(&device, 0, 1, buffer) == EFI_WRITE_PROTECTED);
    assert(bio_write(NULL, UINT64_MAX, UINT64_MAX, NULL) == EFI_WRITE_PROTECTED);
    assert(writes == 0);
    puts("PASS: isolated POC block reads work and physical writes never reach firmware");
    return 0;
}