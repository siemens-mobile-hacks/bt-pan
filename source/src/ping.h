#ifndef PING_H
#define PING_H

#include <stddef.h>
#include <stdint.h>

/*
 * Ping the PC: ICMP echo requests built by the elf, one per second, sent straight into the BT
 * channel (like the direct speed test) and the replies caught before ENIP. Measures the BT link
 * and the PC, not the phone's stack. Round trip from the STM timer (us, not 4.6 ms GBS ticks).
 * Runs in the BTPAN process (start/stop by message from the UI).
 */
#define PING_HIST 32u
#define PING_LOST 0xffffffffu

struct ping_stats {
	volatile int running;
	volatile unsigned size;          /* ICMP payload bytes */
	volatile unsigned sent, received;
	volatile unsigned last_us;       /* PING_LOST: the last one got no reply */
	volatile unsigned min_us, max_us;
	volatile uint32_t sum_us;
	volatile unsigned hist[PING_HIST]; /* newest at hist_n - 1 (mod PING_HIST) */
	volatile unsigned hist_n;
	const char *volatile error;
};

extern struct ping_stats ping_stats;

struct bt_pan_netif;

/* largest payload the link takes: IP MTU - 28 */
unsigned ping_max_size(void);
void ping_start(struct bt_pan_netif *netif, uint32_t destination, unsigned size);
void ping_stop(void);
/* RX path: 1 = our echo reply (not for ENIP) */
int ping_rx(const uint8_t *ip, size_t length);

#endif
