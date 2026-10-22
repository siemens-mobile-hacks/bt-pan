#ifndef PAN_LMAN_H
#define PAN_LMAN_H

#include <stdint.h>

/*
 * Make phone applications (Java, HTTP_DOWNLOAD, WSP browser, elfs) use the PAN interface.
 * See pan_lman.c for the mechanism.
 */

/* MMI, at elf start: register the socket helper (pins our contexts). */
void pan_lman_init(void);
/* PAN interface registered in ENIP as (driver bearer, iface): start serving NAP requests. */
void pan_lman_up(uint8_t bearer, uint16_t iface, uint32_t local_ip, uint32_t dns1, uint32_t dns2);
/* Before the ENIP driver goes away: stop serving, restore hooks, release our contexts. */
void pan_lman_down(void);
/* Periodic (~1 s): install hooks for processes that appear later, log what happened. */
void pan_lman_poll(void);
/* Helper process, before the stack speed test's socket(): make sure the helper has a context. */
void pan_lman_test_begin(void);

#endif
