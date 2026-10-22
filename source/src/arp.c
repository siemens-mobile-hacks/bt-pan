#include <swilib.h>

#include "arp.h"

int arp_validate_request(const struct ether_arp *request, size_t length,
						 const uint8_t local_ip[4]) {
	if (length < sizeof(*request))
		return 0;

	if (ntohs(request->ea_hdr.ar_hrd) != ARPHRD_ETHER)
		return 0;

	if (ntohs(request->ea_hdr.ar_pro) != ETHERTYPE_IP)
		return 0;

	if (request->ea_hdr.ar_hln != sizeof(request->arp_sha) ||
		request->ea_hdr.ar_pln != sizeof(request->arp_spa))
		return 0;

	if (ntohs(request->ea_hdr.ar_op) != ARPOP_REQUEST)
		return 0;

	if (memcmp(request->arp_tpa, local_ip, sizeof(request->arp_tpa)))
		return 0;

	return 1;
}

size_t arp_encode_reply(uint8_t *frame, size_t capacity,
						const struct ether_arp *request,
						const uint8_t local_mac[ETHER_ADDR_LEN], const uint8_t local_ip[4]) {
	struct arp_ethernet_frame *reply;

	if (capacity < sizeof(*reply))
		return 0;

	reply = (struct arp_ethernet_frame *)frame;
	memcpy(reply->ethernet.ether_dhost, request->arp_sha,
		   sizeof(reply->ethernet.ether_dhost));
	memcpy(reply->ethernet.ether_shost, local_mac, sizeof(reply->ethernet.ether_shost));
	reply->ethernet.ether_type = htons(ETHERTYPE_ARP);

	reply->arp.ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
	reply->arp.ea_hdr.ar_pro = htons(ETHERTYPE_IP);
	reply->arp.ea_hdr.ar_hln = sizeof(reply->arp.arp_sha);
	reply->arp.ea_hdr.ar_pln = sizeof(reply->arp.arp_spa);
	reply->arp.ea_hdr.ar_op = htons(ARPOP_REPLY);
	memcpy(reply->arp.arp_sha, local_mac, sizeof(reply->arp.arp_sha));
	memcpy(reply->arp.arp_spa, local_ip, sizeof(reply->arp.arp_spa));
	memcpy(reply->arp.arp_tha, request->arp_sha, sizeof(reply->arp.arp_tha));
	memcpy(reply->arp.arp_tpa, request->arp_spa, sizeof(reply->arp.arp_tpa));
	return sizeof(*reply);
}
