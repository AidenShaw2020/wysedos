// Dell BIOS settings ("tokens") through Dell's SMI calling interface.
//
// The Wyse 3040's settings live where only the firmware's SMM code reaches
// them; Dell describes a way in through SMBIOS type 0xDA: write the command
// code (05h) to the command port (B2h) with EBX pointing at a request buffer
// below 4 GB and ECX holding "BSI1", and SMM reads or writes a token. The DA
// tables also list the tokens: an ID, a location and the value that turns it
// on. This is what Linux's dell-smbios-smm and dcdbas do.

#include <string.h>
#include <stdio.h>
#include "../include/efi.h"
#include "delltoken.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;

typedef struct __attribute__((packed)) {
    uint16_t cmd_class, cmd_select;
    uint32_t input[4], output[4];
} dell_buffer_t;

static uint16_t cmd_port;
static uint8_t cmd_code;
static dell_token_t tokens[256];
static int ntokens = -1;
static dell_buffer_t *buf;

static void scan(void) {
    static const EFI_GUID g3 = SMBIOS3_TABLE_GUID;
    ntokens = 0;
    const uint8_t *table = NULL, *end = NULL;
    for (uint64_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &gST->ConfigurationTable[i];
        const uint8_t *ep = t->VendorTable;
        if (!memcmp(&t->VendorGuid, &g3, sizeof(EFI_GUID)) && !memcmp(ep, "_SM3_", 5)) {
            table = (const uint8_t *)(uintptr_t)*(const uint64_t *)(ep + 0x10);
            end = table + *(const uint32_t *)(ep + 0x0c);
        }
    }
    for (const uint8_t *s = table; s && s + 4 <= end && s[0] != 127;) {
        if (s[0] == 0xda && s[1] >= 11) {
            cmd_port = *(const uint16_t *)(s + 4);
            cmd_code = s[6];
            for (int off = 11; off + 6 <= s[1] && ntokens < 256; off += 6) {
                dell_token_t *t = &tokens[ntokens];
                t->id = *(const uint16_t *)(s + off);
                t->location = *(const uint16_t *)(s + off + 2);
                t->value = *(const uint16_t *)(s + off + 4);
                if (t->id == 0xffff) break;
                ntokens++;
            }
        }
        const uint8_t *p = s + s[1];
        while (p + 1 < end && (p[0] || p[1])) p++;
        s = p + 2;
    }
    EFI_PHYSICAL_ADDRESS a = 0xffffffffu;
    if (!EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &a))) buf = (dell_buffer_t *)a;
}

static bool call(uint16_t cls, uint16_t sel, uint32_t in0, uint32_t in1) {
    if (ntokens < 0) scan();
    if (!buf || !cmd_port) return false;
    memset(buf, 0, sizeof(*buf));
    buf->cmd_class = cls;
    buf->cmd_select = sel;
    buf->input[0] = in0;
    buf->input[1] = in1;
    uint32_t ebx = (uint32_t)(uintptr_t)buf, ecx = 0x42534931;     // "BSI1"
    __asm__ volatile("outb %b0, %w1\n\tinb %w1" : : "a"(cmd_code), "d"(cmd_port), "b"(ebx), "c"(ecx) : "memory");
    return buf->output[0] == 0;
}

int dell_tokens(const dell_token_t **list) {
    if (ntokens < 0) scan();
    *list = tokens;
    return ntokens;
}

// The token's location holds this value now (class 0 read), or -1
int dell_token_current(const dell_token_t *t) {
    if (!call(0, 0, t->location, 0)) return -1;
    return (int)buf->output[1];
}

bool dell_token_active(uint16_t id) {
    const dell_token_t *list;
    int n = dell_tokens(&list);
    for (int i = 0; i < n; i++)
        if (list[i].id == id) return dell_token_current(&list[i]) == list[i].value;
    return false;
}

// Turn a token on: its value into its location (class 1 write)
bool dell_token_activate(uint16_t id) {
    const dell_token_t *list;
    int n = dell_tokens(&list);
    for (int i = 0; i < n; i++)
        if (list[i].id == id) return call(1, 0, list[i].location, list[i].value);
    return false;
}

// "tokens" on the control port: every token, its value now and whether it is on
void dell_token_report(void) {
    const dell_token_t *list;
    int n = dell_tokens(&list);
    printf("dell: SMI port %x code %x, %d tokens\n", cmd_port, cmd_code, n);
    char line[300];
    int len = 0;
    for (int i = 0; i < n; i++) {
        int cur = dell_token_current(&list[i]);
        len += snprintf(line + len, sizeof(line) - len, " %04x:%d%s", list[i].id, cur,
                        cur == (int)list[i].value ? "*" : "");
        if (len > 200 || i == n - 1) {
            printf("dell:%s\n", line);
            len = 0;
        }
    }
}
