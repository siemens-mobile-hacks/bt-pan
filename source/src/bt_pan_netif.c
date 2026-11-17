#include <swilib.h>

#include <stddef.h>
#include <string.h>

#include "arp.h"
#include "bnep.h"
#include "bt_pan_netif.h"
#include "config.h"
#include "dhcp.h"
#include "pan_lman.h"
#include "ping.h"
#include "speedtest.h"
#include "lib.h"
#ifdef SGOLD
#include "bt_sg.h"
#include "sg_tcpwin.h"
#endif

#ifndef SGOLD
/* NSG (EL71 sw45, S75 sw52) TCP receive window. ENIP accounts packet bytes per socket: used u32[]
 * and limits u32[] in RAM (init EL71 A097B2AC / S75 A0998EB8: pools 0-6 from the flash table
 * EL71 A0C49574 / S75 A0C0705C (identical), then socket RX normal, TX normal = 5000, RX/TX big =
 * 20000, drivers 6000). EL71: 12 sockets (RX 7-0x12, TX 0x13-0x1E, big RX 0x1F-0x22, big TX
 * 0x23-0x26, 0x2F accounts); S75: 20 (RX 7-0x1A, TX 0x1B-0x2E, big RX 0x2F-0x34, big TX
 * 0x35-0x3A, 0x43 accounts). enip_tcph_sock_init (EL71 A0A6946C) sets the window to
 * 0.9 * (limit - used) - 1500 (EL71 A0A6944E), so a normal socket offers 3000 (S65 offers 6507).
 * socket() hands out a big pair only while one is free (EL71 A097BB2E); A097BD2A does not free a
 * big RX slot closed with unread data, so after a while every browser socket is normal. Raising
 * the normal RX limits gives new sockets 0.9 * BT_PAN_EL71_RX_LIMIT - 1500. Only accounting:
 * packets still come from ENIP's heaps. ENIP_PacketAlloc (EL71 A097B468, S75 A0999072): starving
 * pool u8 at used - 0x0C, failed-allocation count u16 at -0x0A. */
#define BT_PAN_EL71_RX_FIRST 7u
#define BT_PAN_EL71_RX_STOCK 5000u
struct bt_pan_nsg_enip {
	uint32_t alloc;               /* library 46A ENIP_PacketAlloc: identifies the firmware */
	volatile uint32_t *used;      /* bytes in use per account */
	volatile uint32_t *limits;    /* limit per account */
	uint8_t rx_end;               /* normal socket RX accounts: 7 .. rx_end - 1 */
	uint8_t big_rx, big_rx_end;   /* big socket RX accounts */
};
static const struct bt_pan_nsg_enip bt_pan_nsg_enips[] = {
	{0xA097B469u, (volatile uint32_t *)0xA8E9EC98u, (volatile uint32_t *)0xA8E9ED54u, 0x13u, 0x1Fu, 0x23u}, /* EL71v45 */
	{0xA0999073u, (volatile uint32_t *)0xA8DC7F04u, (volatile uint32_t *)0xA8DC8010u, 0x1Bu, 0x2Fu, 0x35u}, /* S75v52 */
};

/* NULL on another firmware (E71: not mapped) */
static const struct bt_pan_nsg_enip *bt_pan_nsg_enip(void) {
	unsigned i;

	for (i = 0; i < sizeof(bt_pan_nsg_enips) / sizeof(bt_pan_nsg_enips[0]); ++i)
		if (LIB_ENTRY(0x46A) == bt_pan_nsg_enips[i].alloc)
			return &bt_pan_nsg_enips[i];
	return NULL;
}

#endif

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define BT_PAN_NETIF_ACTIVE_FLAGS 0xc0u

/* ENIP receive buffers (ENIP_PacketAlloc). Every pool has a byte limit; the TCP window the stack
 * offers (6507 seen on S65) is bigger than 3 full packets, and the stack keeps out-of-order
 * segments in their buffers: with only one 5000-byte pool a lost segment could never be
 * re-allocated (S65 trace D97: page stuck at ~4 KB). So one common rule: try the entries below
 * in order. Only the numbers are per platform:
 *  - SGOLD (S65/SK65): limits u16[8] at flash A13C9164 / B4613C = 6000 8000 8000 8000 10000 6000 5000
 *    3000; the quota argument is ignored. Pool 4 (10000, native PPP/GPRS receive, idle without a
 *    SIM) first, the native link pool 6 (5000, caller A13358B2) as the overflow.
 *  - ELKA (EL71): limits at A0C49574 = 4000 8000 8000 10000 6000 5000 2000; limit = pool limit *
 *    quota / 100 (A097B468). The native link pool 5 at quota 600 = 30000 for our packets only
 *    (D137 400, D138 600: windows of 16500, room for a backlog while ENIP is busy). Packets leave
 *    the pool for the socket's account as soon as ENIP reads them (A097BF76). */
#define BT_PAN_NETIF_RX_OFFSET 7
static const struct { uint8_t pool; uint16_t quota; } bt_pan_rx_pools[] = {
#ifdef SGOLD
	{4, 100}, {6, 100},
#else
	{5, 600},
#endif
};
#define BT_PAN_NETIF_RX_EXTRA 9u
#define BT_PAN_NETIF_RX_HEADROOM 4u

/* largest IP packet one L2CAP SDU carries */
#ifdef SGOLD
unsigned bt_pan_tx_max = BT_PAN_SG_IP_MTU; /* main.c raises it with the MTU patch */
#define BT_PAN_TX_MAX bt_pan_tx_max
#else
#define BT_PAN_TX_MAX ETH_DATA_LEN
#endif

#define IPV4_HEADER_SIZE 20u
#define IPV4_DESTINATION_OFFSET 16u

static void bt_pan_netif_connected(struct bt_panu *panu, void *user_data);
static void bt_pan_netif_recv(struct bt_panu *panu, const uint8_t *packet, size_t length,
							  void *user_data);
static void bt_pan_netif_disconnected(struct bt_panu *panu, void *user_data);
static void bt_pan_netif_bringup(struct bt_pan_netif *netif);
static void bt_pan_netif_dhcp_timeout(GBSTMR *timer);

const struct bt_panu_cb bt_pan_netif_cb = {
	.connected = bt_pan_netif_connected,
	.recv = bt_pan_netif_recv,
	.disconnected = bt_pan_netif_disconnected,
};

static void bt_pan_netif_up(ENIP_MODULE *module) {
	module->flags |= BT_PAN_NETIF_ACTIVE_FLAGS;
}

static void bt_pan_netif_down(ENIP_MODULE *module) {
	module->flags &= (uint8_t)~BT_PAN_NETIF_ACTIVE_FLAGS;
}

volatile uint32_t bt_pan_rx_bytes, bt_pan_tx_bytes; /* IPv4 bytes, for the UI */
#ifdef SGOLD
/* ENIP RAM (read by ENIP_PacketAlloc S65 A1335022 / SK65 A09ED21E, init S65 A1334F34 from the
 * flash table A13C9164 / A0B4613C): bytes in use u32[8], limits u16[8] right after, the starving
 * pool u8 at used-4 (fail count u16 at used-2). TCP socket table: see sg_tcpwin.h. 0 = unknown firmware. */
static volatile uint32_t *bt_pan_sg_pool_used(void) {
	switch (bt_sg_fw()) {
	case SG_FW_S65V58: return (volatile uint32_t *)0xA876E4DCu;
	case SG_FW_SK65V50: return (volatile uint32_t *)0xA8F17D90u;
	default: return NULL;
	}
}

static volatile uint16_t *bt_pan_sg_pool_limits(void) {
	volatile uint32_t *used = bt_pan_sg_pool_used();

	return used ? (volatile uint16_t *)(used + 8) : NULL;
}

static volatile uint8_t *bt_pan_sg_pool_starve(void) {
	switch (bt_sg_fw()) {
	case SG_FW_S65V58: return (volatile uint8_t *)0xA876E4D8u;
	case SG_FW_SK65V50: return (volatile uint8_t *)0xA8F17D8Cu;
	default: return NULL;
	}
}

static uint8_t *const *bt_pan_sg_socks(void) {
	switch (bt_sg_fw()) {
	case SG_FW_S65V58: return (uint8_t *const *)(0xA876D5A0u + 0x45Cu);
	case SG_FW_SK65V50: return (uint8_t *const *)(0xA8F16E54u + 0x45Cu);
	default: return NULL;
	}
}

static void bt_pan_sg_rx_limit(unsigned from, unsigned to) {
	volatile uint16_t *limits = bt_pan_sg_pool_limits();

	if (limits && limits[BT_PAN_SG_RX_POOL] == from)
		limits[BT_PAN_SG_RX_POOL] = (uint16_t)to;
}

/* every packet we send (ENIP process): raise the window of a TCP socket on its SYN */
static void bt_pan_sg_tcp_window(const uint8_t *packet, size_t length) {
	uint8_t *sock;

	if (length < 40u || packet[9] != 6u)
		return;
	LockSched();
	sock = sg_tcp_window_raise(bt_pan_sg_socks(), packet, length, BT_PAN_SG_TCP_WINDOW);
	UnlockSched();
	(void)sock;
}
#endif

unsigned bt_pan_netif_tx_max(void) {
	return BT_PAN_TX_MAX;
}

/* frame: caller's buffer, BT_PAN_FRAME_SIZE; the IPv4 packet is copied after the header */
int bt_pan_netif_send_frame(struct bt_pan_netif *netif, uint8_t *frame, const uint8_t *packet,
							size_t length) {
	struct bnep_general_hdr *bnep = (struct bnep_general_hdr *)frame;

	if (length > BT_PAN_TX_MAX || !netif->panu)
		return 0;
	bnep->type = BNEP_GENERAL;
	memcpy(bnep->ethernet.ether_dhost, netif->panu->config.peer_addr,
		   sizeof(bnep->ethernet.ether_dhost));
	memcpy(bnep->ethernet.ether_shost, netif->panu->local_addr,
		   sizeof(bnep->ethernet.ether_shost));
	bnep->ethernet.ether_type = htons(ETHERTYPE_IP);
	if (packet != (const uint8_t *)(bnep + 1))
		memcpy(bnep + 1, packet, length);
	if (!bt_panu_send(netif->panu, frame, sizeof(*bnep) + length))
		return 0;
	bt_pan_tx_bytes += length;
	return 1;
}

#ifndef SGOLD
static void bt_pan_netif_tx_retry(GBSTMR *timer) {
	struct bt_pan_netif *netif = container_of(timer, struct bt_pan_netif, tx_retry_timer);

	if (!netif->tx_retry_len)
		return;
	if (!netif->registered || !netif->panu) {
		netif->tx_retry_len = 0;
		return;
	}
	if (bt_panu_send(netif->panu, netif->tx_retry_buf, netif->tx_retry_len)) {
		netif->tx_retry_len = 0;
		return;
	}
	if (++netif->tx_retry_tries >= 50u) { /* ~0.23 s: the link is stuck, TCP takes over */
		netif->tx_retry_len = 0;
		return;
	}
	GBS_StartTimerProc(&netif->tx_retry_timer, 1, bt_pan_netif_tx_retry);
}
#endif

static void bt_pan_netif_send_ipv4(struct bt_pan_netif *netif, const uint8_t *packet,
								   size_t length) {
	struct bnep_general_hdr *bnep = (struct bnep_general_hdr *)netif->tx_buf;
	size_t bnep_length;

	if (length > BT_PAN_TX_MAX)
		return; /* dropped: bigger than one SDU */
	bt_pan_tx_bytes += length;
#ifdef SGOLD
	bt_pan_sg_tcp_window(packet, length);
#endif

	bnep->type = BNEP_GENERAL;
	memcpy(bnep->ethernet.ether_dhost, netif->panu->config.peer_addr,
		   sizeof(bnep->ethernet.ether_dhost));
	memcpy(bnep->ethernet.ether_shost, netif->panu->local_addr,
		   sizeof(bnep->ethernet.ether_shost));
	bnep->ethernet.ether_type = htons(ETHERTYPE_IP);
	memcpy(bnep + 1, packet, length);
	bnep_length = sizeof(*bnep) + length;
	if (bt_panu_send(netif->panu, netif->tx_buf, bnep_length)) {
#ifndef SGOLD
		/* a newer TCP packet went out: it carries the newer ACK and window, the old one is moot */
		if (netif->tx_retry_len && packet[9] == 6u)
			netif->tx_retry_len = 0;
#endif
	} else {
#ifndef SGOLD
		/* BT send buffers full: losing an ACK is cheap, losing the window update after a zero
		 * window stalls the peer for its persist timer (seconds). Keep the last TCP one and resend
		 * it from a 1-tick timer in this process (BTPAN). */
		if (packet[9] == 6u && netif->config.tx_message) {
			memcpy(netif->tx_retry_buf, netif->tx_buf, bnep_length);
			netif->tx_retry_len = bnep_length;
			netif->tx_retry_tries = 0;
			if (!IsTimerProc(&netif->tx_retry_timer))
				GBS_StartTimerProc(&netif->tx_retry_timer, 1, bt_pan_netif_tx_retry);
		}
#endif
	}
}

static void bt_pan_netif_run(ENIP_MODULE *module) {
	struct bt_pan_netif *netif = container_of(module, struct bt_pan_netif, module);
	ENIP_PKT *packet;

	while ((packet = zDLL_RemHead(&module->port.packets_queue))) {
		bt_pan_netif_send_ipv4(netif, packet->data, packet->size);
		enip_packet_release(packet);
	}
}

static void bt_pan_netif_notify(ENIP_MODULE *module, int event) {
	(void)module;
	(void)event;
}

/* ENIP's notify (S65 A1333B90) calls this only when the module is neither posted nor running;
 * enip_module_run (A1333B38) clears those flags and runs run_proc. Run it right here, in the
 * process that handed us the packet (usually ENIP): queued to ElfLoader's helper (SUBPROC) it
 * never ran while Java polled ENIP for its DNS answer (D83: helper starved, query never sent).
 * S65 only; EL71/E71 keep the helper. */
static void bt_pan_netif_post_run(ENIP_MODULE *module) {
	struct bt_pan_netif *netif = container_of(module, struct bt_pan_netif, module);

#ifdef SGOLD
	enip_module_run(module, &netif->run_result);
#else
	/* EL71/E71: ElfLoader's helper (SUBPROC) is shared with every elf: an internet radio decrypting
	 * TLS and parsing its stream there held our ACKs back behind its work (D137). Our own process
	 * (BTPAN, the same kind of context as the helper) runs them instead; it also drains the RX ring
	 * and ends a batch early while this is queued (rx_yield). Helper when no process is set. */
	if (netif->config.tx_message) {
		netif->tx_posted = 1;
		GBS_SendMessage(netif->config.tx_cepid, netif->config.tx_message, 0, 0, 0);
	} else {
		SUBPROC(enip_module_run, module, &netif->run_result);
	}
#endif
}

void bt_pan_netif_tx_event(struct bt_pan_netif *netif) {
	netif->tx_posted = 0;
	if (netif->registered)
		enip_module_run(&netif->module, &netif->run_result);
}

static void bt_pan_netif_packet_available(ENIP_MODULE *module) {
	(void)module;
}

#ifndef SGOLD
#define BT_PAN_NSG_POOL_STARVE(e) ((volatile uint8_t *)(e)->used - 0x0C)

static void bt_pan_el71_rx_limits(uint32_t from, uint32_t to) {
	const struct bt_pan_nsg_enip *e = bt_pan_nsg_enip();
	unsigned i;

	if (!e || from == to)
		return;
	for (i = BT_PAN_EL71_RX_FIRST; i < e->rx_end; ++i)
		if (e->limits[i] == from)
			e->limits[i] = to;
}
#endif

void bt_pan_netif_init(struct bt_pan_netif *netif, const struct bt_pan_netif_config *config) {
	memset(netif, 0, sizeof(*netif));
	netif->config = *config;
	netif->lman_connection = -1;
}

int bt_pan_netif_start(struct bt_pan_netif *netif) {
	int result;

	if (netif->registered)
		return 0;

	enip_module_init(&netif->module, "BTPAN", bt_pan_netif_up, bt_pan_netif_down,
					 bt_pan_netif_run, bt_pan_netif_notify, bt_pan_netif_post_run,
					 bt_pan_netif_packet_available);
	enip_module_port_init(&netif->module.port, &netif->module);
	enip_module_up(&netif->module);

	netif->driver.port_tx = &netif->module.port;
	netif->driver.local_ip = netif->config.local_ip;
	netif->driver.dns1 = netif->config.dns1;
	netif->driver.dns2 = netif->config.dns2;
	netif->driver.ip_version = 4;
	netif->driver.mtu = netif->config.mtu;
	netif->driver.module = &netif->module;
	netif->driver.iface_bearer_id = netif->config.driver_bearer;
	netif->driver.iface_index = netif->config.iface_index;

	result = enip_ip_register_driver(&netif->driver, 0);
	if (result != 1) {
		enip_module_down(&netif->module, 0);
		return -1;
	}
	netif->registered = true;

	netif->lman_connection = lman_connection_open(netif->config.bearer_id, 1);
	if (netif->lman_connection < 0) {
		enip_ip_unregister_driver(&netif->module);
		enip_module_down(&netif->module, 0);
		netif->registered = false;
		return -2;
	}

	lman_connection_attach_IF(netif->lman_connection, netif->config.iface_index, 0xffffu);
#ifdef SGOLD
	bt_pan_sg_rx_limit(BT_PAN_SG_RX_POOL_STOCK, BT_PAN_SG_RX_POOL_LIMIT);
#else
	bt_pan_el71_rx_limits(BT_PAN_EL71_RX_STOCK, BT_PAN_EL71_RX_LIMIT);
#endif
	return 0;
}

static int bt_pan_netif_input(struct bt_pan_netif *netif, const uint8_t *packet, size_t length) {
	ENIP_PKT *enip_packet;
	unsigned i;

	if (!netif->registered)
		return -1;

	enip_packet = NULL;
	for (i = 0; !enip_packet && i < sizeof(bt_pan_rx_pools) / sizeof(bt_pan_rx_pools[0]); ++i)
		enip_packet = enip_packet_allocate(&netif->module, (int)(length + BT_PAN_NETIF_RX_EXTRA),
										  bt_pan_rx_pools[i].pool, BT_PAN_NETIF_RX_OFFSET,
										  bt_pan_rx_pools[i].quota);
	if (!enip_packet) {
#ifdef SGOLD
		/* a failed allocation leaves "starving pool" = ours, which blocks every other pool's
		 * packets >= 100 bytes (app data, TCP segments) until ours gets one again. Native PPP
		 * waits for the free; we drop the packet and do not wait, so release the claim. */
		volatile uint8_t *starve = bt_pan_sg_pool_starve();

		if (starve) {
			LockSched();
			for (i = 0; i < sizeof(bt_pan_rx_pools) / sizeof(bt_pan_rx_pools[0]); ++i)
				if (*starve == bt_pan_rx_pools[i].pool) {
					*starve = 0;
					break;
				}
			UnlockSched();
		}
#else
		/* EL71/S75 have the same claim (A097B468, set when the heap fails within the pool's own
		 * limit): it blocks other pools' packets >= 100 bytes until ours gets one again */
		const struct bt_pan_nsg_enip *e = bt_pan_nsg_enip();

		if (e) {
			LockSched();
			for (i = 0; i < sizeof(bt_pan_rx_pools) / sizeof(bt_pan_rx_pools[0]); ++i)
				if (*BT_PAN_NSG_POOL_STARVE(e) == bt_pan_rx_pools[i].pool) {
					*BT_PAN_NSG_POOL_STARVE(e) = 0;
					break;
				}
			UnlockSched();
		}
#endif
		return -2;
	}

	enip_packet->data = (uint8_t *)enip_packet->data - BT_PAN_NETIF_RX_HEADROOM;
	memcpy(enip_packet->data, packet, length);
	enip_packet->size = length;
	enip_port_send(netif->driver.port_rx, enip_packet);
	return 0;
}

static void bt_pan_netif_send_arp_reply(struct bt_pan_netif *netif,
									const struct ether_arp *request) {
	const uint8_t *local_ip = (const uint8_t *)&netif->config.local_ip;
	uint8_t frame[sizeof(struct arp_ethernet_frame)];
	uint8_t packet[sizeof(struct arp_ethernet_frame) + sizeof(struct bnep_hdr)];
	size_t frame_length;
	size_t packet_length;

	frame_length = arp_encode_reply(frame, sizeof(frame), request, netif->panu->local_addr, local_ip);
	packet_length = bnep_encode_ethernet_frame(packet, sizeof(packet), frame, frame_length);
	bt_panu_send(netif->panu, packet, packet_length);
}

/* Gratuitous ARP announcement: tell the NAP host our IP<->MAC up front so a
 * PC-initiated ping resolves immediately instead of hitting a stale/empty
 * neighbour entry (seen as "Destination Host Unreachable"). */
static void bt_pan_netif_arp_request(struct bt_pan_netif *netif, const uint8_t *target_ip) {
	const uint8_t *local_ip = (const uint8_t *)&netif->config.local_ip;
	const uint8_t *mac = netif->panu->local_addr;
	struct arp_ethernet_frame frame;
	uint8_t packet[sizeof(frame) + sizeof(struct bnep_hdr)];
	size_t packet_length;

	memset(frame.ethernet.ether_dhost, 0xff, ETHER_ADDR_LEN);
	memcpy(frame.ethernet.ether_shost, mac, ETHER_ADDR_LEN);
	frame.ethernet.ether_type = htons(ETHERTYPE_ARP);
	frame.arp.ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
	frame.arp.ea_hdr.ar_pro = htons(ETHERTYPE_IP);
	frame.arp.ea_hdr.ar_hln = ETHER_ADDR_LEN;
	frame.arp.ea_hdr.ar_pln = 4;
	frame.arp.ea_hdr.ar_op = htons(ARPOP_REQUEST);
	memcpy(frame.arp.arp_sha, mac, ETHER_ADDR_LEN);
	memcpy(frame.arp.arp_spa, local_ip, 4);
	memset(frame.arp.arp_tha, 0, ETHER_ADDR_LEN);
	memcpy(frame.arp.arp_tpa, target_ip, 4);

	packet_length = bnep_encode_ethernet_frame(packet, sizeof(packet),
											   (const uint8_t *)&frame, sizeof(frame));
	if (packet_length)
		bt_panu_send(netif->panu, packet, packet_length);
}

static void bt_pan_netif_announce(struct bt_pan_netif *netif) {
	bt_pan_netif_arp_request(netif, (const uint8_t *)&netif->config.local_ip);
}

/* Keepalive / liveness probe: the PC answers an ARP request even when idle. */
void bt_pan_netif_arp_probe(struct bt_pan_netif *netif, uint32_t target_ip) {
	if (netif->panu && netif->registered)
		bt_pan_netif_arp_request(netif, (const uint8_t *)&target_ip);
}

static void bt_pan_netif_bringup(struct bt_pan_netif *netif) {
	if (netif->registered)
		return;
	if (bt_pan_netif_start(netif))
		return; /* ENIP start failed */
	netif->dhcp_state = BT_PAN_DHCP_BOUND;
	pan_lman_up((uint8_t)netif->config.driver_bearer, (uint16_t)netif->config.iface_index,
				netif->config.local_ip, netif->config.dns1, netif->config.dns2);
	bt_pan_netif_announce(netif);
	if (netif->config.connected)
		netif->config.connected(netif);
}

static void bt_pan_netif_dhcp_send(struct bt_pan_netif *netif) {
	uint8_t frame[320];
	uint8_t packet[1 + sizeof(frame)];
	size_t frame_len;
	size_t packet_len;

	if (netif->dhcp_state == BT_PAN_DHCP_SELECTING)
		frame_len = dhcp_build_discover(frame, sizeof(frame), netif->panu->local_addr,
										netif->dhcp_xid);
	else
		frame_len = dhcp_build_request(frame, sizeof(frame), netif->panu->local_addr,
									   netif->dhcp_xid, netif->dhcp_your_ip,
									   netif->dhcp_server_id);
	if (!frame_len)
		return;
	packet_len = bnep_encode_ethernet_frame(packet, sizeof(packet), frame, frame_len);
	if (packet_len)
		bt_panu_send(netif->panu, packet, packet_len);
}

static void bt_pan_netif_dhcp_arm(struct bt_pan_netif *netif) {
	if (IsTimerProc(&netif->dhcp_timer))
		GBS_StopTimer(&netif->dhcp_timer);
	GBS_StartTimerProc(&netif->dhcp_timer, BT_PAN_DHCP_RETX_TICKS, bt_pan_netif_dhcp_timeout);
}

static void bt_pan_netif_dhcp_start(struct bt_pan_netif *netif) {
	static uint32_t seed;
	const uint8_t *m = netif->panu->local_addr;

	netif->dhcp_xid = (((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) |
					   ((uint32_t)m[4] << 8) | m[5]) ^ (++seed);
	netif->dhcp_tries = 0;
	netif->dhcp_state = BT_PAN_DHCP_SELECTING;
	bt_pan_netif_dhcp_send(netif);
	bt_pan_netif_dhcp_arm(netif);
}

static void bt_pan_netif_dhcp_timeout(GBSTMR *timer) {
	struct bt_pan_netif *netif = container_of(timer, struct bt_pan_netif, dhcp_timer);

	if (netif->registered)
		return;
	if (++netif->dhcp_tries >= BT_PAN_DHCP_MAX_TRIES) {
		netif->dhcp_state = BT_PAN_DHCP_DISABLED;
		bt_pan_netif_bringup(netif);
		return;
	}
	bt_pan_netif_dhcp_send(netif);
	bt_pan_netif_dhcp_arm(netif);
}

static void bt_pan_netif_dhcp_rx(struct bt_pan_netif *netif, const struct dhcp_result *reply) {
	if (netif->dhcp_state == BT_PAN_DHCP_SELECTING) {
		if (reply->msg_type != DHCP_MSG_OFFER)
			return;
		netif->dhcp_your_ip = reply->your_ip;
		netif->dhcp_server_id = reply->server_id;
		netif->dhcp_state = BT_PAN_DHCP_REQUESTING;
		netif->dhcp_tries = 0;
		bt_pan_netif_dhcp_send(netif);
		bt_pan_netif_dhcp_arm(netif);
		return;
	}

	if (netif->dhcp_state == BT_PAN_DHCP_REQUESTING) {
		if (reply->msg_type == DHCP_MSG_ACK) {
			if (IsTimerProc(&netif->dhcp_timer))
				GBS_StopTimer(&netif->dhcp_timer);
			netif->config.local_ip = reply->your_ip;
			if (reply->dns1)
				netif->config.dns1 = reply->dns1;
			if (reply->dns2)
				netif->config.dns2 = reply->dns2;
			bt_pan_netif_bringup(netif);
		} else if (reply->msg_type == DHCP_MSG_NAK) {
			bt_pan_netif_dhcp_start(netif);
		}
	}
}

static void bt_pan_netif_connected(struct bt_panu *panu, void *user_data) {
	struct bt_pan_netif *netif = user_data;

	netif->panu = panu;
	if (BT_PAN_USE_DHCP) {
		bt_pan_netif_dhcp_start(netif);
		return;
	}
	netif->dhcp_state = BT_PAN_DHCP_DISABLED;
	bt_pan_netif_bringup(netif);
}

static void bt_pan_netif_recv(struct bt_panu *panu, const uint8_t *packet, size_t length,
							  void *user_data) {
	struct bt_pan_netif *netif = user_data;
	const struct ether_arp *arp;
	const uint8_t *payload;
	size_t payload_length;
	uint16_t protocol;

	(void)panu;
	if (!bnep_decode_network_packet(packet, length, &protocol, &payload, &payload_length))
		return;

	/* Before the ENIP driver is registered we have no IP yet: the only traffic we
	 * act on is our own DHCP replies. Everything else is dropped until we are bound. */
	if (!netif->registered) {
		struct dhcp_result reply;

		if ((netif->dhcp_state == BT_PAN_DHCP_SELECTING ||
			 netif->dhcp_state == BT_PAN_DHCP_REQUESTING) &&
			protocol == ETHERTYPE_IP &&
			dhcp_parse_reply(payload, payload_length, netif->panu->local_addr,
							 netif->dhcp_xid, &reply))
			bt_pan_netif_dhcp_rx(netif, &reply);
		return;
	}

	if (protocol == ETHERTYPE_IP) {
		if (payload_length < IPV4_HEADER_SIZE)
			return;

		if ((payload[IPV4_DESTINATION_OFFSET] & 0xf0u) == 0xe0u)
			return;
		bt_pan_rx_bytes += payload_length;
		if (speedtest_direct_rx(payload, payload_length))
			return; /* the direct speed test's own UDP, never shown to ENIP */
		if (ping_rx(payload, payload_length))
			return; /* our ping's echo replies */

		bt_pan_netif_input(netif, payload, payload_length);
		return;
	}

	if (protocol != ETHERTYPE_ARP)
		return;

	arp = (const struct ether_arp *)payload;
	if (!arp_validate_request(arp, payload_length, (const uint8_t *)&netif->config.local_ip))
		return;

	bt_pan_netif_send_arp_reply(netif, arp);
}

static void bt_pan_netif_disconnected(struct bt_panu *panu, void *user_data) {
	struct bt_pan_netif *netif = user_data;

	(void)panu;
	bt_pan_netif_stop(netif);
	netif->panu = NULL;
	if (netif->config.disconnected)
		netif->config.disconnected(netif);
}

void bt_pan_netif_stop(struct bt_pan_netif *netif) {
	if (IsTimerProc(&netif->dhcp_timer))
		GBS_StopTimer(&netif->dhcp_timer);
	if (IsTimerProc(&netif->tx_retry_timer))
		GBS_StopTimer(&netif->tx_retry_timer);
	netif->tx_retry_len = 0;

	if (netif->lman_connection >= 0) {
		lman_connection_close(netif->lman_connection);
		netif->lman_connection = -1;
	}

	pan_lman_down(); /* apps' contexts point at this driver */
#ifdef SGOLD
	if (netif->registered)
		bt_pan_sg_rx_limit(BT_PAN_SG_RX_POOL_LIMIT, BT_PAN_SG_RX_POOL_STOCK);
#else
	if (netif->registered)
		bt_pan_el71_rx_limits(BT_PAN_EL71_RX_LIMIT, BT_PAN_EL71_RX_STOCK);
#endif
	if (netif->registered) {
		enip_ip_unregister_driver(&netif->module);
		enip_module_down(&netif->module, 0);
		netif->registered = false;
	}
}
