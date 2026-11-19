#ifndef WYSE_EMMC_SNAPSHOT_H
#define WYSE_EMMC_SNAPSHOT_H
#include <stdbool.h>

/* Dedicated netboot mode, before any guest or storage writer is started. */
bool emmc_snapshot_requested(void);
bool emmc_snapshot_run(void);
#endif
