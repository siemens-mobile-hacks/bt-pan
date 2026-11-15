#ifndef BT_PAN_PROF_H
#define BT_PAN_PROF_H

/*
 * CPU profile of the GBS processes (SG trace builds only: bt-pan_SG_TRACE*). Why: SK65 showed
 * 60-70 % CPU load while connected and idle, and our own code wakes ~2 times a second then.
 * prof_start (MMI, our CSM's oncreate) wraps the handler (proc+0x3C) of every GBS process
 * found and counts messages and STM ticks spent in each (exclusive of nested preemption);
 * the processes our pan_lman.c hooks itself (40DD, 40B4, 4409, 4403) and our BTPAN count from
 * there (PROF_IN/PROF_OUT). Once a second from MMI a "CPU" line goes to the trace file: wall
 * ticks, the firmware's own load counters (what GetCPULoad/CpuMon shows: time outside the
 * PsCtrl idle task), the measured share, and the top processes. prof_stop (MMI, onclose)
 * restores the handlers before the elf can unload.
 */

#if BT_PAN_TRACE && defined(SGOLD)
enum { PROF_BTPAN, PROF_LMAN, PROF_ENIP, PROF_H4409, PROF_H4403, PROF_FIXED };
void prof_start(void);
void prof_stop(void);
void prof_in(unsigned slot);
void prof_out(unsigned slot);
#define PROF_IN(s) prof_in(s)
#define PROF_OUT(s) prof_out(s)
#else
#define prof_start() ((void)0)
#define prof_stop() ((void)0)
#define PROF_IN(s) ((void)0)
#define PROF_OUT(s) ((void)0)
#endif

#endif
