#pragma once

#include <net/ethernet.h>
#include <net/if_arp.h>
#include <stddef.h>
#include <stdint.h>

/* The SDK provides arphdr, but not the standard Ethernet/IPv4 ARP body. */
struct ether_arp {
	struct arphdr ea_hdr;
	uint8_t arp_sha[ETHER_ADDR_LEN];
	uint8_t arp_spa[4];
	uint8_t arp_tha[ETHER_ADDR_LEN];
	uint8_t arp_tpa[4];
} __attribute__((packed));

struct arp_ethernet_frame {
	struct ether_header ethernet;
	struct ether_arp arp;
} __attribute__((packed));

_Static_assert(sizeof(struct arphdr) == 8, "ARP header size");
_Static_assert(sizeof(struct ether_arp) == 28, "Ethernet IPv4 ARP packet size");
_Static_assert(sizeof(struct arp_ethernet_frame) == 42, "ARP Ethernet frame size");

int arp_validate_request(const struct ether_arp *request, size_t length,
						 const uint8_t local_ip[4]);
size_t arp_encode_reply(uint8_t *frame, size_t capacity,
						const struct ether_arp *request,
						const uint8_t local_mac[ETHER_ADDR_LEN], const uint8_t local_ip[4]);
