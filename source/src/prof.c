/* CPU profile of the GBS processes, SG trace builds only: see prof.h */
#include "prof.h"

#if BT_PAN_TRACE && defined(SGOLD)
#include <swilib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt_sg.h"
#include "trace.h"

#define PROF_N 100u
#define PROF_DEPTH 16u
#define PROF_TICKS 216u      /* 1 s */
#define PROF_RESCAN 30u      /* s: processes created later (HTTP, Java ...) */
#define PROF_FULL 60u        /* s: full list since start */
#define STM (*(volatile uint32_t *)0xF4B00010u)
#define PROC_CEPID(p) (*(volatile uint16_t *)((char *)(p) + 8))      /* CreateGBSproc A0827CB8 */
#define PROC_HANDLER(p) (*(GBS_PROC_CALLBACK volatile *)((char *)(p) + 0x3C))
#define PROC_TYPE(p) (*(volatile uint8_t *)((char *)(p) + 0x40)) /* 2 = CreateGBSproc */

struct prof_slot {
	uint16_t cepid;
	void *proc;               /* wrapped processes only */
	GBS_PROC_CALLBACK old;
	uint32_t n, ticks;        /* this second */
	uint32_t n_all, ticks_all; /* since start */
};
static struct prof_slot slots[PROF_N];
static unsigned n_slots = PROF_FIXED;
static volatile unsigned sp;
static volatile uint32_t st_t0[PROF_DEPTH], st_child[PROF_DEPTH];
static volatile uint8_t st_slot[PROF_DEPTH];
static volatile int prof_on;
static GBSTMR prof_timer;
static uint32_t last_stm, start_stm, last_busy, last_idle, last_loops;
static unsigned beats;
static uint32_t self_ticks; /* our own file writes in MMI, not the firmware's */
static unsigned n_other;
static uint16_t other[8];   /* processes of another type: not wrapped, listed */

void prof_in(unsigned slot) {
	unsigned k;

	LockSched();
	k = sp++;
	if (k < PROF_DEPTH) {
		st_slot[k] = (uint8_t)slot;
		st_child[k] = 0;
		st_t0[k] = STM;
	}
	UnlockSched();
}

void prof_out(unsigned slot) {
	uint32_t now, dur;
	unsigned k, top;

	LockSched();
	now = STM;
	top = sp < PROF_DEPTH ? sp : PROF_DEPTH;
	for (k = top; k-- > 0;)
		if (st_slot[k] == slot)
			break;
	if (k < top) { /* found: usually the top (strict nesting) */
		dur = now - st_t0[k];
		slots[slot].ticks += dur > st_child[k] ? dur - st_child[k] : 0;
		++slots[slot].n;
		if (k > 0)
			st_child[k - 1] += dur;
		for (; k + 1 < top; ++k) { /* a handler that blocked: others ran above it */
			st_slot[k] = st_slot[k + 1];
			st_child[k] = st_child[k + 1];
			st_t0[k] = st_t0[k + 1];
		}
	} else {
		++slots[slot].n; /* deeper than PROF_DEPTH: counted without time */
	}
	if (sp)
		--sp;
	UnlockSched();
}

static void prof_call(unsigned i) {
	GBS_PROC_CALLBACK old = slots[i].old;

	prof_in(i);
	old();
	prof_out(i);
}

#define S1(n) static void prof_stub_##n(void) { prof_call(n); }
#define S10(d) S1(d##0) S1(d##1) S1(d##2) S1(d##3) S1(d##4) S1(d##5) S1(d##6) S1(d##7) S1(d##8) S1(d##9)
#define T1(n) prof_stub_##n,
#define T10(d) T1(d##0) T1(d##1) T1(d##2) T1(d##3) T1(d##4) T1(d##5) T1(d##6) T1(d##7) T1(d##8) T1(d##9)
S1(0) S1(1) S1(2) S1(3) S1(4) S1(5) S1(6) S1(7) S1(8) S1(9)
S10(1) S10(2) S10(3) S10(4) S10(5) S10(6) S10(7) S10(8) S10(9)
static const GBS_PROC_CALLBACK stubs[PROF_N] = {
	T1(0) T1(1) T1(2) T1(3) T1(4) T1(5) T1(6) T1(7) T1(8) T1(9)
	T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9)
};

/* pan_lman.c hooks these itself (its proc_of wants a flash handler); BTPAN counts itself */
static int excluded(unsigned cepid) {
	return cepid == 0x40DD || cepid == 0x40B4 || cepid == 0x4409 || cepid == 0x4403 || cepid == 0x1A00;
}

static int known(void *proc) {
	unsigned i;

	for (i = PROF_FIXED; i < n_slots; ++i)
		if (slots[i].proc == proc)
			return 1;
	return 0;
}

/* every cepid: GetGBSProcAddress (A0824144) = group table [cepid >> 8] (0x100 entries,
 * A0822DDC + 0x10) -> member [cepid & 0xFF] below the group's count, else 0 */
static unsigned scan(void) {
	unsigned cepid, added = 0;

	for (cepid = 0; cepid < 0xFFFFu && n_slots < PROF_N; ++cepid) {
		void *p;

		if ((cepid & 0xFFu) == 0xFFu || excluded(cepid))
			continue;
		p = GetGBSProcAddress((int)cepid);
		if (!p || PROC_CEPID(p) != cepid || !PROC_HANDLER(p) || known(p))
			continue;
		if (PROC_TYPE(p) != 2) {
			unsigned k;
			for (k = 0; k < n_other && k < 8u && other[k] != cepid; ++k) {}
			if (k == n_other) {
				if (k < 8u) other[k] = (uint16_t)cepid;
				++n_other;
			}
			continue;
		}
		LockSched();
		slots[n_slots].cepid = (uint16_t)cepid;
		slots[n_slots].proc = p;
		slots[n_slots].old = PROC_HANDLER(p);
		PROC_HANDLER(p) = stubs[n_slots];
		++n_slots;
		UnlockSched();
		++added;
	}
	return added;
}

/* the firmware's load counters (PsCtrl idle task FUN_a1246e54, GetCPULoad A08F911E):
 * +0x68 counting on, +0x6C ticks in the idle loop, +0x74 ticks outside it, +0x4C idle-loop
 * passes; GetCPULoad = 74*100/(74+6C) and zeroes both */
static volatile uint32_t *fw_load(void) {
	switch (bt_sg_fw()) {
	case SG_FW_S65V58: return (volatile uint32_t *)0xA86C5744u;
	case SG_FW_SK65V50: return (volatile uint32_t *)0xA8E738B0u;
	default: return NULL;
	}
}

static void sort_top(unsigned *idx, unsigned n_idx, int all) {
	unsigned i, j;

	for (i = 1; i < n_idx; ++i)
		for (j = i; j > 0; --j) {
			uint32_t a = all ? slots[idx[j]].ticks_all : slots[idx[j]].ticks;
			uint32_t b = all ? slots[idx[j - 1]].ticks_all : slots[idx[j - 1]].ticks;
			unsigned t;
			if (a <= b)
				break;
			t = idx[j];
			idx[j] = idx[j - 1];
			idx[j - 1] = t;
		}
}

static void prof_tick(GBSTMR *timer) {
	static char line[512];
	static unsigned idx[PROF_N];
	volatile uint32_t *fw = fw_load();
	uint32_t now = STM, dt = now - last_stm, t_self = now, busy = 0, idle = 0, loops = 0, meas = 0;
	unsigned i, n_idx = 0, l, fwpct = 0;

	(void)timer;
	if (!prof_on)
		return;
	if (fw) {
		uint32_t b = fw[0x74 / 4], id = fw[0x6C / 4];

		/* someone's GetCPULoad (CpuMon) zeroes them: then only what came since */
		busy = b >= last_busy ? b - last_busy : b;
		idle = id >= last_idle ? id - last_idle : id;
		last_busy = b;
		last_idle = id;
		loops = fw[0x4C / 4] - last_loops;
		last_loops = fw[0x4C / 4];
		fwpct = busy + idle ? (unsigned)((uint64_t)busy * 100u / (busy + idle)) : 0;
	}
	LockSched();
	for (i = 0; i < n_slots; ++i) {
		struct prof_slot *s = &slots[i];
		meas += s->ticks;
		s->n_all += s->n;
		s->ticks_all += s->ticks;
		if (s->n)
			idx[n_idx++] = i;
	}
	UnlockSched();
	sort_top(idx, n_idx, 0);
	l = (unsigned)snprintf(line, sizeof(line), "CPU %u dt %lu fw %u%% (busy %lu idle %lu loops %lu) ours %lu%% self %lu%% |",
						   beats, (unsigned long)dt, fwpct, (unsigned long)busy, (unsigned long)idle,
						   (unsigned long)loops, dt ? (unsigned long)((uint64_t)meas * 1000u / dt) : 0ul,
						   dt ? (unsigned long)((uint64_t)self_ticks * 1000u / dt) : 0ul);
	for (i = 0; i < n_idx && i < 12 && l < sizeof(line) - 40u; ++i) {
		struct prof_slot *s = &slots[idx[i]];
		unsigned cepid = idx[i] < PROF_FIXED ? (const unsigned[]){0x1A00, 0x40DD, 0x40B4, 0x4409, 0x4403}[idx[i]]
											 : s->cepid;
		l += (unsigned)snprintf(line + l, sizeof(line) - l, " %X:%lu/%lu", cepid, (unsigned long)s->n,
								dt ? (unsigned long)((uint64_t)s->ticks * 1000u / dt) : 0ul);
	}
	for (i = 0; i < n_slots; ++i)
		slots[i].n = slots[i].ticks = 0;
	trace(line); /* "ours" and per-process numbers in 0.1 % of wall time; n = messages */
	if (++beats % PROF_FULL == 0) {
		uint32_t all = now - start_stm;
		n_idx = 0;
		for (i = 0; i < n_slots; ++i)
			if (slots[i].n_all)
				idx[n_idx++] = i;
		sort_top(idx, n_idx, 1);
		l = (unsigned)snprintf(line, sizeof(line), "CPU ALL %u s, %u procs:", beats, n_slots);
		for (i = 0; i < n_idx && l < sizeof(line) - 40u; ++i) {
			struct prof_slot *s = &slots[idx[i]];
			unsigned cepid = idx[i] < PROF_FIXED ? (const unsigned[]){0x1A00, 0x40DD, 0x40B4, 0x4409, 0x4403}[idx[i]]
												 : s->cepid;
			l += (unsigned)snprintf(line + l, sizeof(line) - l, " %X:%lu/%lu", cepid, (unsigned long)s->n_all,
									all ? (unsigned long)((uint64_t)s->ticks_all * 1000u / all) : 0ul);
		}
		trace(line);
	}
	if (beats % PROF_RESCAN == 0) {
		unsigned added = scan();
		if (added) {
			snprintf(line, sizeof(line), "CPU rescan: +%u, %u procs", added, n_slots);
			trace(line);
		}
	}
	self_ticks = STM - t_self;
	last_stm = now;
	GBS_StartTimerProc(&prof_timer, PROF_TICKS, prof_tick);
}

void prof_start(void) {
	volatile uint32_t *fw = fw_load();
	char line[160];
	uint32_t t0 = STM;
	unsigned added;

	if (prof_on)
		return;
	if (fw && !fw[0x68 / 4]) { /* as StartCpuUsageCount A1247160 (the first GetCPULoad) */
		LockSched();
		fw[0x6C / 4] = 0;
		fw[0x74 / 4] = 0;
		fw[0x78 / 4] = STM;
		fw[0x68 / 4] = 1;
		UnlockSched();
	}
	if (fw) {
		last_busy = fw[0x74 / 4];
		last_idle = fw[0x6C / 4];
		last_loops = fw[0x4C / 4];
	}
	prof_on = 1;
	added = scan();
	snprintf(line, sizeof(line), "CPU profile: %u procs wrapped in %lu ticks, fw counters %s; "
			 "%u of another type: %X %X %X %X %X %X %X %X", added, (unsigned long)(STM - t0),
			 fw ? "yes" : "unknown fw", n_other, other[0], other[1], other[2], other[3], other[4], other[5],
			 other[6], other[7]);
	trace(line);
	start_stm = last_stm = STM;
	GBS_StartTimerProc(&prof_timer, PROF_TICKS, prof_tick);
}

void prof_stop(void) {
	unsigned i, left = 0;
	char line[64];

	if (!prof_on)
		return;
	prof_on = 0;
	if (IsTimerProc(&prof_timer))
		GBS_StopTimer(&prof_timer);
	LockSched();
	for (i = PROF_FIXED; i < n_slots; ++i) {
		struct prof_slot *s = &slots[i];
		if (GetGBSProcAddress(s->cepid) == s->proc && PROC_HANDLER(s->proc) == stubs[i])
			PROC_HANDLER(s->proc) = s->old;
		else if (GetGBSProcAddress(s->cepid) == s->proc)
			++left; /* someone hooked on top of us: cannot take ours out */
	}
	n_slots = PROF_FIXED;
	UnlockSched();
	snprintf(line, sizeof(line), "CPU profile stopped, %u handlers left hooked", left);
	trace(line);
}
#endif
