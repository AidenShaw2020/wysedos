#ifndef WYSE_EXTRAS_H
#define WYSE_EXTRAS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled, esp_write, dhcp;
    char user[32], password[64];
    uint8_t ip[4], mask[4], gateway[4], dns[4];
} ExtrasConfig;

void extras_open(bool installer);
void extras_draw(void);
bool extras_ftp_poll(void);
void extras_ftp_start(const ExtrasConfig *config);
void extras_ftp_reset(void);
void extras_ftp_stop(void);
const char *extras_ftp_status(void);
const ExtrasConfig *extras_ftp_lease(void);

#endif
