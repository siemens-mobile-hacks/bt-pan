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
#if BT_PAN_USART_LOG
/* bt-pan_SG_TRACE_LOG: the same lines to the cable port, no file I/O */
#include "usart.h"
static inline void trace(const char *s) {
	usart_puts((char *)s);
	usart_puts("\r\n");
}
#else
static inline void trace(const char *s) {
	uint32_t err;
	int f = sys_open("0:\\bt-pan-trace.txt", A_WriteOnly + A_Create + A_Append + A_BIN, P_READ + P_WRITE, &err);
	if (f < 0)
		return;
	sys_write(f, s, (int)strlen(s), &err);
	sys_write(f, "\r\n", 2, &err);
	sys_close(f, &err);
}
#endif

/* packets through our ENIP driver, recorded from any process into RAM (no file I/O in the
 * stack's path), written to the file by the 1 s heartbeat (pan_lman_poll, SG) */
#include <stdint.h>
#include <stddef.h>
#define TRACE_PKT_N 64u
struct trace_pkt {
	char dir;            /* 'T' to the PC, 'R' from it, 'D' TX dropped */
	uint8_t proto;
	uint16_t len, sport, dport;
	uint8_t peer[4];
	uint16_t cepid;      /* process that handled it */
	uint8_t flags;       /* TCP flags */
};
extern struct trace_pkt trace_pkts[TRACE_PKT_N];
extern volatile unsigned trace_pkt_n;
void trace_pkt(char dir, const uint8_t *ip, size_t length);

/* where BTPAN (or whoever ran our code last) is: set at entry/exit of our code, written each
 * second by the UI's MMI timer, which keeps running when BTPAN stops */
extern const char *volatile trace_where;
extern volatile unsigned trace_where_arg, trace_where_cepid, trace_where_seq;
#define WHERE(s, a) (trace_where_cepid = GBS_GetCurCepid(), trace_where_arg = (unsigned)(a), \
		trace_where = (s), ++trace_where_seq)
#else
#define trace(s) ((void)0)
#define trace_pkt(d, p, n) ((void)0)
#define WHERE(s, a) ((void)0)
#endif

#endif
