#include "extras_net.h"
#include "extras_disk.h"
#include "ff.h"
#include "ctype.h"

#define TCP_FIN EFI_ERR(104)
#define CONTROL_PORT 21
#define PASSIVE_BASE 49152
#define IDLE_US (120ULL * 1000000)
#define DATA_US (30ULL * 1000000)

enum Operation { NONE, GET, PUT, LIST, MLSD, NLST };
static ExtrasSocket listener, control, passive, data;
static ExtrasConfig settings, address;
static bool have_address, negotiating, prepared, tx, rx, quit, authenticated, user_ok;
static bool data_io, data_closing, walk_open, file_open;
static enum Operation op;
static const char *status;
static char running[32];
static char cwd[FTP_PATH], target[FTP_PATH], temporary[FTP_PATH], previous[FTP_PATH];
static uint8_t control_peer[4];
static char input[1024], response[1024], line[FTP_PATH + 16], payload[8192];
static unsigned input_pos, input_size, line_len, root_cursor, passive_port;
static uint64_t activity, data_activity;
static unsigned bad_logins;
static FIL file;
static DIR walk;
extern uint64_t wyse_ticks_us(void);

const char *extras_ftp_status(void) { return status ? status : "Off"; }
const ExtrasConfig *extras_ftp_lease(void) { return have_address ? &address : NULL; }

static bool ip_set(const uint8_t *ip) { return ip[0] || ip[1] || ip[2] || ip[3]; }

static void close_transfer(void) {
    if (file_open) f_close(&file);
    if (walk_open) f_closedir(&walk);
    if (*temporary) f_unlink(temporary);
    file_open = walk_open = data_io = data_closing = false;
    temporary[0] = previous[0] = 0;
    extras_net_drop(&data);
    extras_net_drop(&passive);
    op = NONE;
}

static void close_client(void) {
    close_transfer();
    extras_net_drop(&control);
    tx = rx = quit = authenticated = user_ok = false;
    input_pos = input_size = line_len = bad_logins = 0;
    strcpy(cwd, "/");
}

void extras_ftp_reset(void) {
    close_client();
    extras_net_drop(&listener);
    if (prepared) extras_disk_close();
    extras_net_end_dhcp();
    memset(&settings, 0, sizeof(settings));
    memset(&address, 0, sizeof(address));
    memset(input, 0, sizeof(input));
    prepared = negotiating = have_address = false;
    status = "Off";
}

void extras_ftp_stop(void) {
    extras_ftp_reset();
    extras_net_end();
}

static void start_listener(void) {
    if (!settings.enabled) { status = "Off"; return; }
    if (!settings.user[0] || !settings.password[0]) { status = "Set username and password"; return; }
    extras_disk_open();
    prepared = true;
    if (!extras_net_listen(&listener, &address, CONTROL_PORT)) {
        extras_disk_close();
        prepared = false;
        status = "TCP port 21 unavailable";
        return;
    }
    snprintf(running, sizeof(running), "On %u.%u.%u.%u:21",
        address.ip[0], address.ip[1], address.ip[2], address.ip[3]);
    status = running;
}

void extras_ftp_start(const ExtrasConfig *config) {
    settings = *config;
    memcpy(address.ip, config->ip, sizeof(address.ip));
    memcpy(address.mask, config->mask, sizeof(address.mask));
    memcpy(address.gateway, config->gateway, sizeof(address.gateway));
    memcpy(address.dns, config->dns, sizeof(address.dns));
    if (!config->dhcp && !ip_set(config->ip)) { status = "Set a static IP address"; return; }
    if (!config->dhcp && !config->enabled) return;
    if (!extras_net_begin()) { status = "UEFI TCP4 unavailable"; return; }
    if (config->dhcp) {
        if (extras_net_pxe_lease(&address)) {
            have_address = true;
            start_listener();
            return;
        }
        negotiating = extras_net_dhcp();
        status = negotiating ? "Waiting for DHCP" : "Could not start DHCP";
    } else {
        have_address = true;
        start_listener();
    }
}

static void reply(const char *message) {
    snprintf(response, sizeof(response), "%s", message);
    tx = extras_net_send(&control, response, strlen(response));
    if (!tx) close_client();
}

static bool open_passive(bool extended) {
    close_transfer();
    for (unsigned i = 0; i < 32; i++) {
        passive_port = (passive_port + 1) % 32;
        unsigned port = PASSIVE_BASE + passive_port;
        if (!extras_net_listen(&passive, &address, port)) continue;
        char message[100];
        if (extended) snprintf(message, sizeof(message), "229 Entering Extended Passive Mode (|||%u|)\r\n", port);
        else snprintf(message, sizeof(message), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
            address.ip[0], address.ip[1], address.ip[2], address.ip[3], port >> 8, port & 255);
        data_activity = wyse_ticks_us();
        reply(message);
        return true;
    }
    return false;
}

static bool upload_open(void) {
    const char *slash = strrchr(target, '/');
    if (!slash || !slash[1]) return false;
    size_t prefix = slash - target + 1;
    if (prefix + 13 > sizeof(temporary)) return false;
    memcpy(temporary, target, prefix);
    memcpy(previous, target, prefix);
    for (unsigned i = 0; i < 100; i++) {
        snprintf(temporary + prefix, sizeof(temporary) - prefix, "WX%06u.TMP", i);
        snprintf(previous + prefix, sizeof(previous) - prefix, "WX%06u.BAK", i);
        if (!strcasecmp(target, temporary) || !strcasecmp(target, previous)) continue;
        FRESULT exists = f_stat(previous, NULL);
        if (exists == FR_OK) continue;
        if (exists != FR_NO_FILE) {
            printf("extras: cannot check upload directory (%u)\n", (unsigned)exists);
            break;
        }
        FRESULT result = f_open(&file, temporary, FA_CREATE_NEW | FA_WRITE);
        if (result == FR_OK) return true;
        if (result != FR_EXIST) {
            printf("extras: cannot create upload temporary file (%u)\n", (unsigned)result);
            break;
        }
    }
    temporary[0] = 0;
    return false;
}

static bool upload_commit(void) {
    FRESULT sync = f_sync(&file);
    FRESULT closed = f_close(&file);
    file_open = false;
    if (sync != FR_OK || closed != FR_OK) {
        printf("extras: upload sync %u close %u\n", (unsigned)sync, (unsigned)closed);
        return false;
    }
    FRESULT result = f_rename(temporary, target);
    if (result == FR_EXIST) {
        FILINFO info;
        if (f_stat(target, &info) != FR_OK || (info.fattrib & AM_DIR) ||
            f_rename(target, previous) != FR_OK) return false;
        result = f_rename(temporary, target);
        if (result != FR_OK) f_rename(previous, target);
        else f_unlink(previous);
    }
    if (result == FR_OK) temporary[0] = 0;
    else printf("extras: upload rename failed (%u)\n", (unsigned)result);
    return result == FR_OK;
}

static void begin_transfer(enum Operation operation, const char *path) {
    if (!passive.tcp && !data.tcp) { reply("425 Use PASV or EPSV first\r\n"); return; }
    bool listing = operation >= LIST;
    bool root = listing && !strcmp(path, "/");
    bool allowed = root || extras_disk_path(path, operation == PUT, settings.esp_write, target);
    if (allowed && operation == PUT) allowed = file_open = upload_open();
    else if (allowed && operation == GET) allowed = file_open = f_open(&file, target, FA_READ) == FR_OK;
    else if (allowed && !root) allowed = walk_open = f_opendir(&walk, target) == FR_OK;
    if (!allowed) { close_transfer(); reply("550 Path unavailable\r\n"); return; }
    op = operation;
    root_cursor = 0;
    data_activity = wyse_ticks_us();
    reply("150 Opening data connection\r\n");
}

static int list_entry(void) {
    FILINFO item = {0};
    if (walk_open) {
        do {
            if (f_readdir(&walk, &item) != FR_OK) return -1;
            if (!item.fname[0]) return 0;
        } while (!strncmp(target, "2:", 2) && !strncasecmp(item.fname, "extras.", 7));
    } else {
        const char *name;
        do {
            name = extras_disk_name(root_cursor++);
            if (!name) return 0;
        } while (!*name);
        strcpy(item.fname, name);
        item.fattrib = AM_DIR;
    }
    bool dir = item.fattrib & AM_DIR;
    if (op == MLSD)
        return snprintf(payload, sizeof(payload), "type=%s;size=%llu; %s\r\n",
            dir ? "dir" : "file", (unsigned long long)item.fsize, item.fname);
    if (op == NLST) return snprintf(payload, sizeof(payload), "%s\r\n", item.fname);
    return snprintf(payload, sizeof(payload), "%crwxr-xr-x 1 wyse wyse %llu Jan 01 2000 %s\r\n",
        dir ? 'd' : '-', (unsigned long long)item.fsize, item.fname);
}

static void end_transfer(bool ok) {
    close_transfer();
    reply(ok ? "226 Transfer complete\r\n" : "426 Transfer aborted\r\n");
}

static void poll_data(void) {
    if (passive.tcp && !data.tcp) {
        uint8_t peer[4];
        if (extras_net_accept(&passive, &data, peer)) {
            if (memcmp(peer, control_peer, 4)) {
                end_transfer(false);
                return;
            }
        }
    }
    if (wyse_ticks_us() - data_activity > DATA_US) {
        if (op) end_transfer(false);
        else close_transfer();
        return;
    }
    if (!op || !data.tcp) return;
    if (data_closing) {
        if (extras_net_done(&data, &data.close.done)) end_transfer(!EFI_ERROR(data.close.done.status));
        return;
    }
    if (data_io) {
        const Completion *done = op == PUT ? &data.rx.done : &data.tx.done;
        if (!extras_net_done(&data, done)) return;
        data_io = false;
        if (op == PUT && done->status == TCP_FIN) { end_transfer(upload_commit()); return; }
        if (EFI_ERROR(done->status)) { end_transfer(false); return; }
        data_activity = wyse_ticks_us();
        activity = data_activity;
        if (op == PUT) {
            unsigned got = data.rx_packet.len, wrote;
            FRESULT result = got > sizeof(payload) ? FR_INVALID_PARAMETER : f_write(&file, payload, got, &wrote);
            if (result != FR_OK || wrote != got) {
                printf("extras: upload write failed (%u)\n", (unsigned)result);
                end_transfer(false);
                return;
            }
        }
    }
    if (op == PUT) {
        EFI_STATUS received = extras_net_receive(&data, payload, sizeof(payload));
        if (received == TCP_FIN) { end_transfer(upload_commit()); return; }
        data_io = !EFI_ERROR(received);
    } else {
        int length;
        if (op == GET) {
            unsigned got;
            length = f_read(&file, payload, sizeof(payload), &got) == FR_OK ? (int)got : -1;
        } else length = list_entry();
        if (!length) data_closing = extras_net_finish(&data);
        if (length > 0) data_io = extras_net_send(&data, payload, length);
    }
    if (!data_io && !data_closing) end_transfer(false);
}

static bool same_password(const char *s) {
    size_t len = strlen(s);
    size_t expected_len = strlen(settings.password);
    unsigned diff = (unsigned)(expected_len ^ len);
    for (unsigned i = 0; i < sizeof(settings.password); i++) {
        unsigned expected = i < expected_len ? (unsigned char)settings.password[i] : 0;
        diff |= (i < len ? (unsigned char)s[i] : 0) ^ expected;
    }
    return !diff;
}

static void command(char *cmd) {
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;
    else arg = cmd + strlen(cmd);
    for (char *p = cmd; *p; p++) *p = toupper((unsigned char)*p);
    if (!strcmp(cmd, "QUIT")) { quit = true; reply("221 Goodbye\r\n"); return; }
    if (!strcmp(cmd, "FEAT")) { reply("211-Features\r\n EPSV\r\n MLSD\r\n SIZE\r\n211 End\r\n"); return; }
    if (!strcmp(cmd, "USER")) {
        close_transfer();
        authenticated = false;
        user_ok = !strcmp(arg, settings.user);
        reply("331 Password required\r\n");
        return;
    }
    if (!strcmp(cmd, "PASS")) {
        authenticated = user_ok && same_password(arg);
        if (!authenticated && ++bad_logins >= 3) quit = true;
        reply(authenticated ? "230 Logged in\r\n" : "530 Login incorrect\r\n");
        return;
    }
    if (!authenticated) { reply("530 Please log in\r\n"); return; }
    if (!strcmp(cmd, "NOOP") || !strcmp(cmd, "TYPE") || !strcmp(cmd, "OPTS")) { reply("200 OK\r\n"); return; }
    if (!strcmp(cmd, "SYST")) { reply("215 UNIX Type: L8\r\n"); return; }
    if (!strcmp(cmd, "ABOR")) { close_transfer(); reply("226 Transfer stopped\r\n"); return; }
    if (op) { reply("450 Transfer in progress\r\n"); return; }
    if (!strcmp(cmd, "PASV") || !strcmp(cmd, "EPSV")) {
        if (!open_passive(cmd[0] == 'E')) reply("425 Cannot listen for data\r\n");
        return;
    }
    char path[FTP_PATH], local[FTP_PATH], message[FTP_PATH + 32];
    if (!strcmp(cmd, "PWD")) {
        snprintf(message, sizeof(message), "257 \"%s\"\r\n", cwd);
        reply(message);
        return;
    }
    if (!strcmp(cmd, "CDUP")) arg = "..";
    if (!strcmp(cmd, "LIST") && (!strcmp(arg, "-a") || !strcmp(arg, "-la") || !strcmp(arg, "-al")))
        arg = "";
    if (!extras_disk_resolve(cwd, arg, path)) { reply("550 Invalid path\r\n"); return; }
    if (!strcmp(cmd, "CWD") || !strcmp(cmd, "CDUP")) {
        DIR dir;
        if (strcmp(path, "/") && (!extras_disk_path(path, false, false, local) ||
                                 f_opendir(&dir, local) != FR_OK)) {
            reply("550 Directory unavailable\r\n");
            return;
        }
        if (strcmp(path, "/")) f_closedir(&dir);
        strcpy(cwd, path);
        reply("250 Directory changed\r\n");
    } else if (!strcmp(cmd, "LIST")) begin_transfer(LIST, path);
    else if (!strcmp(cmd, "MLSD")) begin_transfer(MLSD, path);
    else if (!strcmp(cmd, "NLST")) begin_transfer(NLST, path);
    else if (!strcmp(cmd, "RETR")) begin_transfer(GET, path);
    else if (!strcmp(cmd, "STOR")) begin_transfer(PUT, path);
    else if (!strcmp(cmd, "SIZE")) {
        FILINFO info;
        if (!extras_disk_path(path, false, false, local) || f_stat(local, &info) != FR_OK ||
            (info.fattrib & AM_DIR)) { reply("550 File unavailable\r\n"); return; }
        snprintf(message, sizeof(message), "213 %llu\r\n", (unsigned long long)info.fsize);
        reply(message);
    } else if (!strcmp(cmd, "MKD")) {
        if (!extras_disk_path(path, true, settings.esp_write, local) || f_mkdir(local) != FR_OK) {
            reply("550 Cannot create directory\r\n"); return;
        }
        snprintf(message, sizeof(message), "257 \"%s\" created\r\n", path);
        reply(message);
    } else if (!strcmp(cmd, "DELE") || !strcmp(cmd, "RMD")) {
        FILINFO info;
        bool directory = !strcmp(cmd, "RMD");
        bool ok = extras_disk_path(path, true, settings.esp_write, local) &&
            f_stat(local, &info) == FR_OK && !!(info.fattrib & AM_DIR) == directory && f_unlink(local) == FR_OK;
        reply(ok ? "250 Removed\r\n" : "550 Cannot remove path\r\n");
    } else reply("502 Command not supported\r\n");
}

static bool arm_control(void) {
    rx = !EFI_ERROR(extras_net_receive(&control, input, sizeof(input)));
    return rx;
}

static void poll_client(void) {
    if (wyse_ticks_us() - activity > IDLE_US) { close_client(); return; }
    if (tx) {
        if (!extras_net_done(&control, &control.tx.done)) return;
        tx = false;
        if (quit || EFI_ERROR(control.tx.done.status)) { close_client(); return; }
    }
    if (passive.tcp || data.tcp) poll_data();
    if (tx || !control.tcp) return;
    if (rx && extras_net_done(&control, &control.rx.done)) {
        rx = false;
        input_pos = 0;
        input_size = control.rx_packet.len;
        if (EFI_ERROR(control.rx.done.status) || input_size > sizeof(input)) { close_client(); return; }
        activity = wyse_ticks_us();
    }
    while (!rx && input_pos < input_size && !tx && control.tcp) {
        unsigned char c = input[input_pos++];
        if (c == '\n') {
            if (line_len && line[line_len - 1] == '\r') line_len--;
            line[line_len] = 0;
            command(line);
            memset(line, 0, sizeof(line));
            line_len = 0;
        } else if ((c < 32 && c != '\r') || line_len + 1 >= sizeof(line)) {
            close_client();
            return;
        } else line[line_len++] = c;
    }
    if (!rx && control.tcp && input_pos == input_size && !arm_control()) close_client();
}

bool extras_ftp_poll(void) {
    if (negotiating) {
        int result = extras_net_lease(&address);
        if (!result) return false;
        negotiating = false;
        have_address = result > 0;
        if (have_address) start_listener();
        else status = "DHCP did not assign an address";
        return true;
    }
    if (!listener.tcp) return false;
    if (control.tcp) {
        poll_client();
        if (!control.tcp && !extras_net_wait_client(&listener)) {
            extras_net_drop(&listener);
            status = "Could not reopen port 21";
            return true;
        }
        if (!control.tcp) {
            status = running;
            return true;
        }
    }
    else if (extras_net_accept(&listener, &control, control_peer)) {
        activity = wyse_ticks_us();
        reply("220 WyseDOS Extras FTP\r\n");
        if (!control.tcp) extras_net_wait_client(&listener);
        else status = "FTP client connected";
        return true;
    }
    return false;
}
