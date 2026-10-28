#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bnep.h"
#include "bt_panu.h"
#include "dhcp.h"
#include "enip.h"

struct bt_pan_netif;

#define BT_PAN_FRAME_SIZE (sizeof(struct bnep_general_hdr) + ETH_DATA_LEN)
#define BT_PAN_FRAME_PAYLOAD(frame) ((frame) + sizeof(struct bnep_general_hdr))
extern volatile uint32_t bt_pan_rx_bytes, bt_pan_tx_bytes;

enum bt_pan_dhcp_state {
	BT_PAN_DHCP_DISABLED, /* static IP, or DHCP finished/fell back */
	BT_PAN_DHCP_SELECTING, /* DISCOVER sent, waiting for OFFER */
	BT_PAN_DHCP_REQUESTING, /* REQUEST sent, waiting for ACK */
	BT_PAN_DHCP_BOUND, /* leased, ENIP driver registered */
};

struct bt_pan_netif_config {
	uint32_t local_ip;
	uint32_t dns1;
	uint32_t dns2;
	uint32_t mtu;
	int8_t bearer_id;      /* the netif's own LMAN context (ARMenian Developer's PoC) */
	int8_t driver_bearer;  /* ENIP driver; apps' sockets match (driver_bearer, iface_index) */
	int16_t iface_index;
	void (*connected)(struct bt_pan_netif *netif);
	void (*disconnected)(struct bt_pan_netif *netif);
};

struct bt_pan_netif {
	ENIP_MODULE module;
	ENIP_IP_DRIVER driver;
	struct bt_pan_netif_config config;
	struct bt_panu *panu;
	uint32_t run_result;
	int16_t lman_connection;
	bool registered;
	enum bt_pan_dhcp_state dhcp_state;
	uint32_t dhcp_xid;
	uint32_t dhcp_server_id; /* wire order */
	uint32_t dhcp_your_ip;   /* wire order, from the OFFER */
	unsigned int dhcp_tries;
	GBSTMR dhcp_timer;
	uint8_t tx_buf[sizeof(struct bnep_general_hdr) + ETH_DATA_LEN];
};

extern const struct bt_panu_cb bt_pan_netif_cb;

void bt_pan_netif_init(struct bt_pan_netif *netif, const struct bt_pan_netif_config *config);
int bt_pan_netif_start(struct bt_pan_netif *netif);
void bt_pan_netif_stop(struct bt_pan_netif *netif);
/* largest IPv4 packet sent (the IP MTU) */
unsigned bt_pan_netif_tx_max(void);
/* Send one IPv4 packet in a caller-owned BT_PAN_FRAME_SIZE buffer (any process). 1 = sent. */
void bt_pan_netif_arp_probe(struct bt_pan_netif *netif, uint32_t target_ip); /* wire order */
int bt_pan_netif_send_frame(struct bt_pan_netif *netif, uint8_t *frame, const uint8_t *packet,
							size_t length);
