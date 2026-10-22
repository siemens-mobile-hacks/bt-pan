#ifndef PEERS_H
#define PEERS_H

#include <swilib.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Paired-device list for the device picker. Uses its own short-lived BTA accessor in the
 * BTPAN process (only while PAN is off). No inquiry and
 * no pairing is started.
 */
#define PEERS_MAX 24

struct peer {
	uint8_t mac[6];   /* display order */
	char name[48];    /* UTF-8 */
};

extern struct peer peers[PEERS_MAX];
extern volatile int peer_count;
extern volatile bool peers_busy;

/* BTPAN process only */
void peers_start(uint16_t owner_cepid, uint32_t message_id);
void peers_stop(void);
void peers_handle_bta_msg(GBS_MSG *msg);

#endif
