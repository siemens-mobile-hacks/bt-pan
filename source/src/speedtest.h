#ifndef SPEEDTEST_H
#define SPEEDTEST_H

#include <swilib.h>

#include <stddef.h>
#include <stdint.h>

/*
 * Two speed tests against speedtest-pc.py on the PC (started by nap.sh):
 *   direct - raw UDP built by the elf and sent straight into the BT channel, the PC's flood
 *            caught before ENIP: the Bluetooth link alone (UDP 5001 on the PC, 5002 here);
 *   stack  - a normal TCP socket through the phone's ENIP stack, like any app (TCP 5003).
 * Each measures download (PC -> phone) then upload, SPEEDTEST_SECS each.
 */
#define SPEEDTEST_SECS 6
#define SPEEDTEST_UDP_SERVER 5001
#define SPEEDTEST_UDP_LOCAL 5002
#define SPEEDTEST_TCP_SERVER 5003

enum { SPEEDTEST_DIRECT, SPEEDTEST_STACK };
enum { SPT_IDLE, SPT_RUN, SPT_DONE, SPT_ERR };
enum { SPT_DL, SPT_UL };

struct speedtest_state {
	volatile int state;
	volatile int dir;
	volatile unsigned progress;  /* 0..100 over both phases */
	volatile unsigned live_bps;  /* current phase, bytes/s */
	volatile unsigned dl_bps, ul_bps;
	const char *volatile error;
};

extern struct speedtest_state speedtest[2];

struct bt_pan_netif;

/* direct: BTPAN process */
void speedtest_direct_start(struct bt_pan_netif *netif, uint32_t server_ip);
void speedtest_direct_cancel(void);
int speedtest_direct_rx(const uint8_t *ip, size_t length);

/* stack: MMI (UI) process; sockets run in the elf helper (SUBPROC) */
void speedtest_stack_start(uint32_t server_ip);
void speedtest_stack_cancel(void);
void speedtest_stack_msg(GBS_MSG *msg); /* CSM onmessage: MSG_HELPER_TRANSLATOR */

int speedtest_running(void);

#endif
