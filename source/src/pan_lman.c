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

#include <swilib.h>

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
	unsigned before = GetSocketRegIndex(cepid);
	int result = before < NREG ? 2 : RegisterCepidForSocketWork(&reg);

	pin = GetSocketRegIndex(cepid);
	sprintf(log_buf, "LMAN: helper cepid=%X registration=%u result=%d\r\n", cepid, pin, result);
	plog();
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
