// The wyseDOS installer (installer.c)
#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdbool.h>

// Whether the medium the monitor started from asks for an installation
// (install.ini in its root)
bool installer_wanted(void);
// Install onto the eMMC after the user confirms; ends in a restart
void installer_run(void);
// Development: the running monitor and 386/config.ini onto an installed eMMC
void installer_update_emmc(const char *replace);

#endif
