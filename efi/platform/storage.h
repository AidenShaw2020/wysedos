// Where the machine's files come from (storage.c)
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include "../include/efi.h"

// Pick the default file backend: netfs when netbooted, else the FatFs volume
// with 386/config.ini on the boot disk. False when a disk boot found none.
bool storage_init(void);
// Every Block I/O device and partition, to the log
void storage_list(void);
// The boot source for messages ("netfs" or a device path summary)
const char *storage_source(void);
// The partition the monitor was loaded from (NULL when netbooted)
EFI_HANDLE storage_boot_handle(void);

// Control port diagnostics: "fs N [path]", "fswr N"
void storage_debug(const char *args, bool write_test);
void storage_bench(const char *args);
void storage_file_bench(void);

#endif
