/* Host-side wire-format test for src/dhcp.c. Build:
 *   gcc -I../src -DDHCP_HOST_TEST test_dhcp.c ../src/dhcp.c -o test_dhcp && ./test_dhcp
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dhcp.h"

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

static const uint8_t mac[6] = {0x00, 0x0d, 0x41, 0x21, 0x76, 0x1d};

/* Craft a BOOTREPLY (OFFER or ACK) IP packet exactly as the host NAP would send. */
static size_t craft_reply(uint8_t *buf, uint32_t xid, uint8_t msg_type, uint32_t yiaddr_be,
						  uint32_t server_be, uint32_t mask_be, uint32_t router_be,
						  uint32_t dns_be) {
	uint8_t *p = buf;
	uint8_t *ip = p;
	uint8_t *udp;
	uint8_t *bootp;
	uint8_t *opt;
	uint16_t udp_len, ip_len;

	memset(buf, 0, 600);
	/* IP header (20 bytes) */
	ip[0] = 0x45;
	ip[9] = 17; /* UDP */
	/* src/dst filled below after we know addresses not needed by parser */
	udp = ip + 20;
	bootp = udp + 8;
	/* BOOTP */
	bootp[0] = 2;         /* BOOTREPLY */
	bootp[1] = 1;         /* ethernet */
	bootp[2] = 6;         /* hlen */
	*(uint32_t *)(bootp + 4) = htonl(xid);
	*(uint32_t *)(bootp + 16) = yiaddr_be; /* yiaddr */
	memcpy(bootp + 28, mac, 6);            /* chaddr */
	opt = bootp + 236;
	opt[0] = 0x63; opt[1] = 0x82; opt[2] = 0x53; opt[3] = 0x63;
	opt += 4;
	*opt++ = 53; *opt++ = 1; *opt++ = msg_type;
	*opt++ = 54; *opt++ = 4; memcpy(opt, &server_be, 4); opt += 4;
	*opt++ = 1;  *opt++ = 4; memcpy(opt, &mask_be, 4); opt += 4;
	*opt++ = 3;  *opt++ = 4; memcpy(opt, &router_be, 4); opt += 4;
	*opt++ = 6;  *opt++ = 4; memcpy(opt, &dns_be, 4); opt += 4;
	*opt++ = 255;

	udp_len = (uint16_t)(8 + (opt - bootp));
	*(uint16_t *)(udp + 0) = htons(67); /* sport */
	*(uint16_t *)(udp + 2) = htons(68); /* dport */
	*(uint16_t *)(udp + 4) = htons(udp_len);
	ip_len = (uint16_t)(20 + udp_len);
	*(uint16_t *)(ip + 2) = htons(ip_len);
	return ip_len;
}

int main(void) {
	uint8_t frame[600];
	uint8_t reply[600];
	size_t n;
	uint32_t xid = 0x12345678u;
	struct dhcp_result res;

	/* --- DISCOVER --- */
	n = dhcp_build_discover(frame, sizeof(frame), mac, xid);
	CHECK(n > 14 + 20 + 8 + 236 + 4);
	/* ethernet */
	for (int i = 0; i < 6; i++) CHECK(frame[i] == 0xff);
	CHECK(memcmp(frame + 6, mac, 6) == 0);
	CHECK(frame[12] == 0x08 && frame[13] == 0x00);
	/* ip */
	CHECK(frame[14] == 0x45);
	CHECK(frame[14 + 9] == 17);
	CHECK(*(uint32_t *)(frame + 14 + 12) == 0);          /* src 0.0.0.0 */
	CHECK(*(uint32_t *)(frame + 14 + 16) == 0xffffffffu); /* dst bcast */
	/* udp 68 -> 67 */
	CHECK(ntohs(*(uint16_t *)(frame + 34)) == 68);
	CHECK(ntohs(*(uint16_t *)(frame + 36)) == 67);
	/* bootp */
	CHECK(frame[42] == 1); /* op request */
	CHECK(ntohl(*(uint32_t *)(frame + 42 + 4)) == xid);
	CHECK(memcmp(frame + 42 + 28, mac, 6) == 0);
	/* cookie + option 53 = DISCOVER */
	CHECK(frame[42 + 236] == 0x63 && frame[42 + 239] == 0x63);
	CHECK(frame[42 + 240] == 53 && frame[42 + 241] == 1 && frame[42 + 242] == DHCP_MSG_DISCOVER);

	/* --- OFFER parse --- */
	craft_reply(reply, xid, DHCP_MSG_OFFER, htonl(0xc0a80702u) /*192.168.7.2*/,
				htonl(0xc0a80701u), htonl(0xffffff00u), htonl(0xc0a80701u), htonl(0x08080808u));
	CHECK(dhcp_parse_reply(reply, sizeof(reply), mac, xid, &res) == 1);
	CHECK(res.msg_type == DHCP_MSG_OFFER);
	CHECK(res.your_ip == htonl(0xc0a80702u));
	CHECK(res.server_id == htonl(0xc0a80701u));
	CHECK(res.subnet_mask == htonl(0xffffff00u));
	CHECK(res.router == htonl(0xc0a80701u));
	CHECK(res.dns1 == htonl(0x08080808u));

	/* --- REQUEST built from the offer --- */
	n = dhcp_build_request(frame, sizeof(frame), mac, xid, res.your_ip, res.server_id);
	CHECK(n > 0);
	CHECK(frame[42 + 242] == DHCP_MSG_REQUEST);
	/* option 50 (requested ip) then 54 (server id) follow the type option */
	CHECK(frame[42 + 243] == 50 && frame[42 + 244] == 4);
	CHECK(*(uint32_t *)(frame + 42 + 245) == res.your_ip);
	CHECK(frame[42 + 249] == 54 && frame[42 + 250] == 4);
	CHECK(*(uint32_t *)(frame + 42 + 251) == res.server_id);

	/* --- ACK parse --- */
	craft_reply(reply, xid, DHCP_MSG_ACK, htonl(0xc0a80702u), htonl(0xc0a80701u),
				htonl(0xffffff00u), htonl(0xc0a80701u), htonl(0x08080808u));
	CHECK(dhcp_parse_reply(reply, sizeof(reply), mac, xid, &res) == 1);
	CHECK(res.msg_type == DHCP_MSG_ACK);

	/* --- negative: wrong xid --- */
	craft_reply(reply, xid + 1, DHCP_MSG_ACK, htonl(0xc0a80702u), 0, 0, 0, 0);
	CHECK(dhcp_parse_reply(reply, sizeof(reply), mac, xid, &res) == 0);

	/* --- negative: wrong chaddr --- */
	craft_reply(reply, xid, DHCP_MSG_ACK, htonl(0xc0a80702u), 0, 0, 0, 0);
	reply[20 + 8 + 28] ^= 0xff; /* corrupt first chaddr byte */
	CHECK(dhcp_parse_reply(reply, sizeof(reply), mac, xid, &res) == 0);

	if (failures == 0)
		printf("test_dhcp: ALL PASS\n");
	else
		printf("test_dhcp: %d FAILURE(S)\n", failures);
	return failures ? 1 : 0;
}
