#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void net_init(void);
bool net_active(void);
const uint8_t *net_my_ip(void);
const uint8_t *net_server_ip(void);

// UDP broadcast (log)
void net_broadcast(uint16_t port, const void *data, size_t len);

// unicast to the PC that netbooted us
bool net_resolve_server(void);
void net_send_server(uint16_t sport, uint16_t dport, const void *data, size_t len);

// Poll the NIC once through everything queued; returns the payload of the
// first UDP datagram for our address and `port`, or NULL. Answers ARP meanwhile.
// The returned buffer is valid until the next net_* call.
const uint8_t *net_poll(uint16_t port, size_t *len);

// The guest's NE2000 on this NIC: its frames out as they are, and every frame
// that is not the monitor's to it. bind hands over the NIC's MAC for the card.
bool net_guest_bind(uint8_t mac[6]);
int net_guest_poll(uint8_t *buf, int cap);
int net_guest_send(const uint8_t *buf, int len);
extern uint32_t g_net_guest_rx, g_net_guest_tx, g_net_guest_dropped;

// Also take UDP broadcasts (the control port of a monitor without an address)
extern bool g_net_accept_broadcast;

#endif
