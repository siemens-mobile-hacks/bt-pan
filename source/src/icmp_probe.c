#include <swilib.h>

#include <stddef.h>
#include <string.h>

#include "icmp_probe.h"

#define ICMP_ECHO_REQUEST 8u
#define ICMP_PROTOCOL 1u

#define ICMP_PROBE_POOL 5u
#define ICMP_PROBE_OFFSET 7
#define ICMP_PROBE_QUOTA 100
#define ICMP_PROBE_HEADROOM 8u

static uint16_t internet_checksum(const void *data, size_t length) {
	const uint8_t *bytes = data;
	uint32_t sum = 0;

	while (length >= 2) {
		sum += ((uint16_t)bytes[0] << 8) | bytes[1];
		bytes += 2;
		length -= 2;
	}

	if (length)
		sum += (uint16_t)bytes[0] << 8;

	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);

	return (uint16_t)~sum;
}

int icmp_probe_send(ENIP_MODULE *module, uint32_t source, uint32_t destination) {
	static const uint8_t payload[] = {'E', 'L', '7', '1'};
	struct icmp_probe_packet *probe;
	ENIP_PKT *packet;

	packet = enip_packet_allocate(module, sizeof(*probe) + ICMP_PROBE_HEADROOM,
								  ICMP_PROBE_POOL, ICMP_PROBE_OFFSET, ICMP_PROBE_QUOTA);
	if (!packet)
		return -1;

	probe = packet->data;
	memset(probe, 0, sizeof(*probe));
	probe->ip.version_ihl = 0x45;
	probe->ip.length = htons(sizeof(*probe));
	probe->ip.id = htons(0x7171);
	probe->ip.fragment_offset = htons(0x4000);
	probe->ip.ttl = 64;
	probe->ip.protocol = ICMP_PROTOCOL;
	probe->ip.source = source;
	probe->ip.destination = destination;
	probe->ip.checksum = htons(internet_checksum(&probe->ip, sizeof(probe->ip)));

	probe->icmp.type = ICMP_ECHO_REQUEST;
	probe->icmp.id = htons(0x7171);
	probe->icmp.sequence = htons(1);
	memcpy(probe->payload, payload, sizeof(probe->payload));
	probe->icmp.checksum = htons(internet_checksum(&probe->icmp, sizeof(probe->icmp) +
													 sizeof(probe->payload)));

	packet->size = sizeof(*probe);
	enip_port_send(&module->port, packet);
	return 0;
}
