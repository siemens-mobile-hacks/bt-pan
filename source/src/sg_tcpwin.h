#pragma once

/* SGOLD (S65v58 / SK65v50, identical ENIP code) TCP receive window, D136.
 *
 * ENIP's TCP keeps the receive window in the socket as a plain counter, u16 at sock+0x10C:
 *  - sock init (S65 A134D6BE, SK65 A0A3004A): MSS sock+0x118 = route MTU - 40, window = 3 * MSS;
 *  - the route MTU is the driver MTU clamped by ENIP_RegisterDriver (S65 A12B5E4A) to the
 *    largest ENIP packet, flash constant 2209 (S65 A13354B8, SK65 A09ED6B4) minus the driver's
 *    header bytes (0 for us) -> MSS 2169, window 6507 whatever MTU bt-pan registers;
 *  - received data: window -= len (S65 A134DF64 in tcp input A134D806); the application reads:
 *    window += len (S65 A134E5B8, which also sends the window update);
 *  - the header gets the counter as it is (S65 A134C310; 0 only while below the MSS after a
 *    zero window), the input check accepts seq within [rcv_nxt, rcv_nxt + window).
 * Nothing else limits it, so raising the counter once, right after the init, gives the socket
 * a bigger window for its whole life (the stock 6507 caps a 400 ms internet RTT at ~16 KB/s).
 * The bytes behind the window are ENIP packets from the RX pools (bt_pan_rx_pools), so their
 * byte limits are raised together (bt_pan_netif.c).
 *
 * The socket is found when its SYN (or SYN-ACK) passes our driver: TX runs in ENIP's own process
 * right after the stack built the segment (sock init happened just before). Socket table: 20
 * pointers at S65 A876D5A0+0x45C / SK65 A8F16E54+0x45C (S65 A134D786 walks it); sock+0x19 = IP
 * protocol, +0x50 = local and remote port as on the wire, +0xF8 = ISS (= seq of the SYN). */

#include <stddef.h>
#include <stdint.h>

#define SG_SOCK_COUNT 20u
#define SG_SOCK_PROTO 0x19u
#define SG_SOCK_PORTS 0x50u
#define SG_SOCK_ISS 0xF8u
#define SG_SOCK_WINDOW 0x10Cu
#define SG_SOCK_MSS 0x118u

/* ip/length: an IPv4 packet our driver sends. If it is a TCP SYN of a socket in the table whose
 * window is still the stock 3 * MSS, set the window to target. Returns that socket, else NULL.
 * The caller keeps other processes out (LockSched); this only reads and writes RAM. */
static inline uint8_t *sg_tcp_window_raise(uint8_t *const *socks, const uint8_t *ip, size_t length,
										   unsigned target) {
	unsigned ihl, i, k;
	uint32_t seq;

	if (!socks || length < 40u || (ip[0] >> 4) != 4u || ip[9] != 6u)
		return NULL;
	ihl = (ip[0] & 0x0fu) * 4u;
	if (ihl < 20u || length < ihl + 20u || !(ip[ihl + 13] & 0x02u))
		return NULL;
	seq = (uint32_t)ip[ihl + 4] << 24 | (uint32_t)ip[ihl + 5] << 16 | (uint32_t)ip[ihl + 6] << 8 |
		  ip[ihl + 7];
	for (i = 0; i < SG_SOCK_COUNT; ++i) {
		uint8_t *s = socks[i];
		volatile uint16_t *window;
		unsigned mss;

		if (!s || ((uintptr_t)s & 3u) || s[SG_SOCK_PROTO] != 6u)
			continue;
		for (k = 0; k < 4u && s[SG_SOCK_PORTS + k] == ip[ihl + k]; ++k)
			;
		if (k != 4u || *(volatile uint32_t *)(s + SG_SOCK_ISS) != seq)
			continue;
		window = (volatile uint16_t *)(s + SG_SOCK_WINDOW);
		mss = *(volatile uint16_t *)(s + SG_SOCK_MSS);
		/* stock value only: a retransmitted SYN finds it raised already, a socket with data
		 * queued (window != 3 * MSS) is left alone */
		if (!mss || *window != 3u * mss || *window >= target || target > 0xffffu)
			return NULL;
		*window = (uint16_t)target;
		return s;
	}
	return NULL;
}
