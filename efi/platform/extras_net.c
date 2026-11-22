#include "extras_net.h"
#include "net.h"

static const EFI_GUID tcp_binding_id = {0x00720665,0x67eb,0x4a99,{0xba,0xf7,0xd3,0xc3,0x3a,0x1c,0x7c,0xc9}};
static const EFI_GUID tcp_id = {0x65530bc7,0xa359,0x410f,{0xb0,0x10,0x5a,0xad,0xc7,0xec,0x2b,0x62}};
static const EFI_GUID dhcp_binding_id = {0x9d9a39d8,0xbd42,0x4a73,{0xa4,0xd5,0x8e,0xe9,0x4b,0xe1,0x13,0x80}};
static const EFI_GUID dhcp_id = {0x8a219718,0x4ef5,0x4761,{0x91,0xc8,0xc0,0xf0,0x4b,0xda,0x9e,0x56}};
static const EFI_GUID snp_id = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static const EFI_GUID pxe_id = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
static const EFI_GUID loaded_id = EFI_LOADED_IMAGE_PROTOCOL_GUID;

static Binding *tcp_binding, *dhcp_binding;
static EFI_HANDLE controller, dhcp_handle;
static EFI_EVENT dhcp_event;
static Dhcp *dhcp;
static bool resume_net, dhcp_owned;
static bool pxe_lease;
static ExtrasConfig pxe_address;

static bool event_create(EFI_EVENT *event) {
    return !EFI_ERROR(gBS->CreateEvent(0, 0, NULL, NULL, event));
}

static void dhcp_options(const uint8_t *bytes, unsigned size, ExtrasConfig *out) {
    static const uint8_t cookie[4] = {99, 130, 83, 99};
    if (size < 240 || memcmp(bytes + 236, cookie, 4)) return;
    for (unsigned i = 240; i < size;) {
        unsigned tag = bytes[i++];
        if (tag == 255) break;
        if (!tag) continue;
        if (i == size) break;
        unsigned length = bytes[i++];
        if (length > size - i) break;
        if (length >= 4) {
            if (tag == 3) memcpy(out->gateway, bytes + i, 4);
            if (tag == 6) memcpy(out->dns, bytes + i, 4);
        }
        i += length;
    }
}

static void remember_pxe_lease(void) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_PXE_BASE_CODE_PROTOCOL *pxe;
    if (EFI_ERROR(gBS->HandleProtocol(gImage, &loaded_id, (void **)&loaded)) ||
        EFI_ERROR(gBS->HandleProtocol(loaded->DeviceHandle, &pxe_id, (void **)&pxe)) ||
        !pxe->Mode->Started || !pxe->Mode->DhcpAckReceived || !pxe->Mode->StationIp.v4[0]) return;
    pxe_lease = true;
    memcpy(pxe_address.ip, pxe->Mode->StationIp.v4, 4);
    memcpy(pxe_address.mask, pxe->Mode->SubnetMask.v4, 4);
    dhcp_options(pxe->Mode->DhcpAck, sizeof(pxe->Mode->DhcpAck), &pxe_address);
}

bool extras_net_pxe_lease(ExtrasConfig *out) {
    if (!pxe_lease) return false;
    memcpy(out->ip, pxe_address.ip, 4);
    memcpy(out->mask, pxe_address.mask, 4);
    memcpy(out->gateway, pxe_address.gateway, 4);
    memcpy(out->dns, pxe_address.dns, 4);
    return true;
}

bool extras_net_begin(void) {
    if (tcp_binding) return true;
    remember_pxe_lease();
    if (net_active()) {
        if (!net_release_for_firmware()) return false;
        resume_net = true;
    }
    uint64_t count;
    EFI_HANDLE *handles;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &snp_id, NULL, &count, &handles))) {
        for (uint64_t i = 0; i < count && !tcp_binding; i++) {
            if (EFI_ERROR(gBS->HandleProtocol(handles[i], &tcp_binding_id, (void **)&tcp_binding)))
                gBS->ConnectController(handles[i], NULL, NULL, true);
            if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &tcp_binding_id, (void **)&tcp_binding)))
                controller = handles[i];
        }
        gBS->FreePool(handles);
    }
    if (!tcp_binding && !EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &tcp_binding_id, NULL, &count, &handles))) {
        if (count && !EFI_ERROR(gBS->HandleProtocol(handles[0], &tcp_binding_id, (void **)&tcp_binding)))
            controller = handles[0];
        gBS->FreePool(handles);
    }
    return tcp_binding != NULL;
}

void extras_net_end_dhcp(void) {
    if (dhcp_owned) dhcp->stop(dhcp);
    if (dhcp_handle) dhcp_binding->destroy(dhcp_binding, dhcp_handle);
    if (dhcp_event) gBS->CloseEvent(dhcp_event);
    dhcp = NULL;
    dhcp_handle = NULL;
    dhcp_event = NULL;
    dhcp_binding = NULL;
    dhcp_owned = false;
}

void extras_net_end(void) {
    extras_net_end_dhcp();
    tcp_binding = NULL;
    controller = NULL;
    if (resume_net) net_init();
    resume_net = false;
}

bool extras_net_dhcp(void) {
    static uint32_t timeouts[] = {2, 4};
    DhcpConfig config = {.discover_try = 2, .discover_timeout = timeouts,
                         .request_try = 2, .request_timeout = timeouts};
    if (EFI_ERROR(gBS->HandleProtocol(controller, &dhcp_binding_id, (void **)&dhcp_binding))) return false;
    if (EFI_ERROR(dhcp_binding->create(dhcp_binding, &dhcp_handle))) return false;
    if (EFI_ERROR(gBS->HandleProtocol(dhcp_handle, &dhcp_id, (void **)&dhcp)) ||
        !event_create(&dhcp_event)) return false;
    if (EFI_ERROR(dhcp->configure(dhcp, &config))) {
        DhcpMode mode;
        return !EFI_ERROR(dhcp->mode(dhcp, &mode)) && mode.state >= 4 && mode.state <= 6;
    }
    dhcp_owned = true;
    EFI_STATUS result = dhcp->start(dhcp, dhcp_event);
    if (!EFI_ERROR(result)) return true;
    DhcpMode mode;
    return !EFI_ERROR(dhcp->mode(dhcp, &mode)) && mode.state >= 4 && mode.state <= 6;
}

int extras_net_lease(ExtrasConfig *out) {
    DhcpMode mode;
    if (!dhcp || EFI_ERROR(dhcp->mode(dhcp, &mode))) return -1;
    if (mode.state >= 4 && mode.state <= 6) {
        memcpy(out->ip, mode.client, 4);
        memcpy(out->mask, mode.mask, 4);
        memcpy(out->gateway, mode.router, 4);
        memset(out->dns, 0, 4);
        const DhcpPacket *packet = mode.reply;
        if (packet && packet->size >= 8 && packet->length <= packet->size - 8)
            dhcp_options(packet->message, packet->length, out);
        return 1;
    }
    return gBS->CheckEvent(dhcp_event) == EFI_SUCCESS ? -1 : 0;
}

void extras_net_drop(ExtrasSocket *s) {
    if (s->tcp) s->tcp->configure(s->tcp, NULL);
    if (s->accept.child) tcp_binding->destroy(tcp_binding, s->accept.child);
    if (s->handle) tcp_binding->destroy(tcp_binding, s->handle);
    EFI_EVENT events[] = {s->accept.done.event, s->rx.done.event, s->tx.done.event, s->close.done.event};
    for (unsigned i = 0; i < sizeof(events)/sizeof(events[0]); i++)
        if (events[i]) gBS->CloseEvent(events[i]);
    memset(s, 0, sizeof(*s));
}

static bool default_route(Tcp *tcp, const uint8_t *gateway) {
    static const uint8_t zero[4];
    if (!memcmp(gateway, zero, 4)) return true;
    EFI_STATUS status = tcp->routes(tcp, false, zero, zero, gateway);
    return status == EFI_SUCCESS || status == EFI_ERR(15); // already installed
}

bool extras_net_listen(ExtrasSocket *s, const ExtrasConfig *config, uint16_t port) {
    TcpConfig tcp_config = {.ttl = 64, .access.port = port};
    memcpy(tcp_config.access.station, config->ip, 4);
    memcpy(tcp_config.access.mask, config->mask, 4);
    if (!EFI_ERROR(tcp_binding->create(tcp_binding, &s->handle)) &&
        !EFI_ERROR(gBS->HandleProtocol(s->handle, &tcp_id, (void **)&s->tcp)) &&
        !EFI_ERROR(s->tcp->configure(s->tcp, &tcp_config)) &&
        default_route(s->tcp, config->gateway) &&
        event_create(&s->accept.done.event) &&
        extras_net_wait_client(s)) return true;
    extras_net_drop(s);
    return false;
}

bool extras_net_accept(ExtrasSocket *listener, ExtrasSocket *connection, uint8_t *peer) {
    if (!extras_net_done(listener, &listener->accept.done) ||
        EFI_ERROR(listener->accept.done.status) || !listener->accept.child) return false;
    connection->handle = listener->accept.child;
    listener->accept.child = NULL;
    // GetModeData can fill the caller's optional ControlOption buffer.
    TcpConfig config = {0};
    if (!EFI_ERROR(gBS->HandleProtocol(connection->handle, &tcp_id, (void **)&connection->tcp)) &&
        !EFI_ERROR(connection->tcp->mode(connection->tcp, NULL, &config, NULL, NULL, NULL))) {
        memcpy(peer, config.access.remote, 4);
        return true;
    }
    extras_net_drop(connection);
    extras_net_wait_client(listener);
    return false;
}

bool extras_net_wait_client(ExtrasSocket *s) {
    s->accept.done.status = EFI_NOT_READY;
    return !EFI_ERROR(s->tcp->accept(s->tcp, &s->accept));
}

static EFI_STATUS transfer(ExtrasSocket *s, IoToken *io, TcpPacket *packet, void *buffer, unsigned length, bool send) {
    if (!io->done.event) {
        EFI_STATUS status = gBS->CreateEvent(0, 0, NULL, NULL, &io->done.event);
        if (EFI_ERROR(status)) return status;
    }
    *packet = (TcpPacket){.push = send, .len = length, .count = 1, .frag = {{length, buffer}}};
    io->packet = packet;
    io->done.status = EFI_NOT_READY;
    return send ? s->tcp->send(s->tcp, io) : s->tcp->receive(s->tcp, io);
}

EFI_STATUS extras_net_receive(ExtrasSocket *s, void *buffer, unsigned length) {
    return transfer(s, &s->rx, &s->rx_packet, buffer, length, false);
}

bool extras_net_send(ExtrasSocket *s, void *buffer, unsigned length) {
    return !EFI_ERROR(transfer(s, &s->tx, &s->tx_packet, buffer, length, true));
}

bool extras_net_finish(ExtrasSocket *s) {
    s->close.done.status = EFI_NOT_READY;
    return event_create(&s->close.done.event) && !EFI_ERROR(s->tcp->close(s->tcp, &s->close));
}

bool extras_net_done(ExtrasSocket *s, const Completion *token) {
    if (!s->tcp || !token->event) return false;
    s->tcp->poll(s->tcp);
    return gBS->CheckEvent(token->event) == EFI_SUCCESS;
}
