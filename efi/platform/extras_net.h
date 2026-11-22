#ifndef WYSE_EXTRAS_NET_H
#define WYSE_EXTRAS_NET_H

#include "lib.h"
#include "extras.h"

/* UEFI TCP4/DHCP4 ABI, per TianoCore MdePkg Include/Protocol. */
typedef struct Binding Binding;
struct Binding {
    EFI_STATUS (*create)(Binding *, EFI_HANDLE *);
    EFI_STATUS (*destroy)(Binding *, EFI_HANDLE);
};
typedef struct {
    bool use_default;
    uint8_t station[4], mask[4];
    uint16_t port;
    uint8_t remote[4];
    uint16_t remote_port;
    bool active;
} TcpAccess;
typedef struct {
    uint8_t tos, ttl;
    TcpAccess access;
    void *options;
} TcpConfig;
typedef struct { EFI_EVENT event; EFI_STATUS status; } Completion;
typedef struct { Completion done; EFI_HANDLE child; } AcceptToken;
typedef struct { uint32_t len; void *buf; } Fragment;
typedef struct { bool push, urgent; uint32_t len, count; Fragment frag[1]; } TcpPacket;
typedef struct { Completion done; TcpPacket *packet; } IoToken;
typedef struct { Completion done; bool abort; } CloseToken;
typedef struct Tcp Tcp;
struct Tcp {
    EFI_STATUS (*mode)(Tcp *, void *, TcpConfig *, void *, void *, void *);
    EFI_STATUS (*configure)(Tcp *, TcpConfig *);
    EFI_STATUS (*routes)(Tcp *, bool, const uint8_t *, const uint8_t *, const uint8_t *);
    void *connect;
    EFI_STATUS (*accept)(Tcp *, AcceptToken *);
    EFI_STATUS (*send)(Tcp *, IoToken *);
    EFI_STATUS (*receive)(Tcp *, IoToken *);
    EFI_STATUS (*close)(Tcp *, CloseToken *);
    EFI_STATUS (*cancel)(Tcp *, Completion *);
    EFI_STATUS (*poll)(Tcp *);
};
typedef struct {
    uint32_t discover_try;
    uint32_t *discover_timeout;
    uint32_t request_try;
    uint32_t *request_timeout;
    uint8_t client[4];
    void *callback, *context;
    uint32_t option_count;
    void *options;
} DhcpConfig;
typedef struct {
    uint32_t size, length;
    uint8_t message[1];
} DhcpPacket;
typedef struct {
    uint32_t state;
    DhcpConfig config;
    uint8_t client[4], mac[32], server[4], router[4], mask[4];
    uint32_t lease_time;
    DhcpPacket *reply;
} DhcpMode;
typedef struct Dhcp Dhcp;
struct Dhcp {
    EFI_STATUS (*mode)(Dhcp *, DhcpMode *);
    EFI_STATUS (*configure)(Dhcp *, DhcpConfig *);
    EFI_STATUS (*start)(Dhcp *, EFI_EVENT);
    void *renew;
    EFI_STATUS (*release)(Dhcp *);
    EFI_STATUS (*stop)(Dhcp *);
};

typedef struct {
    EFI_HANDLE handle;
    Tcp *tcp;
    AcceptToken accept;
    IoToken rx, tx;
    TcpPacket rx_packet, tx_packet;
    CloseToken close;
} ExtrasSocket;

bool extras_net_begin(void);
void extras_net_end(void);
void extras_net_end_dhcp(void);
bool extras_net_dhcp(void);
bool extras_net_pxe_lease(ExtrasConfig *out);
int extras_net_lease(ExtrasConfig *out);
bool extras_net_listen(ExtrasSocket *s, const ExtrasConfig *config, uint16_t port);
bool extras_net_wait_client(ExtrasSocket *s);
bool extras_net_accept(ExtrasSocket *listener, ExtrasSocket *connection, uint8_t *peer);
void extras_net_drop(ExtrasSocket *s);
EFI_STATUS extras_net_receive(ExtrasSocket *s, void *buffer, unsigned length);
bool extras_net_send(ExtrasSocket *s, void *buffer, unsigned length);
bool extras_net_finish(ExtrasSocket *s);
bool extras_net_done(ExtrasSocket *s, const Completion *token);
#endif
