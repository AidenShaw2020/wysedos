// Minimal IPv4/UDP over the firmware's Simple Network Protocol.
//
// The monitor takes the NIC for itself (EXCLUSIVE open stops the firmware's MNP/IP
// stack that would otherwise eat incoming frames), answers ARP for its own
// address and talks UDP to the development PC on the direct cable:
//   - log lines, broadcast to port 5555 (no ARP needed, works before net_init)
//   - netfs requests, unicast to the PC (see netfs.c)
// Our address and the PC's come from the PXE DHCP exchange that loaded us.

#include "lib.h"
#include "net.h"

static EFI_SIMPLE_NETWORK_PROTOCOL *snp;
static uint8_t my_mac[6], my_ip[4], server_ip[4], server_mac[6];
static bool server_mac_known;
bool g_net_accept_broadcast;
static uint16_t ip_id;
static uint8_t tx_frame[1514] __attribute__((aligned(16)));
static uint8_t rx_frame[1536] __attribute__((aligned(16)));

static const EFI_GUID snp_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static const EFI_GUID pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
static const EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

bool net_active(void) { return snp != NULL; }
const uint8_t *net_my_ip(void) { return my_ip; }
const uint8_t *net_server_ip(void) { return server_ip; }

static bool snp_ready(EFI_SIMPLE_NETWORK_PROTOCOL *s) {
    if (s->Mode->State == EfiSimpleNetworkStopped && EFI_ERROR(s->Start(s))) return false;
    if (s->Mode->State == EfiSimpleNetworkStarted && EFI_ERROR(s->Initialize(s, 0, 0))) return false;
    return s->Mode->State == EfiSimpleNetworkInitialized;
}

void net_init(void) {
    EFI_HANDLE nic = NULL;

    // our addresses: from the PXE handle we were loaded from
    EFI_LOADED_IMAGE_PROTOCOL *li;
    if (!EFI_ERROR(gBS->HandleProtocol(gImage, &loaded_image_guid, (void **)&li))) {
        EFI_PXE_BASE_CODE_PROTOCOL *pxe;
        if (!EFI_ERROR(gBS->HandleProtocol(li->DeviceHandle, &pxe_guid, (void **)&pxe)) && pxe->Mode->Started) {
            memcpy(my_ip, pxe->Mode->StationIp.v4, 4);
            if (pxe->Mode->DhcpAckReceived) memcpy(server_ip, pxe->Mode->DhcpAck + 20, 4);
            nic = li->DeviceHandle;
        }
    }

    uint64_t count = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &snp_guid, NULL, &count, &handles))) return;
    // prefer the NIC we netbooted from; it carries SNP itself
    for (uint64_t i = 0; i < count && !snp; i++) {
        if (nic && handles[i] != nic) continue;
        EFI_SIMPLE_NETWORK_PROTOCOL *s;
        // EXCLUSIVE: the firmware's MNP gets disconnected and stops polling the NIC
        if (EFI_ERROR(gBS->OpenProtocol(handles[i], &snp_guid, (void **)&s, gImage, NULL, EFI_OPEN_PROTOCOL_EXCLUSIVE)) &&
            EFI_ERROR(gBS->HandleProtocol(handles[i], &snp_guid, (void **)&s)))
            continue;
        if (snp_ready(s) && s->Mode->IfType == 1) snp = s;
    }
    for (uint64_t i = 0; i < count && !snp; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *s;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &snp_guid, (void **)&s))) continue;
        if (snp_ready(s) && s->Mode->IfType == 1) snp = s;
    }
    gBS->FreePool(handles);
    if (!snp) return;

    memcpy(my_mac, snp->Mode->CurrentAddress.Addr, 6);
    snp->ReceiveFilters(snp, EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST, 0, false, 0, NULL);
}

static uint16_t csum(const uint8_t *p, int len) {
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2) sum += (p[i] << 8) | p[i + 1];
    if (len & 1) sum += p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void tx(size_t len) {
    if (len < 60) {
        memset(tx_frame + len, 0, 60 - len);
        len = 60;
    }
    if (EFI_ERROR(snp->Transmit(snp, 0, len, tx_frame, NULL, NULL, NULL))) return;
    // the NIC owns the buffer until GetStatus hands it back
    for (int i = 0; i < 100000; i++) {
        void *done = NULL;
        uint32_t irq;
        if (EFI_ERROR(snp->GetStatus(snp, &irq, &done)) || done) break;
    }
}

static void eth_header(const uint8_t *dst_mac, uint16_t type) {
    memcpy(tx_frame, dst_mac, 6);
    memcpy(tx_frame + 6, my_mac, 6);
    tx_frame[12] = type >> 8;
    tx_frame[13] = type & 0xff;
}

static void arp_send(uint16_t op, const uint8_t *tha, const uint8_t *tpa) {
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    eth_header(op == 1 ? bcast : tha, 0x0806);
    uint8_t *a = tx_frame + 14;
    a[0] = 0; a[1] = 1;             // Ethernet
    a[2] = 8; a[3] = 0;             // IPv4
    a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = (uint8_t)op;
    memcpy(a + 8, my_mac, 6);
    memcpy(a + 14, my_ip, 4);
    if (op == 1) memset(a + 18, 0, 6); else memcpy(a + 18, tha, 6);
    memcpy(a + 24, tpa, 4);
    tx(14 + 28);
}

static void udp_frame(const uint8_t *dst_mac, const uint8_t *dst_ip, uint16_t sport, uint16_t dport,
                      const void *data, size_t len) {
    if (len > 1472) len = 1472;
    eth_header(dst_mac, 0x0800);
    uint8_t *ip = tx_frame + 14, *udp = ip + 20;
    uint16_t ip_len = (uint16_t)(20 + 8 + len);
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = ip_len >> 8; ip[3] = ip_len & 0xff;
    ip_id++;
    ip[4] = ip_id >> 8; ip[5] = ip_id & 0xff;
    ip[6] = 0x40; ip[7] = 0;
    ip[8] = 64; ip[9] = 17;
    ip[10] = ip[11] = 0;
    memcpy(ip + 12, my_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    uint16_t cs = csum(ip, 20);
    ip[10] = cs >> 8; ip[11] = cs & 0xff;
    udp[0] = sport >> 8; udp[1] = sport & 0xff;
    udp[2] = dport >> 8; udp[3] = dport & 0xff;
    udp[4] = (8 + len) >> 8; udp[5] = (8 + len) & 0xff;
    udp[6] = udp[7] = 0;
    memcpy(udp + 8, data, len);
    tx(14 + ip_len);
}

void net_broadcast(uint16_t port, const void *data, size_t len) {
    static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t bcast_ip[4] = {0xff, 0xff, 0xff, 0xff};
    if (!snp) return;
    /* Startup diagnostics are optional. Without a cable, a firmware driver
     * may accept a transmit but never return its buffer, making every log
     * line run the completion polling budget before the splash can appear.
     * GetStatus refreshes MediaPresent, so plugging the cable back in also
     * restores logging without a reboot. Leave unsupported NICs unchanged. */
    if (snp->Mode->MediaPresentSupported) {
        uint32_t irq;
        snp->GetStatus(snp, &irq, NULL);
        if (!snp->Mode->MediaPresent) return;
    }
    udp_frame(bcast_mac, bcast_ip, port, port, data, len);
}

//--------------------------------------------------------------------+
// Receiving. With the guest's NE2000 bridged to this NIC (net_guest_*), the
// two share it and its MAC: UDP to the monitor's own ports (netfs 5556,
// control 5557) and ARP for the monitor's address stay here, everything else
// is the guest's. Frames are taken from the firmware by whichever side asks
// first, so each side's are queued for the other.
//--------------------------------------------------------------------+

#define FRAME_MAX 1536
#define GUEST_Q 64
#define MON_Q 8
static bool bridged;
static uint8_t guest_q[GUEST_Q][FRAME_MAX];
static uint16_t guest_len[GUEST_Q];
static unsigned guest_r, guest_w;
static uint8_t mon_q[MON_Q][FRAME_MAX];
static uint16_t mon_len[MON_Q];
static unsigned mon_r, mon_w;
uint32_t g_net_guest_rx, g_net_guest_tx, g_net_guest_dropped;

// The UDP payload of a frame for the monitor and its port, or NULL
static const uint8_t *monitor_udp(const uint8_t *f, size_t flen, uint16_t *port, size_t *out_len) {
    if (flen < 14 + 28 || f[12] != 0x08 || f[13] != 0x00) return NULL;
    const uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 4 || ip[9] != 17) return NULL;
    // ours, or a broadcast when the control port is open to them (no address
    // of our own when started from the eMMC)
    if (memcmp(ip + 16, my_ip, 4) && !(g_net_accept_broadcast && ip[19] == 0xff)) return NULL;
    int ihl = (ip[0] & 15) * 4;
    const uint8_t *udp = ip + ihl;
    uint16_t dport = (udp[2] << 8) | udp[3];
    uint16_t ulen = (udp[4] << 8) | udp[5];
    if (dport < 5555 || dport > 5557 || ulen < 8 || (size_t)(udp - f) + ulen > flen) return NULL;
    *port = dport;
    *out_len = ulen - 8;
    return udp + 8;
}

static void to_guest(const uint8_t *f, size_t flen) {
    if (!bridged) return;
    if (guest_w - guest_r >= GUEST_Q) { g_net_guest_dropped++; return; }
    memcpy(guest_q[guest_w % GUEST_Q], f, flen);
    guest_len[guest_w % GUEST_Q] = (uint16_t)flen;
    guest_w++;
}

// One frame from the firmware into rx_frame, sorted. Returns its length when
// it is the monitor's (UDP to one of its ports), else 0; -1 when there was none.
static int receive_one(void) {
    uint64_t hsize = 0, bsize = sizeof(rx_frame);
    if (EFI_ERROR(snp->Receive(snp, &hsize, &bsize, rx_frame, NULL, NULL, NULL))) return -1;
    if (bsize < 14) return 0;
    uint16_t type = (rx_frame[12] << 8) | rx_frame[13];
    if (type == 0x0806 && bsize >= 42) {
        uint8_t *a = rx_frame + 14;
        uint16_t op = (a[6] << 8) | a[7];
        if (!memcmp(a + 14, server_ip, 4)) {
            memcpy(server_mac, a + 8, 6);
            server_mac_known = true;
        }
        bool for_us = my_ip[0] && !memcmp(a + 24, my_ip, 4);
        if (op == 1 && for_us) arp_send(2, a + 8, a + 14);
        // the guest sees every ARP: on the development cable it gets the same
        // address as the monitor, and answers for itself with the same MAC
        to_guest(rx_frame, bsize);
        return 0;
    }
    uint16_t port;
    size_t len;
    if (monitor_udp(rx_frame, bsize, &port, &len)) {
        const uint8_t *ip = rx_frame + 14;
        if (!memcmp(ip + 12, server_ip, 4) && !server_mac_known) {
            memcpy(server_mac, rx_frame + 6, 6);
            server_mac_known = true;
        }
        return (int)bsize;
    }
    to_guest(rx_frame, bsize);
    return 0;
}

const uint8_t *net_poll(uint16_t port, size_t *len) {
    if (!snp) return NULL;
    uint16_t p;
    // one that arrived while the guest was asking
    for (unsigned i = mon_r; i != mon_w; i++) {
        const uint8_t *pl = monitor_udp(mon_q[i % MON_Q], mon_len[i % MON_Q], &p, len);
        if (!pl || p != port) continue;
        memcpy(rx_frame, mon_q[i % MON_Q], mon_len[i % MON_Q]);
        pl = rx_frame + (pl - mon_q[i % MON_Q]);
        // close the gap
        for (unsigned k = i; k + 1 != mon_w; k++) {
            memcpy(mon_q[k % MON_Q], mon_q[(k + 1) % MON_Q], mon_len[(k + 1) % MON_Q]);
            mon_len[k % MON_Q] = mon_len[(k + 1) % MON_Q];
        }
        mon_w--;
        return pl;
    }
    for (;;) {
        int n = receive_one();
        if (n < 0) return NULL;
        if (!n) continue;
        const uint8_t *pl = monitor_udp(rx_frame, (size_t)n, &p, len);
        if (p == port) return pl;
        // another of the monitor's ports: kept for its own poll
        if (mon_w - mon_r < MON_Q) {
            memcpy(mon_q[mon_w % MON_Q], rx_frame, (size_t)n);
            mon_len[mon_w % MON_Q] = (uint16_t)n;
            mon_w++;
        }
    }
}

//--------------------------------------------------------------------+
// The guest's side (platform/wyse.c binds the NE2000 to these)
//--------------------------------------------------------------------+

bool net_guest_bind(uint8_t mac[6]) {
    if (!snp) return false;
    memcpy(mac, my_mac, 6);
    bridged = true;
    return true;
}

int net_guest_poll(uint8_t *buf, int cap) {
    if (!snp) return 0;
    if (guest_r == guest_w) {
        for (int i = 0; i < 16; i++) {
            int n = receive_one();
            if (n < 0) break;
            if (n && mon_w - mon_r < MON_Q) {
                memcpy(mon_q[mon_w % MON_Q], rx_frame, (size_t)n);
                mon_len[mon_w % MON_Q] = (uint16_t)n;
                mon_w++;
            }
            if (guest_r != guest_w) break;
        }
    }
    if (guest_r == guest_w) return 0;
    int n = guest_len[guest_r % GUEST_Q];
    if (n > cap) n = cap;
    memcpy(buf, guest_q[guest_r % GUEST_Q], (size_t)n);
    guest_r++;
    g_net_guest_rx++;
    return n;
}

int net_guest_send(const uint8_t *buf, int len) {
    if (!snp || len < 14 || len > 1514) return 0;
    memcpy(tx_frame, buf, (size_t)len);
    tx((size_t)len);
    g_net_guest_tx++;
    return 1;
}

bool net_resolve_server(void) {
    if (!snp) return false;
    for (int tries = 0; tries < 20 && !server_mac_known; tries++) {
        arp_send(1, NULL, server_ip);
        uint64_t t0 = rdtsc();
        while (!server_mac_known && rdtsc() - t0 < 1440ULL * 100000) {   // 100 ms
            size_t l;
            net_poll(0, &l);
        }
    }
    return server_mac_known;
}

void net_send_server(uint16_t sport, uint16_t dport, const void *data, size_t len) {
    if (!snp || !server_mac_known) return;
    udp_frame(server_mac, server_ip, sport, dport, data, len);
}
