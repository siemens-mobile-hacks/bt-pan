#pragma once

#include <net/ethernet.h>
#include <stddef.h>
#include <stdint.h>

#define BNEP_UUID16 0x02u

#define BNEP_SVC_PANU 0x1115u
#define BNEP_SVC_NAP 0x1116u
#define BNEP_SVC_GN 0x1117u

#define BNEP_GENERAL 0x00u
#define BNEP_CONTROL 0x01u
#define BNEP_COMPRESSED 0x02u
#define BNEP_COMPRESSED_SRC_ONLY 0x03u
#define BNEP_COMPRESSED_DST_ONLY 0x04u

#define BNEP_SETUP_CONN_REQ 0x01u
#define BNEP_SETUP_CONN_RSP 0x02u
#define BNEP_FILTER_NET_TYPE_SET 0x03u
#define BNEP_FILTER_NET_TYPE_RSP 0x04u
#define BNEP_FILTER_MULT_ADDR_SET 0x05u
#define BNEP_FILTER_MULT_ADDR_RSP 0x06u

#define BNEP_SUCCESS 0x00u
#define BNEP_FILTER_UNSUPPORTED_REQ 0x01u

#define BNEP_TYPE_MASK 0x7fu
#define BNEP_EXT_HEADER 0x80u

struct bnep_setup_conn_req {
	uint8_t type;
	uint8_t ctrl;
	uint8_t uuid_size;
	uint8_t service[];
} __attribute__((packed));

struct bnep_set_filter_req {
	uint8_t type;
	uint8_t ctrl;
	uint16_t len;
	uint8_t list[];
} __attribute__((packed));

struct bnep_control_rsp {
	uint8_t type;
	uint8_t ctrl;
	uint16_t resp;
} __attribute__((packed));

struct bnep_ext_hdr {
	uint8_t type;
	uint8_t len;
	uint8_t data[];
} __attribute__((packed));

struct bnep_hdr {
	uint8_t type;
} __attribute__((packed));

struct bnep_general_hdr {
	uint8_t type;
	struct ether_header ethernet;
} __attribute__((packed));

struct bnep_compressed_hdr {
	uint8_t type;
	uint16_t protocol;
} __attribute__((packed));

struct bnep_compressed_addr_hdr {
	uint8_t type;
	uint8_t address[ETHER_ADDR_LEN];
	uint16_t protocol;
} __attribute__((packed));

_Static_assert(sizeof(struct bnep_setup_conn_req) == 3, "BNEP setup request header size");
_Static_assert(sizeof(struct bnep_set_filter_req) == 4, "BNEP filter request header size");
_Static_assert(sizeof(struct bnep_control_rsp) == 4, "BNEP control response size");
_Static_assert(sizeof(struct bnep_ext_hdr) == 2, "BNEP extension header size");
_Static_assert(sizeof(struct bnep_hdr) == 1, "BNEP header size");
_Static_assert(sizeof(struct bnep_general_hdr) == 15, "BNEP general Ethernet header size");
_Static_assert(sizeof(struct bnep_compressed_hdr) == 3, "BNEP compressed header size");
_Static_assert(sizeof(struct bnep_compressed_addr_hdr) == 9, "BNEP compressed address header size");

size_t bnep_encode_setup_request(uint8_t *packet, size_t capacity, uint16_t destination_service,
								 uint16_t source_service);
int bnep_validate_setup_response(const struct bnep_control_rsp *response, size_t length);
int bnep_validate_filter_request(const struct bnep_set_filter_req *request, size_t length);
size_t bnep_encode_control_response(uint8_t *packet, size_t capacity, uint8_t control_type,
								uint16_t response_code);
size_t bnep_encode_ethernet_frame(uint8_t *packet, size_t capacity,
								  const uint8_t *ethernet_frame, size_t frame_length);
int bnep_decode_network_packet(const uint8_t *packet, size_t length, uint16_t *protocol,
							   const uint8_t **payload, size_t *payload_length);
