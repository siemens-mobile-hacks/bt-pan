#include <swilib.h>

#include "bnep.h"

int bnep_validate_setup_response(const struct bnep_control_rsp *response, size_t length) {
	if (length < sizeof(*response))
		return 0;

	if ((response->type & BNEP_TYPE_MASK) != BNEP_CONTROL)
		return 0;

	return response->ctrl == BNEP_SETUP_CONN_RSP;
}

int bnep_validate_filter_request(const struct bnep_set_filter_req *request, size_t length) {
	size_t filter_length;

	if (length < sizeof(*request))
		return 0;

	if ((request->type & BNEP_TYPE_MASK) != BNEP_CONTROL)
		return 0;

	if (request->ctrl != BNEP_FILTER_NET_TYPE_SET &&
		request->ctrl != BNEP_FILTER_MULT_ADDR_SET)
		return 0;

	filter_length = ntohs(request->len);
	return filter_length <= length - sizeof(*request);
}

size_t bnep_encode_setup_request(uint8_t *packet, size_t capacity, uint16_t destination_service,
								 uint16_t source_service) {
	const size_t request_size = sizeof(struct bnep_setup_conn_req) + 2 * sizeof(uint16_t);
	struct bnep_setup_conn_req *request;
	uint16_t services[2];

	if (capacity < request_size)
		return 0;

	request = (struct bnep_setup_conn_req *)packet;
	request->type = BNEP_CONTROL;
	request->ctrl = BNEP_SETUP_CONN_REQ;
	request->uuid_size = BNEP_UUID16;
	services[0] = htons(destination_service);
	services[1] = htons(source_service);
	memcpy(request->service, services, sizeof(services));
	return request_size;
}

size_t bnep_encode_control_response(uint8_t *packet, size_t capacity, uint8_t control_type,
								uint16_t response_code) {
	struct bnep_control_rsp *response;

	if (capacity < sizeof(*response))
		return 0;

	response = (struct bnep_control_rsp *)packet;
	response->type = BNEP_CONTROL;
	response->ctrl = control_type;
	response->resp = htons(response_code);
	return sizeof(*response);
}

size_t bnep_encode_ethernet_frame(uint8_t *packet, size_t capacity,
								  const uint8_t *ethernet_frame, size_t frame_length) {
	const size_t packet_size = offsetof(struct bnep_general_hdr, ethernet) + frame_length;
	struct bnep_general_hdr *general;

	if (frame_length < sizeof(struct ether_header))
		return 0;

	if (capacity < packet_size)
		return 0;

	general = (struct bnep_general_hdr *)packet;
	general->type = BNEP_GENERAL;
	memcpy(&general->ethernet, ethernet_frame, frame_length);
	return packet_size;
}

int bnep_decode_network_packet(const uint8_t *packet, size_t length, uint16_t *protocol,
							   const uint8_t **payload, size_t *payload_length) {
	const struct bnep_hdr *header;
	const struct bnep_general_hdr *general;
	const struct bnep_compressed_hdr *compressed;
	const struct bnep_compressed_addr_hdr *compressed_addr;
	const struct bnep_ext_hdr *extension_header;
	uint8_t packet_type;
	size_t offset;
	int has_extension;

	if (length < sizeof(*header))
		return 0;

	header = (const struct bnep_hdr *)packet;
	packet_type = header->type & BNEP_TYPE_MASK;
	has_extension = (header->type & BNEP_EXT_HEADER) != 0;
	switch (packet_type) {
		case BNEP_GENERAL:
			if (length < sizeof(*general))
				return 0;
			general = (const struct bnep_general_hdr *)packet;
			*protocol = ntohs(general->ethernet.ether_type);
			offset = sizeof(*general);
			break;
		case BNEP_COMPRESSED:
			if (length < sizeof(*compressed))
				return 0;
			compressed = (const struct bnep_compressed_hdr *)packet;
			*protocol = ntohs(compressed->protocol);
			offset = sizeof(*compressed);
			break;
		case BNEP_COMPRESSED_SRC_ONLY:
		case BNEP_COMPRESSED_DST_ONLY:
			if (length < sizeof(*compressed_addr))
				return 0;
			compressed_addr = (const struct bnep_compressed_addr_hdr *)packet;
			*protocol = ntohs(compressed_addr->protocol);
			offset = sizeof(*compressed_addr);
			break;
		default:
			return 0;
	}

	while (has_extension) {
		size_t extension_length;

		if (length - offset < sizeof(*extension_header))
			return 0;

		extension_header = (const struct bnep_ext_hdr *)(packet + offset);
		extension_length = extension_header->len;
		has_extension = (extension_header->type & BNEP_EXT_HEADER) != 0;
		if (extension_length > length - offset - sizeof(*extension_header))
			return 0;

		offset += sizeof(*extension_header) + extension_length;
	}

	*payload = packet + offset;
	*payload_length = length - offset;
	return 1;
}
