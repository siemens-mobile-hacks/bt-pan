#include "ping.h"

#include <swilib.h>

#include <stdbool.h>
#include <string.h>

#include "bt_pan_netif.h"

#define PING_ID 0x7050u
#define PING_TICKS 216u /* 1 s */
#define IP_HEADER 20u
#define ICMP_HEADER 8u

/* system timer, the same on PMB8875 (S65) and PMB8876 (EL71): 26 MHz / CLC.RMC */
#ifndef PING_STM_BASE
#define PING_STM_BASE 0xF4B00000u /* tests/test_ping.c: a fake */
#endif
#define STM_CLC (*(volatile uint32_t *)(PING_STM_BASE))
#define STM_TIM0 (*(volatile uint32_t *)(PING_STM_BASE + 0x10u))

struct ping_stats ping_stats;

static struct bt_pan_netif *p_netif;
static uint32_t p_destination;
static GBSTMR p_timer;
static uint8_t p_frame[BT_PAN_FRAME_SIZE] __attribute__((aligned(4)));
static uint16_t p_seq;
static uint32_t p_sent_at;
static bool p_waiting;

static unsigned stm_us(uint32_t delta) {
	unsigned rmc = (STM_CLC >> 8) & 0xffu;

	return (unsigned)((uint64_t)delta * (rmc ? rmc : 1u) / 26u);
}

static uint16_t checksum(const uint8_t *p, size_t n) {
	uint32_t sum = 0;

	for (; n >= 2; p += 2, n -= 2)
		sum += (uint32_t)p[0] << 8 | p[1];
	if (n)
		sum += (uint32_t)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return (uint16_t)~sum;
}

static void put16(uint8_t *p, unsigned v) {
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

unsigned ping_max_size(void) {
	return bt_pan_netif_tx_max() - IP_HEADER - ICMP_HEADER;
}

static void record(unsigned us) {
	struct ping_stats *s = &ping_stats;

	s->hist[s->hist_n % PING_HIST] = us;
	++s->hist_n;
	s->last_us = us;
	if (us == PING_LOST)
		return;
	++s->received;
	s->sum_us += us;
	if (s->received == 1 || us < s->min_us)
		s->min_us = us;
	if (us > s->max_us)
		s->max_us = us;
}

static bool send_request(void) {
	uint8_t *ip = p_frame + sizeof(struct bnep_general_hdr), *icmp = ip + IP_HEADER;
	unsigned size = ping_stats.size, total = IP_HEADER + ICMP_HEADER + size, i;
	uint32_t source = p_netif->config.local_ip;

	++p_seq;
	memset(ip, 0, IP_HEADER + ICMP_HEADER);
	ip[0] = 0x45;
	put16(ip + 2, total);
	put16(ip + 4, p_seq);
	ip[8] = 64;
	ip[9] = 1; /* ICMP */
	memcpy(ip + 12, &source, 4);
	memcpy(ip + 16, &p_destination, 4);
	put16(ip + 10, checksum(ip, IP_HEADER));
	icmp[0] = 8; /* echo request */
	put16(icmp + 4, PING_ID);
	put16(icmp + 6, p_seq);
	for (i = 0; i < size; ++i)
		icmp[ICMP_HEADER + i] = (uint8_t)(i + 0x20);
	put16(icmp + 2, checksum(icmp, ICMP_HEADER + size));
	p_sent_at = STM_TIM0;
	return bt_pan_netif_send_frame(p_netif, p_frame, ip, total) != 0;
}

static void tick(GBSTMR *timer) {
	(void)timer;
	if (!ping_stats.running)
		return;
	if (!p_netif->registered || !p_netif->panu) {
		ping_stats.error = "Link down";
		ping_stats.running = 0;
		return;
	}
	if (p_waiting)
		record(PING_LOST); /* no reply within the second */
	p_waiting = send_request();
	++ping_stats.sent;
	if (!p_waiting) {
		record(PING_LOST);
		ping_stats.error = "Send failed (BT busy)";
	}
	GBS_StartTimerProc(&p_timer, PING_TICKS, tick);
}

void ping_start(struct bt_pan_netif *netif, uint32_t destination, unsigned size) {
	struct ping_stats *s = &ping_stats;

	ping_stop();
	memset((void *)s, 0, sizeof(*s));
	if (size > ping_max_size())
		size = ping_max_size();
	s->size = size;
	if (!netif->registered) {
		s->error = "Connect first";
		return;
	}
	p_netif = netif;
	p_destination = destination;
	p_waiting = false;
	s->running = 1;
	tick(NULL);
}

void ping_stop(void) {
	if (IsTimerProc(&p_timer))
		GBS_StopTimer(&p_timer);
	ping_stats.running = 0;
	p_waiting = false;
}

int ping_rx(const uint8_t *ip, size_t length) {
	unsigned ihl = (ip[0] & 0x0fu) * 4u;
	const uint8_t *icmp = ip + ihl;

	if (ip[9] != 1 || length < ihl + ICMP_HEADER || icmp[0] != 0 ||
		((unsigned)icmp[4] << 8 | icmp[5]) != PING_ID)
		return 0;
	if (ping_stats.running && p_waiting && ((unsigned)icmp[6] << 8 | icmp[7]) == p_seq &&
		!memcmp(ip + 12, &p_destination, 4)) {
		p_waiting = false;
		record(stm_us(STM_TIM0 - p_sent_at));
	}
	return 1; /* ours, also a late one */
}
