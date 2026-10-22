#include "speedtest.h"
#include "pan_lman.h"

#include <stdbool.h>
#include <string.h>

#include "bt_panu.h"
#include "bt_pan_netif.h"
#include "usart.h"

#define TICKS_PER_S 216u
#define PHASE_TICKS (SPEEDTEST_SECS * TICKS_PER_S)
#define UDP_PAYLOAD 1472u /* 20 + 8 + 1472 = 1500 = our MTU */

struct speedtest_state speedtest[2];

int speedtest_running(void) {
	return speedtest[0].state == SPT_RUN || speedtest[1].state == SPT_RUN;
}

static unsigned rate(unsigned bytes, unsigned ticks) {
	return ticks ? (unsigned)((uint64_t)bytes * TICKS_PER_S / ticks) : 0;
}

static void finish(struct speedtest_state *t, const char *error) {
	t->error = error;
	t->state = error ? SPT_ERR : SPT_DONE;
	if (!error)
		t->progress = 100;
}

static void progress(struct speedtest_state *t, unsigned ticks) {
	unsigned p = ticks * 50u / PHASE_TICKS;

	t->progress = (t->dir == SPT_UL ? 50u : 0u) + (p > 50u ? 50u : p);
}

/* ---------------------------------------------------------------- direct (BTPAN process) */

#define D_POLL 5u
enum { D_REQ, D_DL, D_GAP, D_UL, D_DRAIN };

static struct bt_pan_netif *d_netif;
static uint32_t d_server, d_local;
static GBSTMR d_timer;
static int d_phase;
static unsigned d_ticks, d_t0, d_tries;
static volatile unsigned d_bytes;
static volatile int d_started;
static unsigned d_base;
static uint16_t d_ip_id;
static uint8_t d_frame[BT_PAN_FRAME_SIZE];

static uint16_t ip_checksum(const uint8_t *p, unsigned n) {
	uint32_t sum = 0;

	for (; n > 1; p += 2, n -= 2)
		sum += (uint32_t)p[0] << 8 | p[1];
	if (n)
		sum += (uint32_t)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return (uint16_t)~sum;
}

/* IPv4 + UDP to the PC in d_frame; payload must already be in place. */
static int d_send(unsigned payload_len) {
	uint8_t *ip = BT_PAN_FRAME_PAYLOAD(d_frame), *udp = ip + 20;
	unsigned total = 28u + payload_len;
	uint16_t sum;

	ip[0] = 0x45;
	ip[1] = 0;
	ip[2] = (uint8_t)(total >> 8);
	ip[3] = (uint8_t)total;
	++d_ip_id;
	ip[4] = (uint8_t)(d_ip_id >> 8);
	ip[5] = (uint8_t)d_ip_id;
	ip[6] = 0x40; /* DF */
	ip[7] = 0;
	ip[8] = 64;
	ip[9] = 17;
	ip[10] = ip[11] = 0;
	memcpy(ip + 12, &d_local, 4);
	memcpy(ip + 16, &d_server, 4);
	sum = ip_checksum(ip, 20);
	ip[10] = (uint8_t)(sum >> 8);
	ip[11] = (uint8_t)sum;
	udp[0] = SPEEDTEST_UDP_LOCAL >> 8;
	udp[1] = SPEEDTEST_UDP_LOCAL & 0xff;
	udp[2] = SPEEDTEST_UDP_SERVER >> 8;
	udp[3] = SPEEDTEST_UDP_SERVER & 0xff;
	udp[4] = (uint8_t)((8u + payload_len) >> 8);
	udp[5] = (uint8_t)(8u + payload_len);
	udp[6] = udp[7] = 0; /* no UDP checksum (allowed for IPv4) */
	return bt_pan_netif_send_frame(d_netif, d_frame, ip, total);
}

static void d_command(const char *text) {
	unsigned n = strlen(text);

	memcpy(BT_PAN_FRAME_PAYLOAD(d_frame) + 28, text, n);
	d_send(n);
}

static void d_stop(const char *error) {
	if (IsTimerProc(&d_timer))
		GBS_StopTimer(&d_timer);
	if (d_phase == D_DL || d_phase == D_REQ)
		d_command("BTPAN STOP");
	finish(&speedtest[SPEEDTEST_DIRECT], error);
}

static void d_poll(GBSTMR *timer) {
	struct speedtest_state *t = &speedtest[SPEEDTEST_DIRECT];
	unsigned elapsed;
	int k;

	(void)timer;
	if (t->state != SPT_RUN)
		return;
	if (!d_netif->registered || !d_netif->panu) {
		d_stop("No link");
		return;
	}
	d_ticks += D_POLL;
	elapsed = d_ticks - d_t0;
	switch (d_phase) {
		case D_REQ:
			if (d_started) {
				d_phase = D_DL;
				d_t0 = d_ticks;
			} else if (elapsed >= TICKS_PER_S) {
				if (++d_tries > 3) {
					d_stop("PC not responding (speedtest-pc.py)");
					return;
				}
				d_command("BTPAN DL 8");
				d_t0 = d_ticks;
			}
			break;
		case D_DL:
			t->live_bps = rate(d_bytes, elapsed);
			progress(t, elapsed);
			if (elapsed >= PHASE_TICKS) {
				t->dl_bps = t->live_bps;
				d_command("BTPAN STOP");
				d_phase = D_GAP; /* let the rest of the flood arrive before uploading */
				d_t0 = d_ticks;
			}
			break;
		case D_GAP:
			if (elapsed >= TICKS_PER_S / 2) {
				t->dir = SPT_UL;
				t->live_bps = 0;
				d_bytes = 0;
				d_base = bt_panu_tx_free();
				memset(BT_PAN_FRAME_PAYLOAD(d_frame) + 28, 'u', UDP_PAYLOAD);
				d_phase = D_UL;
				d_t0 = d_ticks;
			}
			break;
		case D_UL:
			/* never take more than half the shared send pool: ENIP and BNEP need it too */
			for (k = 0; k < 8 && bt_panu_tx_free() > d_base / 2; ++k) {
				if (!d_send(UDP_PAYLOAD))
					break;
				d_bytes += UDP_PAYLOAD;
			}
			t->live_bps = rate(d_bytes, elapsed);
			progress(t, elapsed);
			if (elapsed >= PHASE_TICKS) {
				t->ul_bps = t->live_bps;
				d_phase = D_DRAIN;
				d_t0 = d_ticks;
			}
			break;
		case D_DRAIN:
			if (bt_panu_tx_free() + 2 >= d_base || elapsed >= 2 * TICKS_PER_S) {
				d_stop(NULL);
				return;
			}
			break;
	}
	GBS_StartTimerProc(&d_timer, D_POLL, d_poll);
}

void speedtest_direct_start(struct bt_pan_netif *netif, uint32_t server_ip) {
	struct speedtest_state *t = &speedtest[SPEEDTEST_DIRECT];

	/* the UI already marked this one running, so only the other test blocks it */
	if (speedtest[SPEEDTEST_STACK].state == SPT_RUN)
		return;
	memset(t, 0, sizeof(*t));
	if (!netif->registered || !netif->panu) {
		finish(t, "No link");
		return;
	}
	d_netif = netif;
	d_server = server_ip;
	d_local = netif->config.local_ip;
	d_ticks = d_t0 = d_tries = 0;
	d_bytes = 0;
	d_started = 0;
	d_phase = D_REQ;
	t->dir = SPT_DL;
	t->state = SPT_RUN;
	d_command("BTPAN DL 8");
	GBS_StartTimerProc(&d_timer, D_POLL, d_poll);
}

void speedtest_direct_cancel(void) {
	if (speedtest[SPEEDTEST_DIRECT].state == SPT_RUN)
		d_stop("Stopped");
}

int speedtest_direct_rx(const uint8_t *ip, size_t length) {
	unsigned ihl;

	if (length < 28 || ip[9] != 17)
		return 0;
	ihl = (ip[0] & 0x0fu) * 4u;
	if (length < ihl + 8u || ((unsigned)ip[ihl + 2] << 8 | ip[ihl + 3]) != SPEEDTEST_UDP_LOCAL)
		return 0;
	if (speedtest[SPEEDTEST_DIRECT].state == SPT_RUN &&
		(d_phase == D_REQ || d_phase == D_DL)) {
		d_started = 1;
		if (d_phase == D_DL)
			d_bytes += (unsigned)length - ihl - 8u;
	}
	return 1;
}

/* ---------------------------------------------------------------- stack (MMI + helper) */

#define S_POLL 22u
enum { S_CONNECTING, S_RUN, S_NEXT };

static GBSTMR s_timer;
static volatile int s_sock = -1;
static volatile int s_phase;
static uint32_t s_server;
static unsigned s_ticks, s_t0;
static volatile unsigned s_bytes;
static volatile int s_started, s_closed;
static int s_clock;
static uint8_t s_buf[1460];

/* helper process (SUBPROC) ------------------------------------------------ */

static void s_close(void) {
	int sock = s_sock;

	s_sock = -1;
	if (sock >= 0)
		closesocket(sock);
}

static void s_open(void) {
	struct sockaddr_in sa;
	int sock;

	pan_lman_test_begin();
	*socklasterr() = 0;
	sock = socket(1, 1, 0);
	if (sock < 0) {
		s_closed = 2; /* reported by the poll */
		return;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = 1; /* AF_INET on this stack */
	sa.sin_port = htons(SPEEDTEST_TCP_SERVER);
	memcpy(&sa.sin_addr, &s_server, 4); /* already network order */
	s_sock = sock;
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		s_close();
		s_closed = 2;
	}
}

static void s_pump(void) {
	int n;

	if (s_sock < 0 || s_phase != S_RUN || speedtest[SPEEDTEST_STACK].dir != SPT_UL)
		return;
	for (;;) {
		n = send(s_sock, s_buf, sizeof(s_buf), 0);
		if (n <= 0)
			return; /* would block: ENIP_BUFFER_FREE resumes */
		s_bytes += (unsigned)n;
		if (n < (int)sizeof(s_buf))
			return;
	}
}

static void s_begin(void) {
	static const char dl[] = "DL\n", ul[] = "UL\n";
	int up = speedtest[SPEEDTEST_STACK].dir == SPT_UL;

	if (s_sock < 0)
		return;
	send(s_sock, up ? ul : dl, 3, 0);
	if (up) {
		s_started = 1;
		s_pump();
	}
}

static void s_recv(void) {
	int n;

	while (s_sock >= 0 && (n = recv(s_sock, s_buf, sizeof(s_buf), 0)) > 0) {
		if (s_phase == S_RUN && speedtest[SPEEDTEST_STACK].dir == SPT_DL) {
			s_started = 1;
			s_bytes += (unsigned)n;
		}
	}
}

/* MMI ------------------------------------------------------------------------ */

static void s_stop(const char *error) {
	if (IsTimerProc(&s_timer))
		GBS_StopTimer(&s_timer);
	SUBPROC((void *)s_close);
	finish(&speedtest[SPEEDTEST_STACK], error);
}

static void s_poll(GBSTMR *timer) {
	struct speedtest_state *t = &speedtest[SPEEDTEST_STACK];
	unsigned elapsed;

	(void)timer;
	if (t->state != SPT_RUN)
		return;
	s_ticks += S_POLL;
	elapsed = s_ticks - s_t0;
	if (s_closed == 2) {
		s_stop("Socket failed to open");
		return;
	}
	if (s_closed == 3) {
		s_stop("PC refused: is speedtest-pc.py running?");
		return;
	}
	switch (s_phase) {
		case S_CONNECTING:
			if (elapsed >= 5 * TICKS_PER_S) {
				s_stop("PC not responding (speedtest-pc.py)");
				return;
			}
			break;
		case S_RUN:
			if (!s_clock) {
				if (!s_started) {
					if (elapsed >= 5 * TICKS_PER_S) {
						s_stop("No data");
						return;
					}
					break;
				}
				s_clock = 1; /* the clock starts with the first byte */
				s_t0 = s_ticks;
				elapsed = 0;
			}
			t->live_bps = rate(s_bytes, elapsed);
			progress(t, elapsed);
			if (elapsed >= PHASE_TICKS || s_closed) {
				if (t->dir == SPT_DL) {
					t->dl_bps = t->live_bps;
					SUBPROC((void *)s_close);
					t->dir = SPT_UL;
					t->live_bps = 0;
					s_bytes = 0;
					s_started = s_closed = s_clock = 0;
					s_phase = S_CONNECTING;
					s_t0 = s_ticks;
					SUBPROC((void *)s_open);
				} else {
					t->ul_bps = t->live_bps;
					s_stop(NULL);
					return;
				}
			}
			break;
	}
	GBS_StartTimerProc(&s_timer, S_POLL, s_poll);
}

void speedtest_stack_start(uint32_t server_ip) {
	struct speedtest_state *t = &speedtest[SPEEDTEST_STACK];

	if (speedtest_running())
		return;
	memset(t, 0, sizeof(*t));
	memset(s_buf, 's', sizeof(s_buf));
	s_server = server_ip;
	s_ticks = s_t0 = 0;
	s_bytes = 0;
	s_started = s_closed = s_clock = 0;
	s_phase = S_CONNECTING;
	t->dir = SPT_DL;
	t->state = SPT_RUN;
	SUBPROC((void *)s_open);
	GBS_StartTimerProc(&s_timer, S_POLL, s_poll);
}

void speedtest_stack_cancel(void) {
	if (speedtest[SPEEDTEST_STACK].state == SPT_RUN)
		s_stop("Stopped");
}

void speedtest_stack_msg(GBS_MSG *msg) {
	if (msg->msg != MSG_HELPER_TRANSLATOR || speedtest[SPEEDTEST_STACK].state != SPT_RUN ||
		s_sock < 0 || (int)msg->data1 != s_sock)
		return;
	switch ((int)msg->data0) {
		case ENIP_SOCK_CONNECTED:
			if (s_phase == S_CONNECTING) {
				s_phase = S_RUN;
				s_t0 = s_ticks;
				SUBPROC((void *)s_begin);
			}
			break;
		case ENIP_SOCK_DATA_READ:
			SUBPROC((void *)s_recv);
			break;
		case ENIP_BUFFER_FREE:
		case ENIP_BUFFER_FREE1:
			SUBPROC((void *)s_pump);
			break;
		case ENIP_SOCK_REMOTE_CLOSED:
		case ENIP_SOCK_CLOSED:
			s_closed = 1;
			if (s_phase == S_CONNECTING)
				s_closed = 3; /* refused (RST): nothing listens on the PC */
			break;
	}
}
