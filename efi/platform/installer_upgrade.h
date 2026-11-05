#ifndef WYSE_INSTALLER_UPGRADE_H
#define WYSE_INSTALLER_UPGRADE_H
#include "bio.h"

typedef struct {
    uint64_t esp_first, esp_count, guest_first, guest_count;
} WyseInstallLayout;

/* Read-only GPT qualification, including checksums, bounds and overlaps. */
bool installer_read_layout(EFI_BLOCK_IO_PROTOCOL *disk, WyseInstallLayout *layout);

typedef bool (*UpgradeFlush)(void *context);
typedef void (*UpgradeProgress)(const char *file, unsigned percent);
/* Only the fixed monitor manifest on volume 2 is modified. Neither config.ini
 * nor native.ini nor a guest volume is written. A valid interrupted journal
 * rolls back an incomplete activation or completes committed-file cleanup. */
bool installer_upgrade_files(UpgradeFlush flush, void *context,
                             UpgradeProgress progress, const char **error);
bool installer_upgrade_pending(void);
#endif
