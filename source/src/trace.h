#ifndef TRACE_H
#define TRACE_H

/* BT_PAN_TRACE (bt-pan_SG_TRACE): start-up checkpoints appended to 0:\bt-pan-trace.txt, one
 * fopen/fclose per line so the file survives a crash. To find where a phone without a cable
 * log dies; the last line is the last step that finished. */
#ifndef BT_PAN_TRACE
#define BT_PAN_TRACE 0
#endif

#if BT_PAN_TRACE
#include <swilib.h>
#ifdef SGOLD
#define TRACE_FILE "0:\\bt-pan-trace.txt" /* S65/SK65: no card */
#else
#define TRACE_FILE "4:\\bt-pan-trace.txt" /* EL71: memory card */
#endif
#if BT_PAN_USART_LOG
/* bt-pan_SG_TRACE_LOG: the same lines to the cable port, no file I/O */
#include "usart.h"
static inline void trace(const char *s) {
	usart_puts((char *)s);
	usart_puts("\r\n");
}
#define trace_flush() ((void)0)
#else
/* Only MMI and BTPAN touch the file. Any other process (ENIP 40B4, LMAN 40DD, HTTP 4409: our
 * handler hooks run there) only appends to a RAM ring: a file open can block, and a blocked
 * ENIP crashes a synchronous socket() caller (assert in A12B36CC, "ebsdc", seen 2026-11-15).
 * The ring goes to the file on the next MMI/BTPAN trace() or trace_flush(). */
void trace(const char *s);
void trace_flush(void);
#endif
extern const unsigned trace_btpan_cepid; /* main.c */

/* packets through our ENIP driver, recorded from any process into RAM (no file I/O in the
 * stack's path), written to the file by the 1 s heartbeat (pan_lman_poll, SG) */
#include <stdint.h>
#include <stddef.h>
#define TRACE_PKT_N 512u
struct trace_pkt {
	char dir;            /* 'T' to the PC, 'R' from it, 'D' TX dropped */
	uint8_t proto;
	uint16_t len, sport, dport;
	uint8_t peer[4];
	uint16_t cepid;      /* process that handled it */
	uint8_t flags;       /* TCP flags */
	int8_t res;          /* R: bt_pan_netif_input result (0 ok, -2 ENIP packet alloc failed) */
	uint32_t seq, ack;   /* TCP */
	uint16_t win;        /* TCP window (unscaled) */
	uint16_t pool;       /* S65: ENIP RX pool 6 bytes in use after this packet */
	uint32_t tick;       /* STM ticks (Timer: GetSessionAge-ish, see trace_now) */
};
extern struct trace_pkt trace_pkts[TRACE_PKT_N];
extern volatile unsigned trace_pkt_n;
struct trace_pkt *trace_pkt(char dir, const uint8_t *ip, size_t length);
/* once a second from pan_lman_poll (BTPAN): HB line, counters, packet lines to TRACE_FILE */
void trace_heartbeat(void);
extern volatile uint32_t trace_rx_alloc_fails;
unsigned trace_pool_used(unsigned pool); /* S65 ENIP packet pool counters, 0 elsewhere */

/* where BTPAN (or whoever ran our code last) is: set at entry/exit of our code, written each
 * second by the UI's MMI timer, which keeps running when BTPAN stops */
extern const char *volatile trace_where;
extern volatile unsigned trace_where_arg, trace_where_cepid, trace_where_seq;
#define WHERE(s, a) (trace_where_cepid = GBS_GetCurCepid(), trace_where_arg = (unsigned)(a), \
		trace_where = (s), ++trace_where_seq)
#else
#define trace(s) ((void)0)
#define trace_flush() ((void)0)
#define trace_pkt(d, p, n) ((void)0)
#define trace_heartbeat() ((void)0)
#define trace_pkt_res(t, r) ((void)0)
#define WHERE(s, a) ((void)0)
#endif

#endif
