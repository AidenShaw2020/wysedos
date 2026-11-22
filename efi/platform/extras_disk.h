#ifndef WYSE_EXTRAS_DISK_H
#define WYSE_EXTRAS_DISK_H

#include "extras.h"

/* Fits the FatFs wrapper's 300-byte path including its drive prefix. */
#define FTP_PATH 300

void extras_disk_open(void);
void extras_disk_close(void);
const char *extras_disk_name(unsigned index);
bool extras_disk_resolve(const char *cwd, const char *arg, char *out);
bool extras_disk_path(const char *path, bool write, bool esp_write, char *out);

#endif
