#pragma once

#include <stdint.h>

#include "enip.h"

struct ipv4_header {
	uint8_t version_ihl;
	uint8_t tos;
	uint16_t length;
	uint16_t id;
	uint16_t fragment_offset;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;
	uint32_t source;
	uint32_t destination;
} __attribute__((packed));

struct icmp_echo_header {
	uint8_t type;
	uint8_t code;
	uint16_t checksum;
	uint16_t id;
	uint16_t sequence;
} __attribute__((packed));

struct icmp_probe_packet {
	struct ipv4_header ip;
	struct icmp_echo_header icmp;
	uint8_t payload[4];
} __attribute__((packed));

int icmp_probe_send(ENIP_MODULE *module, uint32_t source, uint32_t destination);
