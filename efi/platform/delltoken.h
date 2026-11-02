// Dell BIOS settings ("tokens") through Dell's SMI calling interface (delltoken.c)
#ifndef DELLTOKEN_H
#define DELLTOKEN_H

#include <stdint.h>
#include <stdbool.h>

// Tokens of the Wyse 3040 (BIOS 1.2.5), found by changing the setting in
// BIOS setup and reading the tokens before and after
#define DELL_TOKEN_FASTBOOT_MINIMAL  0x00ea     // 0x00eb Thorough (default), 0x00ec Auto
#define DELL_TOKEN_VT_DISABLE        0x014a
#define DELL_TOKEN_VT_ENABLE         0x014b

typedef struct {
    uint16_t id, location, value;   // value: what the location holds when the token is on
} dell_token_t;

int dell_tokens(const dell_token_t **list);
int dell_token_current(const dell_token_t *t);
bool dell_token_active(uint16_t id);
bool dell_token_activate(uint16_t id);
void dell_token_report(void);

#endif
