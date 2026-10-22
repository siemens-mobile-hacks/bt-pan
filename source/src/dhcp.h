#pragma once

#include <stddef.h>
#include <stdint.h>

/* DHCP message types (option 53). */
#define DHCP_MSG_DISCOVER 1u
#define DHCP_MSG_OFFER 2u
#define DHCP_MSG_REQUEST 3u
#define DHCP_MSG_DECLINE 4u
#define DHCP_MSG_ACK 5u
#define DHCP_MSG_NAK 6u

/*
 * All IPv4 addresses in results and request arguments are in wire (big-endian)
 * order, i.e. exactly as they appear on the wire and in BNEP IP payloads - so a
 * value parsed out of an OFFER can be fed straight back into a REQUEST.
 */
struct dhcp_result {
	uint8_t msg_type;    /* DHCP_MSG_* from option 53 */
	uint32_t your_ip;    /* yiaddr - the address the server assigned us */
	uint32_t server_id;  /* option 54 */
	uint32_t subnet_mask; /* option 1, 0 if absent */
	uint32_t router;     /* option 3 (first), 0 if absent */
	uint32_t dns1;       /* option 6 (first), 0 if absent */
	uint32_t dns2;       /* option 6 (second), 0 if absent */
};

/*
 * Build a complete Ethernet frame (dst = broadcast, src = mac, type IPv4)
 * carrying a DHCP DISCOVER / REQUEST over UDP/IPv4. Returns the frame length,
 * or 0 if the buffer is too small. xid is the caller's transaction id. For a
 * REQUEST, requested_ip and server_id come from the OFFER's dhcp_result.
 */
size_t dhcp_build_discover(uint8_t *frame, size_t capacity, const uint8_t mac[6], uint32_t xid);
size_t dhcp_build_request(uint8_t *frame, size_t capacity, const uint8_t mac[6], uint32_t xid,
						  uint32_t requested_ip, uint32_t server_id);

/*
 * Parse an inbound IPv4 packet (the BNEP IP payload) as a DHCP reply addressed
 * to us. Returns 1 and fills result on a valid BOOTREPLY whose xid and chaddr
 * match, else 0.
 */
int dhcp_parse_reply(const uint8_t *ip_packet, size_t length, const uint8_t mac[6], uint32_t xid,
					 struct dhcp_result *result);
