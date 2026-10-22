/*
 * Minimal DHCP client wire format: build DISCOVER/REQUEST and parse OFFER/ACK.
 * Pure functions over byte buffers - no firmware or BT dependency - so the same
 * code compiles into the elf and into tests/test_dhcp.c on the host
 * (-DDHCP_HOST_TEST). The netif drives the state machine and the actual send/recv.
 */
#ifdef DHCP_HOST_TEST
#include <arpa/inet.h>
#include <string.h>
#else
#include <swilib.h>
#include <string.h>
#endif

#include "dhcp.h"

#define ETH_TYPE_IPV4 0x0800u
#define IP_PROTO_UDP 17u
#define BOOTPS_PORT 67u
#define BOOTPC_PORT 68u
#define DHCP_BROADCAST_FLAG 0x8000u
#define BOOTP_OP_REQUEST 1u
#define BOOTP_OP_REPLY 2u
#define BOOTP_HTYPE_ETHER 1u

#define DHCP_OPT_SUBNET 1u
#define DHCP_OPT_ROUTER 3u
#define DHCP_OPT_DNS 6u
#define DHCP_OPT_REQUESTED_IP 50u
#define DHCP_OPT_MSG_TYPE 53u
#define DHCP_OPT_SERVER_ID 54u
#define DHCP_OPT_PARAM_LIST 55u
#define DHCP_OPT_END 255u

struct eth_hdr {
	uint8_t dst[6];
	uint8_t src[6];
	uint16_t type;
} __attribute__((packed));

struct ip4_hdr {
	uint8_t ver_ihl;
	uint8_t tos;
	uint16_t tot_len;
	uint16_t id;
	uint16_t frag;
	uint8_t ttl;
	uint8_t proto;
	uint16_t check;
	uint32_t src;
	uint32_t dst;
} __attribute__((packed));

struct udp_hdr {
	uint16_t sport;
	uint16_t dport;
	uint16_t len;
	uint16_t check;
} __attribute__((packed));

struct bootp_hdr {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
} __attribute__((packed));

static const uint8_t dhcp_magic_cookie[4] = {0x63, 0x82, 0x53, 0x63};

_Static_assert(sizeof(struct eth_hdr) == 14, "eth header size");
_Static_assert(sizeof(struct ip4_hdr) == 20, "ipv4 header size");
_Static_assert(sizeof(struct udp_hdr) == 8, "udp header size");
_Static_assert(sizeof(struct bootp_hdr) == 236, "bootp header size");

static uint16_t ip_checksum(const void *data, size_t length) {
	const uint8_t *bytes = data;
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 1 < length; i += 2)
		sum += (uint32_t)(bytes[i] << 8) | bytes[i + 1];
	if (i < length)
		sum += (uint32_t)bytes[i] << 8;
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return (uint16_t)~sum;
}

static size_t dhcp_build(uint8_t *frame, size_t capacity, const uint8_t mac[6], uint32_t xid,
						 uint8_t msg_type, uint32_t requested_ip, uint32_t server_id) {
	struct eth_hdr *eth;
	struct ip4_hdr *ip;
	struct udp_hdr *udp;
	struct bootp_hdr *bootp;
	uint8_t *options;
	uint8_t *option;
	size_t options_len;
	size_t dhcp_len;
	size_t total_len;

	/* Worst case: fixed headers + cookie + the longest option set we emit. */
	if (capacity < sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + sizeof(*bootp) + 4 + 32)
		return 0;

	memset(frame, 0, sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + sizeof(*bootp) + 4);
	eth = (struct eth_hdr *)frame;
	memset(eth->dst, 0xff, sizeof(eth->dst));
	memcpy(eth->src, mac, sizeof(eth->src));
	eth->type = htons(ETH_TYPE_IPV4);

	ip = (struct ip4_hdr *)(eth + 1);
	udp = (struct udp_hdr *)(ip + 1);
	bootp = (struct bootp_hdr *)(udp + 1);

	bootp->op = BOOTP_OP_REQUEST;
	bootp->htype = BOOTP_HTYPE_ETHER;
	bootp->hlen = 6;
	bootp->xid = htonl(xid);
	bootp->flags = htons(DHCP_BROADCAST_FLAG);
	memcpy(bootp->chaddr, mac, 6);

	options = (uint8_t *)(bootp + 1);
	memcpy(options, dhcp_magic_cookie, sizeof(dhcp_magic_cookie));
	option = options + sizeof(dhcp_magic_cookie);

	*option++ = DHCP_OPT_MSG_TYPE;
	*option++ = 1;
	*option++ = msg_type;

	if (msg_type == DHCP_MSG_REQUEST) {
		*option++ = DHCP_OPT_REQUESTED_IP;
		*option++ = 4;
		memcpy(option, &requested_ip, 4);
		option += 4;
		*option++ = DHCP_OPT_SERVER_ID;
		*option++ = 4;
		memcpy(option, &server_id, 4);
		option += 4;
	}

	*option++ = DHCP_OPT_PARAM_LIST;
	*option++ = 3;
	*option++ = DHCP_OPT_SUBNET;
	*option++ = DHCP_OPT_ROUTER;
	*option++ = DHCP_OPT_DNS;
	*option++ = DHCP_OPT_END;

	options_len = (size_t)(option - options);
	dhcp_len = sizeof(*bootp) + options_len;

	udp->sport = htons(BOOTPC_PORT);
	udp->dport = htons(BOOTPS_PORT);
	udp->len = htons((uint16_t)(sizeof(*udp) + dhcp_len));
	udp->check = 0; /* UDP checksum is optional over IPv4 */

	ip->ver_ihl = 0x45;
	ip->tot_len = htons((uint16_t)(sizeof(*ip) + sizeof(*udp) + dhcp_len));
	ip->ttl = 64;
	ip->proto = IP_PROTO_UDP;
	ip->src = 0;          /* 0.0.0.0 - we have no address yet */
	ip->dst = 0xffffffffu; /* 255.255.255.255 */
	ip->check = htons(ip_checksum(ip, sizeof(*ip)));

	total_len = sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + dhcp_len;
	return total_len;
}

size_t dhcp_build_discover(uint8_t *frame, size_t capacity, const uint8_t mac[6], uint32_t xid) {
	return dhcp_build(frame, capacity, mac, xid, DHCP_MSG_DISCOVER, 0, 0);
}

size_t dhcp_build_request(uint8_t *frame, size_t capacity, const uint8_t mac[6], uint32_t xid,
						  uint32_t requested_ip, uint32_t server_id) {
	return dhcp_build(frame, capacity, mac, xid, DHCP_MSG_REQUEST, requested_ip, server_id);
}

int dhcp_parse_reply(const uint8_t *ip_packet, size_t length, const uint8_t mac[6], uint32_t xid,
					 struct dhcp_result *result) {
	const struct ip4_hdr *ip;
	const struct udp_hdr *udp;
	const struct bootp_hdr *bootp;
	const uint8_t *options;
	const uint8_t *end;
	size_t ihl;
	size_t udp_total;
	int have_type = 0;

	if (length < sizeof(*ip))
		return 0;

	ip = (const struct ip4_hdr *)ip_packet;
	if ((ip->ver_ihl >> 4) != 4)
		return 0;
	ihl = (size_t)(ip->ver_ihl & 0x0fu) * 4u;
	if (ihl < sizeof(*ip) || ip->proto != IP_PROTO_UDP)
		return 0;
	if (length < ihl + sizeof(*udp))
		return 0;

	udp = (const struct udp_hdr *)(ip_packet + ihl);
	if (ntohs(udp->dport) != BOOTPC_PORT)
		return 0;

	udp_total = ntohs(udp->len);
	if (udp_total < sizeof(*udp) + sizeof(*bootp) + 4)
		return 0;
	if (length < ihl + udp_total)
		return 0;

	bootp = (const struct bootp_hdr *)(ip_packet + ihl + sizeof(*udp));
	if (bootp->op != BOOTP_OP_REPLY)
		return 0;
	if (bootp->xid != htonl(xid))
		return 0;
	if (memcmp(bootp->chaddr, mac, 6))
		return 0;

	options = (const uint8_t *)(bootp + 1);
	if (memcmp(options, dhcp_magic_cookie, sizeof(dhcp_magic_cookie)))
		return 0;
	options += sizeof(dhcp_magic_cookie);
	end = ip_packet + ihl + udp_total;

	memset(result, 0, sizeof(*result));
	result->your_ip = bootp->yiaddr;

	while (options < end) {
		uint8_t code = *options++;
		uint8_t len;

		if (code == DHCP_OPT_END)
			break;
		if (code == 0)
			continue; /* pad */
		if (options >= end)
			break;
		len = *options++;
		if (options + len > end)
			break;

		switch (code) {
			case DHCP_OPT_MSG_TYPE:
				if (len >= 1) {
					result->msg_type = options[0];
					have_type = 1;
				}
				break;
			case DHCP_OPT_SERVER_ID:
				if (len >= 4)
					memcpy(&result->server_id, options, 4);
				break;
			case DHCP_OPT_SUBNET:
				if (len >= 4)
					memcpy(&result->subnet_mask, options, 4);
				break;
			case DHCP_OPT_ROUTER:
				if (len >= 4)
					memcpy(&result->router, options, 4);
				break;
			case DHCP_OPT_DNS:
				if (len >= 4)
					memcpy(&result->dns1, options, 4);
				if (len >= 8)
					memcpy(&result->dns2, options + 4, 4);
				break;
			default:
				break;
		}
		options += len;
	}

	return have_type;
}
