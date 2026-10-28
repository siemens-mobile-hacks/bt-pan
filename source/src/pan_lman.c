/*
 * Phone applications over the PAN interface, through the native LMAN.
 *
 * An application does not just open sockets: it first asks LMAN to connect its NAP profile.
 * With a GPRS profile LMAN would activate a PDP context (no SIM -> "Network not available").
 * But FUN_a0974ac4 REUSES an existing context instead, without touching SM/GMM, when one has
 * bearer 1, the same profile name, state 4, and the request's profile has +0x1A8 != 0
 * (ghidra-el71/lman-connect-path.txt). It then answers the application "connected".
 *
 * A socket takes its interface from that context: socket() reads bearer (+1) and the interface
 * number (+4, written by attach_IF) of the owner's context, and ENIP picks the IP driver whose
 * (iface_bearer_id, iface_index) is exactly that pair (A0A65862). So we prepare such a context,
 * attach OUR interface number to it, and register our ENIP driver with bearer 1: the
 * application's sockets then go through the PAN driver with no socket interception.
 *
 * Where the NAP request is caught:
 *   - Java (service 3) msg 6245/6246 and HTTP_DOWNLOAD (4338) msg 6240: process handler hook;
 *   - WSP browser and elfs (RequestLMANConnect called directly): the flash dispatchers of the
 *     hooks VKP (patches/). They find our callbacks through a gateway that sits in
 *     the ENIP process handler (proc+0x3C) and forwards normal ENIP calls unchanged.
 * Contexts are pinned to the elf helper's socket registration, so a client's disconnect never
 * reaches SM/PDP; they are released (DISCONNECT_IND to the other owners) when PAN goes down.
 */
#include "pan_lman.h"
#include "trace.h"
#include "bt_pan_netif.h"

#include <swilib.h>

#ifdef SGOLD
/*
 * S65 sw58: an older LMAN (8 contexts of 8 bytes {u8 state, u8 bearer, u8 type, u8, u16 iface,
 * u16}, 14 socket registrations of 0x14 bytes {u16 cepid, u16 base, ...}). Here a connect does
 * not compare profile names: RequestLMANConnect type 0 (A1332258 -> A1331B3E) and LINKMANAGER
 * (40DD) msg 10 first take the context the caller's registration already refers to
 * (REFS[reg * 8 + cid] == 1, A1333296); state 4 -> LMAN_CONNECT_CNF at once, no PDP. Sockets
 * then use that context's bearer and iface (lman_get_socket_destination A13332C2), and ENIP
 * picks our driver (bearer 1, our iface). So there is one context of ours, pinned by the elf
 * helper's registration (the last ref going away frees a context), and every registration
 * without a ref gets one to it:
 *   - HTTP_DOWNLOAD (4409; browser and Java HTTP): LMAN_Connect (A0CB134C) registers and connects
 *     in one go and a new registration starts without refs, so the handler hook registers the
 *     process first (the same {0x80, FFFF, 0}); its LMAN_Connect then finds our ref;
 *   - LINKMANAGER handler hook (msg 10 from other processes) and the 1 s poll: all registrations.
 * The resolver takes DNS for bearer 1 from A876E258[iface] (0x28 each: +4 ip, +8/+C dns; 206B
 * when both are empty). Untested on the phone.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "enip.h"
#include "usart.h"

#define PAN_LMAN_CEPID 0x40DD
/* HTTP processes: every LMAN connect is their http_connect (A0CB06AC -> LMAN_Connect A0CB134C);
 * 4409 = HTTP_DOWNLOAD, 4403 = the second instance (Java, seen in the D71 trace) */
#define PAN_HTTP_N 2
static const uint16_t http_cepid[PAN_HTTP_N] = {0x4409, 0x4403};
#define PAN_HANDLER(p) (*(GBS_PROC_CALLBACK *)((char *)(p) + 0x3C))
#define PAN_PROC_CEPID(p) (*(uint16_t *)((char *)(p) + 8)) /* CreateGBSproc A0827CB8 */
#define PAN_RAM(p) ((uint32_t)(p) >= 0xA8000000u && (uint32_t)(p) < 0xAC000000u - 64u)
#define PAN_FLASH(p) ((uint32_t)(p) >= 0xA0000000u && (uint32_t)(p) < 0xA2000000u)
#define CTX ((volatile unsigned char *)RamLmanContexts())
#define REFS ((volatile uint32_t *)RamLmanRefs())
#define REG_CEPID(r) (*(volatile uint16_t *)((volatile unsigned char *)RamSocketRegs() + (r) * 0x14u))
#define NREG 14u
#define NCTX 8u
#define PDP_DRIVERS LIB_ENTRY(0x8471) /* RamPdpDrivers */
#define PDP_DRIVER_SIZE 0x28u
/* swilib declares these for ELKA only; the S65 extended library has them (btnet/signatures/S65.md) */
#define GetSocketRegIndex ((unsigned (*)(unsigned cepid))LIB_ENTRY(0x43C))
#define LMAN_AllocContext ((unsigned (*)(unsigned bearer, uint8_t type))LIB_ENTRY(0x43E))
#define LMAN_SetRef ((void (*)(unsigned reg_index, unsigned cid, int on))LIB_ENTRY(0x440))
#define LMAN_SetState ((void (*)(unsigned cid, int state))LIB_ENTRY(0x441))
#define RamLmanContexts() ((uint8_t *)LIB_ENTRY(0x8444))
#define RamLmanRefs() ((uint32_t *)LIB_ENTRY(0x8445))
#define RamSocketRegs() ((void *)LIB_ENTRY(0x8446))
#define PDP_DRIVER_COUNT 8u

static volatile bool pan_wanted, pan_ready;
static uint16_t pan_iface;
static unsigned pin = NREG;       /* helper's socket registration index */
static volatile int pan_cid = -1; /* our context */
static volatile uint32_t *pdp_dns;
static uint32_t pdp_saved[3];
static GBSTMR poll_timer;
static void *lman_proc, *http_proc[PAN_HTTP_N];
static GBS_PROC_CALLBACK lman_old, http_old[PAN_HTTP_N];
static volatile unsigned n_refs, n_http_regs; /* refs given / HTTP registrations made */
static char log_buf[96];

static void plog(void) {
	usart_puts(log_buf);
#if BT_PAN_TRACE && !BT_PAN_USART_LOG
	{
		char *e = log_buf + strlen(log_buf);
		while (e > log_buf && (e[-1] == '\n' || e[-1] == '\r'))
			*--e = 0;
		trace(log_buf);
	}
#endif
}

static void helper_init(void) {
	static REGSOCKCEPID_DATA reg = {0x80, 0xffff, 0};
	unsigned cepid = GBS_GetCurCepid();

	if (GetSocketRegIndex(cepid) >= NREG)
		RegisterCepidForSocketWork(&reg);
	pin = GetSocketRegIndex(cepid);
	sprintf(log_buf, "LMAN: helper cepid=%X registration=%u\r\n", cepid, pin);
	plog();
}

void pan_lman_init(void) {
	if (!PAN_RAM(RamSocketRegs()) || !PAN_RAM(RamLmanContexts()) || !PAN_RAM(RamLmanRefs()))
		usart_puts("LMAN: tables unavailable (library?)\r\n");
}

/* every registration without a ref to any context gets one to ours (any process) */
static void refs_give(void) {
	int cid = pan_cid;
	unsigned r, c;

	WHERE("refs give", 0);
	if (!pan_ready || cid < 0)
		return;
	for (r = 0; r < NREG; ++r) {
		if (REG_CEPID(r) == 0xffffu)
			continue;
		for (c = 0; c < NCTX; ++c)
			if (REFS[r * 8 + c] == 1)
				break;
		if (c == NCTX) {
			WHERE("LMAN_SetRef", r);
			LMAN_SetRef(r, (unsigned)cid, 1);
			++n_refs;
		}
	}
}

static void http_pre(void) {
	WHERE("http pre", GBS_GetCurCepid());
	if (pan_ready && GetSocketRegIndex(GBS_GetCurCepid()) >= NREG) {
		static REGSOCKCEPID_DATA reg = {0x80, 0xffff, 0}; /* as LMAN_Connect A0CB134C */

		RegisterCepidForSocketWork(&reg);
		++n_http_regs;
	}
	refs_give();
}

#if BT_PAN_TRACE
/* the message the handler is about to take: GBS queue head at proc+0x24 (queue proc+0xC,
 * A0828C30), frame +0A from, +0C msg (0x400 = data by pointer), +10 size, +14 data
 * (GBS_RecActDstMessage A0826964 -> A082690C). Written at once with a "done" line after the
 * handler: the last line without "done" is the message a hang is in. Own buffer per call. */
static unsigned msg_budget = 400;

static int msg_trace(const char *tag, void *proc) {
	const unsigned char *f = *(const unsigned char *const *)((char *)proc + 0x24);
	unsigned msg, from;
	uint32_t d[3];
	char buf[96];

	if (!f || !msg_budget)
		return 0;
	msg = *(const uint16_t *)(f + 0xC);
	from = *(const uint16_t *)(f + 0xA);
	if ((from == GBS_GetCurCepid() && (msg & 0x3ffu) == 0x45u) || (msg & 0x3ffu) == 0x31u)
		return 0; /* 4403's own poll / Java's DNS status poll, many per second */
	--msg_budget;
	memcpy(d, f + 0x14, sizeof(d));
	sprintf(buf, "%s msg %X from %X size %u: %08lX %08lX %08lX", tag, msg, from,
			*(const uint16_t *)(f + 0x10), (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2]);
	trace(buf);
	return 1;
}

static void msg_done(int traced, const char *tag) {
	char buf[24];

	if (!traced)
		return;
	sprintf(buf, "%s done", tag);
	trace(buf);
}
#else
#define msg_trace(t, p) 0
#define msg_done(x, t) ((void)(x))
#endif

static void http_hook0(void) {
	int t = msg_trace("H4409", http_proc[0]);

	http_pre();
	http_old[0]();
	msg_done(t, "H4409");
}

static void http_hook1(void) {
	int t = msg_trace("H4403", http_proc[1]);

	http_pre();
	http_old[1]();
	msg_done(t, "H4403");
}

static const GBS_PROC_CALLBACK http_hooks[PAN_HTTP_N] = {http_hook0, http_hook1};

static int http_hooked(void) {
	unsigned i;
	int n = 0;

	for (i = 0; i < PAN_HTTP_N; ++i)
		n += http_proc[i] != NULL;
	return n;
}

/*
 * A client that connects with LINKMANAGER msg A (the WAP browser 4402, D89 trace) has no socket
 * registration yet: msg A registers it and, finding no ref, allocates a new GPRS context
 * (st 3, no SIM -> D3, "no network"). Register the sender first, as RegisterCepidForSocketWork
 * (A1331828) does for the current process: slot = A1333516(cepid) (find or take a free one),
 * A13337E6(slot, {0x80, FFFF, 0}, 1). refs_give then points it at our context, which msg A
 * reuses (A1331B3E -> A1333296). Frame at proc+0x24: +0A from, +0C msg.
 */
#define LMAN_REG_SLOT ((unsigned (*)(unsigned))LIB_ENTRY(0x4DF)) /* LMAN_GetRegSlot */
#define LMAN_REG_FILL ((int (*)(unsigned, const REGSOCKCEPID_DATA *, int))LIB_ENTRY(0x4E0)) /* LMAN_FillReg */

static void lman_pre(void) {
	const unsigned char *f = *(const unsigned char *const *)((char *)lman_proc + 0x24);
	static const REGSOCKCEPID_DATA reg = {0x80, 0xffff, 0};
	unsigned from, r, slot;

	if (!pan_ready || !f || (*(const uint16_t *)(f + 0xC) & 0x3ffu) != 0xAu)
		return;
	from = *(const uint16_t *)(f + 0xA);
	if (from == 0xffffu || GetSocketRegIndex(from) < NREG)
		return;
	for (r = 0; r < NREG && REG_CEPID(r) != 0xffffu; ++r)
		;
	if (r == NREG)
		return; /* full: A1333516 would assert */
	slot = LMAN_REG_SLOT(from);
	if (slot < NREG) {
		LMAN_REG_FILL(slot, &reg, 1);
		++n_http_regs;
	}
}

static void lman_hook(void) {
	int t = msg_trace("L", lman_proc);

	lman_pre();
	refs_give();
	lman_old();
	msg_done(t, "L");
}

/*
 * ARM gateway in the LMAN handler slot, also the record that the browser patch
 * (patches/bt-pan-browser_S65v58.vkp) looks for: word0 "LDR pc,[pc,#0]" forwards every LMAN
 * call to word2 = lman_hook; word1 BTN1, word4 BTS1; word3 = 1 while our context is in state 4.
 * Then the WAP browser's bearer check (A0B715CC) takes the cell registration as done.
 */
static uint32_t pan_gateway[5] __attribute__((aligned(4))) = {
	0xE59FF000u, 0x314E5442u, 0, 0, 0x31535442u,
};
#define PAN_GATEWAY ((GBS_PROC_CALLBACK)(void *)pan_gateway)
#define PAN_GATEWAY_READY pan_gateway[3]

/* ENIP (sockets, DNS): trace build only, no behaviour change */
#define PAN_ENIP_CEPID 0x40B4
static void *enip_proc;
static GBS_PROC_CALLBACK enip_old;

#if BT_PAN_TRACE
/* msg 0x30 = gethostbyname (ENIP dispatcher A1319C3C case -> A13380FA -> resolver A12B57F6):
 * data1 -> {flag, name, result*, id, dest*}; dest +6 bearer type (0/1), +8 iface. The resolver
 * returns early when the DNS module is off (*A876D400 == 0), the bearer type is not 0/1, the
 * table has no DNS, no slot is free or the table's local IP is 0. */
static const uint32_t *gh_req;
static unsigned gh_msg;

static void gh_trace(void) {
	const unsigned char *f = *(const unsigned char *const *)((char *)enip_proc + 0x24);
	const uint32_t *r;
	const unsigned char *dst;
	char buf[128], name[41];
	unsigned i;

	gh_req = NULL;
	if (!f)
		return;
	gh_msg = *(const uint16_t *)(f + 0xC) & 0x3ffu;
	if (gh_msg != 0x30u && gh_msg != 0x31u)
		return;
	memcpy(&r, f + 0x18, sizeof(r)); /* data0 */
	if (!PAN_RAM(r))
		return;
	gh_req = r;
	if (gh_msg == 0x31u)
		return;
	name[0] = 0;
	if (PAN_RAM(r[1])) {
		for (i = 0; i < 40 && ((const char *)r[1])[i]; ++i)
			name[i] = ((const char *)r[1])[i];
		name[i] = 0;
	}
	dst = PAN_RAM(r[4]) ? (const unsigned char *)r[4] : NULL;
	sprintf(buf, "GH name='%s' flag=%lX res=%08lX id=%lX dnsmod=%lX", name, (unsigned long)r[0],
			(unsigned long)r[2], (unsigned long)r[3], (unsigned long)*(volatile uint32_t *)0xA876D400u);
	trace(buf);
	if (dst) {
		sprintf(buf, "GH dest %02X%02X %02X%02X %02X%02X bearer=%u %02X iface=%u", dst[0], dst[1], dst[2],
				dst[3], dst[4], dst[5], dst[6], dst[7], dst[8] | dst[9] << 8);
		trace(buf);
	}
}

/* after the handler: the dispatcher stores the resolver's return at req+0x14 (u16), 1 at +0x18
 * (A131A07E). 0x31 = status poll (A131A01C -> A12B4960(req[0]) -> req[1]); logged on change.
 * DNS module state A876D400: 4 query slots of 0x34 at +0x80 (+0x24 open, +0x28 socket,
 * +0x2C local ip, +0x30 port, +0x34/+0x38 dns, A12B4B5E/A12B577E). */
static unsigned gh_dumps;
static uint32_t gh_poll_last = 0xdeadbeefu;

static void gh_after(void) {
	const uint32_t *r = gh_req;
	char buf[112];
	unsigned i;

	if (!r)
		return;
	if (gh_msg == 0x30u) {
		sprintf(buf, "GH ret=%X done=%lX res=%08lX *res=%08lX", (unsigned)*(const uint16_t *)((const char *)r + 0x14),
				(unsigned long)r[6], (unsigned long)r[2],
				PAN_RAM(r[2]) ? (unsigned long)*(const uint32_t *)r[2] : 0ul);
		trace(buf);
	} else if (r[1] != gh_poll_last) {
		gh_poll_last = r[1];
		sprintf(buf, "GH poll id=%08lX -> %08lX", (unsigned long)r[0], (unsigned long)r[1]);
		trace(buf);
	} else
		return;
	if (gh_dumps >= 6)
		return;
	++gh_dumps;
	for (i = 0; i < 4; ++i) {
		const volatile uint32_t *q = (const volatile uint32_t *)(0xA876D400u + 0x80u + i * 0x34u);
		const volatile unsigned char *qb = (const volatile unsigned char *)q;

		sprintf(buf, "DNS slot%u open=%u sock=%08lX ip=%08lX port=%u dns=%08lX %08lX", i, qb[0x24],
				(unsigned long)q[0x28 / 4], (unsigned long)q[0x2C / 4], *(const volatile uint16_t *)(qb + 0x30),
				(unsigned long)q[0x34 / 4], (unsigned long)q[0x38 / 4]);
		trace(buf);
	}
}
#endif

static void enip_hook(void) {
	int t = msg_trace("E", enip_proc);

#if BT_PAN_TRACE
	gh_trace();
#endif
	enip_old();
#if BT_PAN_TRACE
	gh_after();
#endif
	msg_done(t, "E");
}

static void *proc_of(unsigned cepid) {
	void *p = GetGBSProcAddress(cepid);

	return p && PAN_PROC_CEPID(p) == cepid && PAN_FLASH(PAN_HANDLER(p)) ? p : NULL;
}

static void hooks_install(void) {
	void *p;
	unsigned i;

	LockSched();
	if (!lman_proc && (p = proc_of(PAN_LMAN_CEPID))) {
		pan_gateway[2] = (uint32_t)lman_hook;
		lman_proc = p;
		lman_old = PAN_HANDLER(p);
		PAN_HANDLER(p) = PAN_GATEWAY;
	}
	if (BT_PAN_TRACE && !enip_proc && (p = proc_of(PAN_ENIP_CEPID))) {
		enip_proc = p;
		enip_old = PAN_HANDLER(p);
		PAN_HANDLER(p) = enip_hook;
	}
	for (i = 0; i < PAN_HTTP_N; ++i)
		if (!http_proc[i] && (p = proc_of(http_cepid[i]))) { /* created on first use */
			http_proc[i] = p;
			http_old[i] = PAN_HANDLER(p);
			PAN_HANDLER(p) = http_hooks[i];
		}
	UnlockSched();
}

static void hooks_restore(void) {
	unsigned i;

	LockSched();
	PAN_GATEWAY_READY = 0;
	if (lman_proc && GetGBSProcAddress(PAN_LMAN_CEPID) == lman_proc && PAN_HANDLER(lman_proc) == PAN_GATEWAY)
		PAN_HANDLER(lman_proc) = lman_old;
	for (i = 0; i < PAN_HTTP_N; ++i) {
		if (http_proc[i] && GetGBSProcAddress(http_cepid[i]) == http_proc[i] &&
			PAN_HANDLER(http_proc[i]) == http_hooks[i])
			PAN_HANDLER(http_proc[i]) = http_old[i];
		http_proc[i] = NULL;
	}
	if (enip_proc && GetGBSProcAddress(PAN_ENIP_CEPID) == enip_proc && PAN_HANDLER(enip_proc) == enip_hook)
		PAN_HANDLER(enip_proc) = enip_old;
	enip_proc = lman_proc = NULL;
	UnlockSched();
}

/* helper process (registration of its own cepid): our context, then serve */
static void sg_up(void) {
	unsigned cid, i, free_slots = 0;

	if (!pan_wanted || pan_cid >= 0)
		return;
	helper_init();
	if (pin >= NREG) {
		usart_puts("LMAN: no helper registration, apps stay on GPRS\r\n");
		return;
	}
	for (i = 0; i < NCTX; ++i)
		if (CTX[i * 8 + 1] == 0xff)
			++free_slots;
	if (!free_slots) {
		usart_puts("LMAN: context table full\r\n"); /* the allocator asserts */
		return;
	}
	cid = LMAN_AllocContext(1, 1);
	if (cid >= NCTX)
		return;
	/* our interface before the state change: SetState(4) notifies, sockets may follow */
	lman_connection_attach_IF((uint16_t)cid, pan_iface, 0xffffu);
	LMAN_SetRef(pin, cid, 1);
	LMAN_SetState(cid, 4);
	pan_cid = (int)cid;
	hooks_install();
	pan_ready = true;
	PAN_GATEWAY_READY = lman_proc != NULL;
	refs_give();
	if (pdp_dns) {
		sprintf(log_buf, "LMAN: dns table iface %u: ip %08lX dns %08lX %08lX\r\n", (unsigned)pan_iface,
				(unsigned long)pdp_dns[1], (unsigned long)pdp_dns[2], (unsigned long)pdp_dns[3]);
		plog();
	}
	sprintf(log_buf, "LMAN: cid=%u bearer 1 iface %u (lman=%d http=%d enip=%d)\r\n", cid,
			(unsigned)pan_iface, lman_proc != NULL, http_hooked(), enip_proc != NULL);
	plog();
}

void pan_lman_up(uint8_t bearer, uint16_t iface, uint32_t local_ip, uint32_t dns1, uint32_t dns2) {
	pan_iface = iface;
	if (bearer == 1 && iface < PDP_DRIVER_COUNT && !pdp_dns) {
		volatile uint32_t *d = (volatile uint32_t *)(PDP_DRIVERS + iface * PDP_DRIVER_SIZE);

		LockSched();
		pdp_saved[0] = d[1];
		pdp_saved[1] = d[2];
		pdp_saved[2] = d[3];
		d[1] = local_ip;
		d[2] = dns1;
		d[3] = dns2 ? dns2 : dns1;
		pdp_dns = d;
		UnlockSched();
	}
	pan_wanted = true;
	SUBPROC((void *)sg_up);
	GBS_StartTimerProc(&poll_timer, 216, (void (*)(GBSTMR *))pan_lman_poll);
}

void pan_lman_down(void) {
	int cid = pan_cid;

	pan_wanted = false;
	pan_ready = false;
	if (IsTimerProc(&poll_timer))
		GBS_StopTimer(&poll_timer);
	hooks_restore();
	if (cid >= 0) {
		/* the firmware's own teardown: drops every ref (and the sockets of a registration
		 * left without one), frees the context; bearer 1 only reads GPRS counters there */
		LMAN_SetState((unsigned)cid, 0);
		pan_cid = -1;
		usart_puts("LMAN: context released\r\n");
	}
	if (pdp_dns) {
		LockSched();
		pdp_dns[1] = pdp_saved[0];
		pdp_dns[2] = pdp_saved[1];
		pdp_dns[3] = pdp_saved[2];
		pdp_dns = NULL;
		UnlockSched();
	}
}

void pan_lman_poll(void) {
	WHERE("lman poll", 0);
	static unsigned seen_refs, seen_regs, seen_sig = 0xffffffffu;
	unsigned r, c, sig = 0;

	if (!pan_wanted)
		return;
	GBS_StartTimerProc(&poll_timer, 216, (void (*)(GBSTMR *))pan_lman_poll);
#if BT_PAN_TRACE
	{ /* heartbeat: this timer runs in BTPAN; its absence = BTPAN stuck */
		static unsigned beat, done;
		unsigned n = trace_pkt_n;

		if (n - done > TRACE_PKT_N)
			done = n - TRACE_PKT_N; /* overwritten */
		if (beat < 600) {
			sprintf(log_buf, "HB %u cepid=%X rx=%lu tx=%lu pkts=%u", beat, (unsigned)GBS_GetCurCepid(),
					(unsigned long)bt_pan_rx_bytes, (unsigned long)bt_pan_tx_bytes, n);
			trace(log_buf);
		}
		++beat;
		for (; done != n; ++done) {
			const struct trace_pkt *t = &trace_pkts[done % TRACE_PKT_N];

			if (done >= 400)
				continue;
			sprintf(log_buf, "%c %u.%u.%u.%u p%u len %u %u->%u f%02X in %X", t->dir, t->peer[0],
					t->peer[1], t->peer[2], t->peer[3], t->proto, t->len, t->sport, t->dport, t->flags,
					t->cepid);
			trace(log_buf);
		}
	}
#endif
	if (!pan_ready)
		return;
	if (!lman_proc || http_hooked() < PAN_HTTP_N)
		hooks_install();
	PAN_GATEWAY_READY = lman_proc && CTX[pan_cid * 8] == 4;
	refs_give();
	for (r = 0; r < NREG; ++r)
		sig = sig * 31u + REG_CEPID(r) + (REFS[r * 8 + (unsigned)pan_cid] ? 7u : 0u);
	sig = sig * 31u + CTX[pan_cid * 8] + (lman_proc != NULL) * 2u + (unsigned)http_hooked() * 4u;
	if (sig == seen_sig && seen_refs == n_refs && seen_regs == n_http_regs)
		return;
	seen_sig = sig;
	seen_refs = n_refs;
	seen_regs = n_http_regs;
	sprintf(log_buf, "LMAN: cid=%d st=%u refs given %u, http regs %u (lman=%d http=%d)\r\n",
			pan_cid, CTX[pan_cid * 8], n_refs, n_http_regs, lman_proc != NULL, http_hooked());
	plog();
	for (c = 0; c < NCTX; ++c) {
		volatile unsigned char *x = &CTX[c * 8];
		if (x[1] == 0xff)
			continue;
		sprintf(log_buf, "  ctx%u st=%u bearer=%u type=%u %02X iface=%u %04X\r\n", c, x[0], x[1],
				x[2], x[3], *(volatile uint16_t *)(x + 4), *(volatile uint16_t *)(x + 6));
		plog();
	}
	for (r = 0; r < NREG; ++r) {
		unsigned mask = 0;

		if (REG_CEPID(r) == 0xffffu)
			continue;
		for (c = 0; c < NCTX; ++c)
			if (REFS[r * 8 + c])
				mask |= 1u << c;
		sprintf(log_buf, "  reg%u cepid=%X refs=%02X%s\r\n", r, REG_CEPID(r), mask,
				r == pin ? " (ours)" : "");
		plog();
	}
}

void pan_lman_test_begin(void) {}

int hooks_patch_state(void) {
	return PATCH_OK; /* nothing in flash needed */
}
#else

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "enip.h"
#include "usart.h"

#define PAN_ENIP_CEPID 0x40B4
#define PAN_HTTP_CEPID 0x4338
#define PAN_HANDLER(p) (*(GBS_PROC_CALLBACK *)((char *)(p) + 0x3C))
#define PAN_RAM(p) ((uint32_t)(p) >= 0xA8000000u && (uint32_t)(p) < 0xAE000000u - 64u)
#define PAN_FLASH(p) ((uint32_t)(p) >= 0xA0000000u && (uint32_t)(p) < 0xA2000000u)
#define CTX ((volatile unsigned char *)RamLmanContexts())
#define REFS ((volatile uint32_t *)RamLmanRefs())
#define NREG 14u

static volatile bool pan_ready;   /* interface up: serve NAP requests */
static uint8_t pan_bearer;
static uint16_t pan_iface;
static unsigned pin = NREG;       /* helper's socket registration index */
static unsigned owned;            /* bit per context we created */
static int self_cid = -1;         /* the helper's own context (stack speed test), or -1 */
static void self_release(void);
static char log_buf[128];
/*
 * The resolver (FUN_a0978a34) takes DNS servers for a bearer-1 owner not from our driver but
 * from the GPRS PDP driver table A8E9E9CC[iface] (ENIP_IP_DRIVER, 0x2C each): +4 local ip,
 * +8 dns1, +C dns2; both DNS empty -> error 206B. Filled while PAN is up, restored after.
 */
#define PDP_DRIVERS LIB_ENTRY(0x8471) /* RamPdpDrivers */
#define PDP_DRIVER_COUNT 8u
static volatile uint32_t *pdp_dns;
static uint32_t pdp_saved[3];
static GBSTMR poll_timer;

static void *java_proc, *http_proc, *enip_proc;
static GBS_PROC_CALLBACK java_old, http_old, enip_old;

/* what happened, logged from pan_lman_poll (never from the hooks: they run in other processes) */
static volatile unsigned n_select, n_prepared, last_cid = 0xffff, last_type, last_owner;
/* why the last request got no context: 0 ok, 1 not GPRS, 2 no +30 flag, 3 bad name,
 * 4 same name but not our UP context, 5 table full, 6 alloc failed, 7 reused ours */
static volatile unsigned last_why;
static char last_name[26];
static volatile unsigned n_wsp_online, n_wsp_bearer, wsp_bearer_result;

/* ENIP requests from applications (socket 1F, connect 24, DNS 30), logged from the poll */
struct enip_note {
	uint16_t msg, from, cepid, bearer, iface, error;
};
#define NOTES 16u
static struct enip_note notes[NOTES];
static volatile unsigned notes_head, notes_tail, notes_lost;

static void plog(void) {
	usart_puts(log_buf);
}

static void helper_init(void) {
	static REGSOCKCEPID_DATA reg = {0x80, 0xffff, 0};
	unsigned cepid = GBS_GetCurCepid();

	trace("H helper start");
	unsigned before = GetSocketRegIndex(cepid);
	int result = before < NREG ? 2 : RegisterCepidForSocketWork(&reg);

	pin = GetSocketRegIndex(cepid);
	sprintf(log_buf, "LMAN: helper cepid=%X registration=%u result=%d\r\n", cepid, pin, result);
	plog();
	trace("H helper done");
}

void pan_lman_init(void) {
	if (!PAN_RAM(RamSocketRegs()) || !PAN_RAM(RamLmanContexts()) || !PAN_RAM(RamLmanRefs())) {
		usart_puts("LMAN: tables unavailable (library?)\r\n");
		return;
	}
	SUBPROC((void *)helper_init);
}

/* A NAP container was selected: make LMAN reuse a context on our interface. -> cid or -1 */
static int pan_prepare(uint32_t container) {
	unsigned char *p;
	unsigned n, cid, i;
	_Alignas(uint32_t) CTX_NAPDEF profile;
	uint32_t shareable = 1;

	if (!pan_ready || pin >= NREG || !PAN_RAM(container))
		return -1;
	p = (unsigned char *)(uintptr_t)*(uint32_t *)(uintptr_t)(container + 4);
	if (!PAN_RAM(p))
		return -1;
	++n_select;
	last_cid = 0xffff;
	last_owner = GBS_GetCurCepid();
	last_type = p[0x34] ? p[0x34] : 0x100u + p[0xa0];
	for (n = 0; n < 25 && p[n]; ++n)
		last_name[n] = (char)p[n];
	last_name[n < 25 ? n : 25] = 0;
	/* Same GPRS branch as RequestLMANConnect. Never convert CSD. */
	if (!(p[0x34] == 3 || (!p[0x34] && p[0xa0] == 5))) {
		last_why = 1;
		return -1;
	}
	if (!p[0x30]) {
		last_why = 2;
		return -1;
	}
	if (!n || n == 25) {
		last_why = 3;
		return -1;
	}
	for (i = 0; i < 8; ++i) {
		if (CTX[i * 0x70 + 1] != 0xff && !strcmp((const char *)(CTX + i * 0x70 + 8), (char *)p)) {
			/* never hand out somebody else's or a non-UP context as ours */
			if ((owned & (1u << i)) && CTX[i * 0x70] == 4 && CTX[i * 0x70 + 1] == 1) {
				last_why = 7;
				last_cid = i;
				return (int)i;
			}
			last_why = 4;
			last_cid = i | 0x100u;
			return -1;
		}
	}
	/* Apps must get the lowest free cid: with the browser on cid 2 (cid 1 held by our
	 * context) RAP_BEARER corrupted memory and died (data abort in async_gethostbyname).
	 * Ours normally sits on cid >= 2 (pan_self_context); give way if it ever got cid 1. */
	if (self_cid >= 0 && self_cid < 2)
		self_release();
	for (i = 0; i < 8; ++i)
		if (CTX[i * 0x70 + 1] == 0xff)
			break;
	if (i == 8) {
		last_why = 5; /* the allocator asserts on a full table */
		return -1;
	}
	cid = LMAN_AllocContext(1, 1);
	if (cid >= 8) {
		last_why = 6;
		return -1;
	}
	last_why = 0;
	/* only the bounded profile name, not GPRS addresses/credentials */
	memset(&profile, 0, sizeof(profile));
	memcpy(profile.body, p, n);
	memcpy(profile.body + 0x1a8, &shareable, sizeof(shareable));
	LMAN_SetProfile(cid, &profile);
	/* our interface before the state change: SetState notifies, sockets may follow at once */
	lman_connection_attach_IF((uint16_t)cid, pan_iface, 0xffffu);
	LMAN_SetRef(pin, cid, 1);
	LMAN_SetState(cid, 4);
	owned |= 1u << cid;
	last_cid = cid;
	++n_prepared;
	return (int)cid;
}

/* Context for ElfLoader's helper process (0x440A): sockets of every elf (BalletMini, our stack
 * speed test, ...) take the context their cepid holds a ref on. Kept off cid 1, which the
 * browser must get: take the two lowest free slots and free the lower one. Name matches no
 * NAP profile, so apps never reuse it. */
static void pan_self_context(void) {
	_Alignas(uint32_t) CTX_NAPDEF profile;
	static const char name[] = "BT PAN";
	unsigned cid, i;

	unsigned spare = 0xff, free_slots = 0;

	for (i = 0; i < 8; ++i)
		if (CTX[i * 0x70 + 1] == 0xff)
			++free_slots;
	if (free_slots < 2)
		return; /* the allocator asserts on a full table; leave one for apps */
	cid = LMAN_AllocContext(1, 1);
	if (cid >= 8)
		return;
	if (cid < 2) {
		spare = cid;
		cid = LMAN_AllocContext(1, 1);
		LMAN_FreeContext(spare);
		if (cid >= 8)
			return;
	}
	memset(&profile, 0, sizeof(profile));
	memcpy(profile.body, name, sizeof(name));
	LMAN_SetProfile(cid, &profile);
	lman_connection_attach_IF((uint16_t)cid, pan_iface, 0xffffu);
	LMAN_SetRef(pin, cid, 1);
	LMAN_SetState(cid, 4);
	owned |= 1u << cid;
	self_cid = (int)cid;
	sprintf(log_buf, "LMAN: test context cid=%u\r\n", cid);
	plog();
}

static void self_release(void) {
	unsigned cid = (unsigned)self_cid, r;

	if (self_cid < 0)
		return;
	LockSched();
	for (r = 0; r < NREG; ++r)
		REFS[r * 8 + cid] = 0;
	LMAN_FreeContext(cid);
	owned &= ~(1u << cid);
	self_cid = -1;
	UnlockSched();
	sprintf(log_buf, "LMAN: test context cid=%u released\r\n", cid);
	plog();
}

/* helper: before the speed test's socket(); the context normally exists since up */
void pan_lman_test_begin(void) {
	if (pan_ready && pin < NREG && self_cid < 0)
		pan_self_context();
}

/* first frame of a process queue that is a real message (A009121C walk) */
static uint32_t *pan_frame(void *proc) {
	uint32_t *f = (uint32_t *)(uintptr_t)*(uint32_t *)((char *)proc + 0x24);

	while (f) {
		unsigned flags = f[2];
		if (!(flags & 0x80) && (flags & 0x7f) != 0x42 && (flags & 0x7f) != 3)
			break;
		f = (uint32_t *)(uintptr_t)f[0];
	}
	return f;
}

static uint32_t *pan_payload(uint32_t *f) {
	uint32_t *payload = f + 6;

	/* short type 41 refers to an external long frame */
	if (*(uint16_t *)((char *)f + 0x14) < 13 && (f[2] & 0x7f) == 0x41)
		payload = PAN_RAM(f[6]) ? (uint32_t *)(uintptr_t)(f[6] + 0x18) : NULL;
	return payload;
}

/* Java: 6245 (NAP selected) / 6246; its handler sets the profile's +1A8 itself. */
static void java_hook(void) {
	uint32_t *f = pan_frame(java_proc);

	if (f) {
		unsigned msg = *(uint16_t *)((char *)f + 0x10);
		uint32_t *payload = pan_payload(f);

		if (payload && msg == 0x6245 && PAN_RAM(payload[0])) {
			unsigned char *event = (unsigned char *)(uintptr_t)payload[0];
			if (!event[0x14])
				pan_prepare(*(uint32_t *)(event + 0x18));
		} else if (payload && msg == 0x6246 && PAN_RAM(payload[1])) {
			unsigned char *ctx = (unsigned char *)(uintptr_t)payload[1];
			pan_prepare(*(uint32_t *)(ctx + 0x90));
		}
	}
	java_old();
}

/* HTTP_DOWNLOAD: 6240 (NAP selector created) connects inside the original handler, so the
 * context must be ready before it runs. Mark the transient NAP copy shareable (+1A8). */
static void http_hook(void) {
	uint32_t *f = pan_frame(http_proc);

	if (f && *(uint16_t *)((char *)f + 0x10) == 0x6240) {
		uint32_t *payload = pan_payload(f);

		if (payload && PAN_RAM(payload[0])) {
			unsigned char *event = (unsigned char *)(uintptr_t)payload[0];
			uint32_t container = *(uint32_t *)(event + 0x18);

			if (!event[0x14] && PAN_RAM(container)) {
				unsigned char *p = (unsigned char *)(uintptr_t)*(uint32_t *)(uintptr_t)(container + 4);
				if (PAN_RAM(p) && pan_prepare(container) >= 0)
					*(uint32_t *)(p + 0x1a8) = 1;
			}
		}
	}
	http_old();
}

/* LMAN flash dispatcher: RequestLMANConnect entry, in the caller's process (WSP, elfs). */
static void before_connect(void *request) {
	unsigned char *r = request;
	uint32_t container;
	unsigned char *p;

	if (!PAN_RAM(request) || !pan_ready)
		return;
	if (r[0] || r[9] != 4)
		return; /* type 0, NAP-container variant */
	container = *(uint32_t *)(r + 0x0c);
	if (!PAN_RAM(container))
		return;
	p = (unsigned char *)(uintptr_t)*(uint32_t *)(uintptr_t)(container + 4);
	if (!PAN_RAM(p))
		return;
	if (pan_prepare(container) >= 0)
		*(uint32_t *)(p + 0x1a8) = 1;
}

/* WSP online dispatcher: instead of the cellular offline policy while PAN is up. */
static int wsp_online(void) {
	++n_wsp_online;
	return pan_ready && pin < NREG;
}

/* WSP availability dispatcher: advertise configured GPRS NAPs as available while PAN is up. */
static int wsp_bearer(int bearer, int nap) {
	const unsigned char *prefs;
	char key[48], type[12];
	unsigned length = sizeof(type) - 1;
	unsigned native;

	++n_wsp_bearer;
	wsp_bearer_result = 0xffff;
	if (!pan_ready || pin >= NREG || bearer < 0 || nap < 0 || nap >= 20)
		return -1;
	prefs = WSP_GetObject(0x17);
	if (!PAN_RAM(prefs) || bearer != *(const int16_t *)(prefs + 0x30))
		return -1;
	memset(type, 0, sizeof(type));
	snprintf(key, sizeof(key), "browser:napdef..%d.bearer..0", nap);
	if (!WSP_ReadProperty(key, type, &length))
		return -1;
	type[sizeof(type) - 1] = 0;
	if (strcmp(type, "GSM-GPRS"))
		return -1;
	native = WSP_GetConnectionState(bearer, nap);
	/* the native enum: connectable until LMAN's CNF makes the WSP state 2 */
	wsp_bearer_result = native == 2 ? 3 : 2;
	return (int)wsp_bearer_result;
}

/* ENIP process handler while PAN is up: note the owner record of app requests, then forward. */
static void enip_hook(void) {
	uint32_t *f = enip_proc ? pan_frame(enip_proc) : NULL;
	uint32_t *r = NULL;
	unsigned msg = 0;

	if (f && *(uint16_t *)((char *)f + 0x14) < 13 && PAN_RAM(f[7])) {
		msg = *(uint16_t *)((char *)f + 0x10);
		r = (uint32_t *)(uintptr_t)f[7];
	}
	if (r && (msg == 0x1f || msg == 0x30 || msg == 0x24)) {
		struct enip_note n = {(uint16_t)msg, *(uint16_t *)((char *)f + 0x0e), 0, 0xffff, 0xffff, 0};
		/* owner record {u16 cepid, u16 base, u16 selector, u8 bearer, u16 iface} (A097667E) */
		if (msg != 0x24 && PAN_RAM(r[4])) {
			uint16_t *owner = (uint16_t *)(uintptr_t)r[4];
			n.cepid = owner[0];
			n.bearer = *(uint8_t *)&owner[3];
			n.iface = owner[4];
		}
		enip_old();
		n.error = *(uint16_t *)((char *)r + 0x14);
		if (notes_head - notes_tail < NOTES)
			notes[notes_head++ % NOTES] = n;
		else
			++notes_lost;
		return;
	}
	enip_old();
}

/*
 * Executable ARM gateway in the ENIP process handler slot, also the discovery record of the
 * flash dispatchers of the hooks VKP (fixed layout: BTN1/BTN2/BTN3 magics).
 * Word0 ARM "LDR pc,[pc,#0]" forwards every normal ENIP call to word2 = the original handler.
 * Words 3/4/6 are called only by the dispatchers after checking opcode/magic/RAM bounds.
 */
static uint32_t pan_gateway[8] __attribute__((aligned(4))) = {
	0xE59FF000u, 0x314E5442u, 0, 0, 0, 0x324E5442u, 0, 0x334E5442u,
};

static void hooks_install(void) {
	void *p;
	unsigned cepid;

	LockSched();
	if (!enip_proc) {
		p = GetGBSProcAddress(PAN_ENIP_CEPID);
		if (p && *(uint16_t *)((char *)p + 12) == PAN_ENIP_CEPID && PAN_FLASH(PAN_HANDLER(p))) {
			pan_gateway[2] = (uint32_t)enip_hook;
			pan_gateway[3] = (uint32_t)before_connect;
			pan_gateway[4] = (uint32_t)wsp_online;
			pan_gateway[6] = (uint32_t)wsp_bearer;
			enip_proc = p;
			enip_old = PAN_HANDLER(p);
			PAN_HANDLER(p) = (GBS_PROC_CALLBACK)(void *)pan_gateway;
		}
	}
	if (!java_proc) {
		cepid = GetServiceCepid(3) & 0xffffu;
		p = cepid ? GetGBSProcAddress(cepid) : NULL;
		if (p && *(uint16_t *)((char *)p + 12) == cepid && PAN_FLASH(PAN_HANDLER(p))) {
			java_proc = p;
			java_old = PAN_HANDLER(p);
			PAN_HANDLER(p) = java_hook;
		}
	}
	if (!http_proc) {
		p = GetGBSProcAddress(PAN_HTTP_CEPID);
		if (p && *(uint16_t *)((char *)p + 12) == PAN_HTTP_CEPID && PAN_FLASH(PAN_HANDLER(p))) {
			http_proc = p;
			http_old = PAN_HANDLER(p);
			PAN_HANDLER(p) = http_hook;
		}
	}
	UnlockSched();
}

static void hooks_restore(void) {
	LockSched();
	if (enip_proc && GetGBSProcAddress(PAN_ENIP_CEPID) == enip_proc &&
		PAN_HANDLER(enip_proc) == (GBS_PROC_CALLBACK)(void *)pan_gateway)
		PAN_HANDLER(enip_proc) = enip_old;
	if (java_proc && PAN_HANDLER(java_proc) == java_hook)
		PAN_HANDLER(java_proc) = java_old;
	if (http_proc && GetGBSProcAddress(PAN_HTTP_CEPID) == http_proc &&
		PAN_HANDLER(http_proc) == http_hook)
		PAN_HANDLER(http_proc) = http_old;
	enip_proc = java_proc = http_proc = NULL;
	UnlockSched();
}

static void contexts_release(void) {
	unsigned cid, r;

	LockSched();
	for (cid = 0; cid < 8; ++cid) {
		if (!(owned & (1u << cid)))
			continue;
		for (r = 0; r < NREG; ++r) {
			if (!REFS[r * 8 + cid])
				continue;
			if (r != pin) {
				volatile uint16_t *reg = (volatile uint16_t *)((unsigned char *)RamSocketRegs() + r * 0x14);
				GBS_SendMessage(reg[0], reg[1] + LMAN_DISCONNECT_IND - LMAN_CONNECT_CNF, 0,
								(void *)cid, 0);
			}
			REFS[r * 8 + cid] = 0;
		}
		/* not SetState(0): it queries a physical PDP that does not exist */
		LMAN_FreeContext(cid);
	}
	owned = 0;
	UnlockSched();
}

/* The hooks VKP puts its three dispatchers at the same flash addresses on EL71 and E71 sw45
 * (the hook sites jump there). Each starts with STMFD SP!,{R0-R6,LR}; LDR R0,[PC,#n]. */
static int dispatcher_present(uint32_t address, uint32_t ldr) {
	const volatile uint32_t *p = (const volatile uint32_t *)address;

	return p[0] == 0xE92D407Fu && p[1] == ldr;
}

int hooks_patch_state(void) {
	int lman = dispatcher_present(0xA15BFC00u, 0xE59F0098u);
	int online = dispatcher_present(0xA15BFE00u, 0xE59F00B4u);
	int avail = dispatcher_present(0xA15BF800u, 0xE59F00DCu); /* missing in the old VKP */

	if (lman && online && avail)
		return PATCH_OK;
	if (!lman && !online && !avail)
		return PATCH_NONE;
	return PATCH_OLD;
}

static void dispatchers_log(void) {
	int lman = dispatcher_present(0xA15BFC00u, 0xE59F0098u);
	int online = dispatcher_present(0xA15BFE00u, 0xE59F00B4u);
	int avail = dispatcher_present(0xA15BF800u, 0xE59F00DCu);

	sprintf(log_buf, "LMAN: dispatchers lman=%d wsp-online=%d wsp-avail=%d%s\r\n", lman, online,
			avail, lman && online && avail ? "" : " (browser/elfs need the hooks VKP)");
	plog();
}

void pan_lman_up(uint8_t bearer, uint16_t iface, uint32_t local_ip, uint32_t dns1, uint32_t dns2) {
	pan_bearer = bearer;
	pan_iface = iface;
	if (bearer == 1 && iface < PDP_DRIVER_COUNT && !pdp_dns) {
		volatile uint32_t *d = (volatile uint32_t *)(PDP_DRIVERS + iface * 0x2cu);

		LockSched();
		pdp_saved[0] = d[1];
		pdp_saved[1] = d[2];
		pdp_saved[2] = d[3];
		d[1] = local_ip;
		d[2] = dns1;
		d[3] = dns2 ? dns2 : dns1;
		pdp_dns = d;
		UnlockSched();
		sprintf(log_buf, "LMAN: resolver table iface %u: dns %08X %08X (was %08X %08X %08X)\r\n",
				(unsigned)iface, (unsigned)dns1, (unsigned)dns2, (unsigned)pdp_saved[0],
				(unsigned)pdp_saved[1], (unsigned)pdp_saved[2]);
		plog();
	}
	if (pin >= NREG) {
		usart_puts("LMAN: no helper registration, apps stay on GPRS\r\n");
		return;
	}
	hooks_install();
	pan_ready = true;
	pan_self_context(); /* elfs' sockets (helper 440A) */
	GBS_StartTimerProc(&poll_timer, 216, (void (*)(GBSTMR *))pan_lman_poll);
	sprintf(log_buf, "LMAN: serving NAP requests on bearer %u iface %u (enip=%d java=%d http=%d)\r\n",
			(unsigned)pan_bearer, (unsigned)pan_iface, enip_proc != NULL, java_proc != NULL,
			http_proc != NULL);
	plog();
	dispatchers_log();
}

void pan_lman_down(void) {
	if (!pan_ready && !enip_proc && !java_proc && !http_proc && !owned && !pdp_dns)
		return;
	pan_ready = false;
	if (pdp_dns) {
		LockSched();
		pdp_dns[1] = pdp_saved[0];
		pdp_dns[2] = pdp_saved[1];
		pdp_dns[3] = pdp_saved[2];
		pdp_dns = NULL;
		UnlockSched();
	}
	if (IsTimerProc(&poll_timer))
		GBS_StopTimer(&poll_timer);
	hooks_restore();
	contexts_release();
	self_cid = -1;
	usart_puts("LMAN: hooks restored, contexts released\r\n");
}

void pan_lman_poll(void) {
	static unsigned seen_select, seen_prepared, seen_online, seen_bearer;

	if (!pan_ready)
		return;
	GBS_StartTimerProc(&poll_timer, 216, (void (*)(GBSTMR *))pan_lman_poll);
	if (!java_proc || !http_proc)
		hooks_install(); /* Java/HTTP processes may start after PAN came up */
	{
		/* watch: our contexts' state/bearer/iface and our interface in ENIP, log on change */
		static uint32_t seen_sig = 0xffffffffu;
		static void *seen_if;
		uint32_t sig = owned;
		void *ifp = enip_iph_find_IF_for_bearer(pan_bearer, pan_iface);
		unsigned i;

		for (i = 0; i < 8; ++i)
			if (owned & (1u << i))
				sig = sig * 31u + CTX[i * 0x70] * 7u + CTX[i * 0x70 + 1] * 3u +
					  *(volatile uint16_t *)(CTX + i * 0x70 + 4);
		if (sig != seen_sig || ifp != seen_if) {
			seen_sig = sig;
			seen_if = ifp;
			sprintf(log_buf, "LMAN: watch iface(%u,%u)=%p owned=%02X\r\n", (unsigned)pan_bearer,
					(unsigned)pan_iface, ifp, owned);
			plog();
			for (i = 0; i < 8; ++i) {
				volatile unsigned char *c = CTX + i * 0x70;

				if (c[1] == 0xff)
					continue;
				sprintf(log_buf, "  ctx%u st=%u bearer=%u if=%u ours=%u '%.24s'\r\n", i, c[0],
						c[1], (unsigned)*(volatile uint16_t *)(c + 4), (owned >> i) & 1u,
						(const char *)(c + 8));
				plog();
			}
		}
	}
	if (seen_select != n_select || seen_prepared != n_prepared) {
		seen_select = n_select;
		seen_prepared = n_prepared;
		unsigned i;

		sprintf(log_buf, "LMAN: NAP '%s' owner=%X type=%X -> cid=%X why=%u (req %u, new %u)\r\n",
				last_name, last_owner, last_type, last_cid, last_why, seen_select, seen_prepared);
		plog();
		for (i = 0; i < 8; ++i) {
			volatile unsigned char *c = CTX + i * 0x70;

			if (c[1] == 0xff)
				continue;
			sprintf(log_buf, "  ctx%u st=%u bearer=%u if=%u ours=%u '%.24s'\r\n", i, c[0], c[1],
					(unsigned)*(volatile uint16_t *)(c + 4), (owned >> i) & 1u, (const char *)(c + 8));
			plog();
		}
		for (i = 0; i < NREG; ++i) {
			unsigned cid, mask = 0;

			for (cid = 0; cid < 8; ++cid)
				if (REFS[i * 8 + cid])
					mask |= 1u << cid;
			if (!mask)
				continue;
			sprintf(log_buf, "  reg%u cepid=%X refs=%02X%s\r\n", i,
					*(volatile uint16_t *)((unsigned char *)RamSocketRegs() + i * 0x14), mask,
					i == pin ? " (ours)" : "");
			plog();
		}
	}
	while (notes_tail != notes_head) {
		struct enip_note n = notes[notes_tail % NOTES];

		++notes_tail;
		sprintf(log_buf, "ENIP: %s from=%X owner=%X bearer=%X iface=%X -> error=%X\r\n",
				n.msg == 0x1f ? "socket" : n.msg == 0x30 ? "dns" : "connect", n.from, n.cepid,
				n.bearer, n.iface, n.error);
		plog();
	}
	if (notes_lost) {
		sprintf(log_buf, "ENIP: %u notes lost\r\n", notes_lost);
		notes_lost = 0;
		plog();
	}
	if (seen_online != n_wsp_online || seen_bearer != n_wsp_bearer) {
		seen_online = n_wsp_online;
		seen_bearer = n_wsp_bearer;
		sprintf(log_buf, "WSP: online checks %u, bearer checks %u (last %X)\r\n", seen_online,
				seen_bearer, wsp_bearer_result);
		plog();
	}
}
#endif
