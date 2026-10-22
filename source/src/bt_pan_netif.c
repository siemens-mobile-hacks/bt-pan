#include <swilib.h>

#include <stddef.h>
#include <string.h>

#include "arp.h"
#include "bnep.h"
#include "bt_pan_netif.h"
#include "config.h"
#include "dhcp.h"
#include "pan_lman.h"
#include "speedtest.h"
#include "usart.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define BT_PAN_NETIF_ACTIVE_FLAGS 0xc0u

/* Values used by the native EL71 link receive path. */
#define BT_PAN_NETIF_RX_POOL 5u
#define BT_PAN_NETIF_RX_OFFSET 7
#define BT_PAN_NETIF_RX_QUOTA 100
#define BT_PAN_NETIF_RX_EXTRA 9u
#define BT_PAN_NETIF_RX_HEADROOM 4u

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

/* frame: caller's buffer, BT_PAN_FRAME_SIZE; the IPv4 packet is copied after the header */
int bt_pan_netif_send_frame(struct bt_pan_netif *netif, uint8_t *frame, const uint8_t *packet,
							size_t length) {
	struct bnep_general_hdr *bnep = (struct bnep_general_hdr *)frame;

	if (length > ETH_DATA_LEN || !netif->panu)
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

static void bt_pan_netif_send_ipv4(struct bt_pan_netif *netif, const uint8_t *packet,
								   size_t length) {
	struct bnep_general_hdr *bnep = (struct bnep_general_hdr *)netif->tx_buf;
	size_t bnep_length;

	if (length > ETH_DATA_LEN) {
		static unsigned oversize;
		static char big_log[48];

		if (++oversize <= 5) {
			sprintf(big_log, "BT PAN TX: dropped %u-byte packet (> 1500)\r\n", (unsigned)length);
			usart_puts(big_log);
		}
		return;
	}
	bt_pan_tx_bytes += length;

	/* our TCP SYNs: the window and MSS the phone's stack offers (see BT_PAN_MTU) */
	if (length >= 40 && packet[9] == 6) {
		unsigned ihl = (packet[0] & 0x0fu) * 4u;

		if (length >= ihl + 20u && (packet[ihl + 13] & 0x02u)) {
			static char syn_log[80];
			unsigned mss = 0;

			if ((packet[ihl + 12] >> 4) * 4u > 20u && packet[ihl + 20] == 2)
				mss = (unsigned)packet[ihl + 22] << 8 | packet[ihl + 23];
			sprintf(syn_log, "TCP SYN%s to port %u: win %u mss %u\r\n",
					(packet[ihl + 13] & 0x10u) ? "-ACK" : "",
					(unsigned)packet[ihl + 2] << 8 | packet[ihl + 3],
					(unsigned)packet[ihl + 14] << 8 | packet[ihl + 15], mss);
			usart_puts(syn_log);
		}
	}

	bnep->type = BNEP_GENERAL;
	memcpy(bnep->ethernet.ether_dhost, netif->panu->config.peer_addr,
		   sizeof(bnep->ethernet.ether_dhost));
	memcpy(bnep->ethernet.ether_shost, netif->panu->local_addr,
		   sizeof(bnep->ethernet.ether_shost));
	bnep->ethernet.ether_type = htons(ETHERTYPE_IP);
	memcpy(bnep + 1, packet, length);
	bnep_length = sizeof(*bnep) + length;
	bt_panu_send(netif->panu, netif->tx_buf, bnep_length);

	/* one line per outgoing packet (ENIP process); ~5 ms of USART each, debug only */
	if (BT_PAN_LOG_PACKETS && length >= 20) {
		static char tx_log[80];
		unsigned ihl = (packet[0] & 0x0fu) * 4u, port = 0;

		if ((packet[9] == 6 || packet[9] == 17) && length >= ihl + 4u)
			port = (unsigned)packet[ihl + 2] << 8 | packet[ihl + 3];
		sprintf(tx_log, "BT PAN TX IPv4: len=%u proto=%u dst=%u.%u.%u.%u:%u\r\n",
				(unsigned)length, packet[9], packet[16], packet[17], packet[18], packet[19], port);
		usart_puts(tx_log);
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

static void bt_pan_netif_post_run(ENIP_MODULE *module) {
	struct bt_pan_netif *netif = container_of(module, struct bt_pan_netif, module);

	SUBPROC(enip_module_run, module, &netif->run_result);
}

static void bt_pan_netif_packet_available(ENIP_MODULE *module) {
	(void)module;
}

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
	return 0;
}

static int bt_pan_netif_input(struct bt_pan_netif *netif, const uint8_t *packet, size_t length) {
	ENIP_PKT *enip_packet;

	if (!netif->registered)
		return -1;

	enip_packet = enip_packet_allocate(&netif->module, (int)(length + BT_PAN_NETIF_RX_EXTRA),
									  BT_PAN_NETIF_RX_POOL,
									  BT_PAN_NETIF_RX_OFFSET, BT_PAN_NETIF_RX_QUOTA);
	if (!enip_packet)
		return -2;

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
	if (bt_pan_netif_start(netif)) {
		usart_puts("BT PAN: ENIP start failed\r\n");
		return;
	}
	netif->dhcp_state = BT_PAN_DHCP_BOUND;
	pan_lman_up((uint8_t)netif->config.driver_bearer, (uint16_t)netif->config.iface_index,
				netif->config.local_ip, netif->config.dns1, netif->config.dns2);
	sprintf(netif->panu->log_buf, "BT PAN: ENIP up, ip=%u.%u.%u.%u\r\n",
			(unsigned int)(netif->config.local_ip & 0xffu),
			(unsigned int)((netif->config.local_ip >> 8) & 0xffu),
			(unsigned int)((netif->config.local_ip >> 16) & 0xffu),
			(unsigned int)((netif->config.local_ip >> 24) & 0xffu));
	usart_puts(netif->panu->log_buf);
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
	usart_puts("BT PAN: DHCP DISCOVER\r\n");
	bt_pan_netif_dhcp_send(netif);
	bt_pan_netif_dhcp_arm(netif);
}

static void bt_pan_netif_dhcp_timeout(GBSTMR *timer) {
	struct bt_pan_netif *netif = container_of(timer, struct bt_pan_netif, dhcp_timer);

	if (netif->registered)
		return;
	if (++netif->dhcp_tries >= BT_PAN_DHCP_MAX_TRIES) {
		usart_puts("BT PAN: DHCP timeout, falling back to static IP\r\n");
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
		usart_puts("BT PAN: DHCP OFFER -> REQUEST\r\n");
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
			usart_puts("BT PAN: DHCP ACK\r\n");
			bt_pan_netif_bringup(netif);
		} else if (reply->msg_type == DHCP_MSG_NAK) {
			usart_puts("BT PAN: DHCP NAK, restarting\r\n");
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
	int result;
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

		result = bt_pan_netif_input(netif, payload, payload_length);
		if (BT_PAN_LOG_PACKETS || result) {
			unsigned ihl = (payload[0] & 0x0fu) * 4u, port = 0;

			if ((payload[9] == 6 || payload[9] == 17) && payload_length >= ihl + 4u)
				port = (unsigned)payload[ihl] << 8 | payload[ihl + 1];
			sprintf(netif->panu->log_buf,
					"BT PAN RX IPv4: len=%u proto=%u src=%u.%u.%u.%u:%u result=%d\r\n",
					(unsigned int)payload_length, payload[9], payload[12], payload[13], payload[14],
					payload[15], port, result);
			usart_puts(netif->panu->log_buf);
		}
		return;
	}

	if (protocol != ETHERTYPE_ARP)
		return;

	arp = (const struct ether_arp *)payload;
	if (!arp_validate_request(arp, payload_length, (const uint8_t *)&netif->config.local_ip))
		return;

	usart_puts("BT PAN RX ARP request: sending reply\r\n");
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

	if (netif->lman_connection >= 0) {
		lman_connection_close(netif->lman_connection);
		netif->lman_connection = -1;
	}

	pan_lman_down(); /* apps' contexts point at this driver */
	if (netif->registered) {
		enip_ip_unregister_driver(&netif->module);
		enip_module_down(&netif->module, 0);
		netif->registered = false;
	}
}
