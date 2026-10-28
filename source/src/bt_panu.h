#ifndef BT_PANU_H
#define BT_PANU_H

#include <swilib.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bt.h"

#define BT_PANU_RX_BUF_SIZE 0x700u

enum bt_panu_state {
	BT_PANU_STATE_IDLE,
	BT_PANU_STATE_STARTING,
	BT_PANU_STATE_CONNECTING,
	BT_PANU_STATE_CONNECTED,
	BT_PANU_STATE_STOPPING,
};

struct bt_panu;

struct bt_panu_cb {
	void (*connected)(struct bt_panu *panu, void *user_data);
	/*
	 * data points into the bt_panu RX buffer and is valid only for the
	 * duration of the callback.
	 */
	void (*recv)(struct bt_panu *panu, const uint8_t *data, size_t len, void *user_data);
	void (*disconnected)(struct bt_panu *panu, void *user_data);
};

struct bt_panu_config {
	uint16_t owner_cepid;
	uint32_t message_id;
	uint8_t peer_addr[6];
	uint16_t rx_mtu;
	uint16_t tx_mtu;
	const struct bt_panu_cb *cb;
	void *user_data;
};

struct bt_panu {
	struct bt_panu_config config;
	BT_HANDLE accessor;      /* SGOLD: the message replicator (bt_sg.h) */
	BT_HANDLE conn;
	BT_REMOTE_DEVICE_INFO *remote_device;
	BT_TRANSACTION_ID transaction_id;
	uint8_t local_addr[6];
	GBSTMR setup_timer;
	GBSTMR rx_timer;
	GBSTMR close_timer;
	GBSTMR acl_timer;
	char log_buf[256];
	uint8_t rx_buf[BT_PANU_RX_BUF_SIZE];
	enum bt_panu_state state;
	bool app_installed;      /* our SDev app sits in SDEV_APPS[0x05] */
	uint16_t pool_tx_base;   /* free L2CAP send buffers when the channel opened */
	uint16_t pool_rx_base;   /* free receive buffers when the channel opened */
	unsigned int drain_polls;
	unsigned int close_polls;
	bool close_sent;
	bool acl_disconnect_requested;
	bool acl_disconnect_pending;
	bool acl_disconnect_settling;
	bool setup_accepted;
	unsigned int setup_attempts;
	bool mr_stopping;        /* SGOLD: MR_Stop sent, waiting for its event */
	unsigned int mr_polls;
};

void bt_panu_init(struct bt_panu *panu, const struct bt_panu_config *config);
int bt_panu_start(struct bt_panu *panu);
void bt_panu_handle_bta_msg(struct bt_panu *panu, GBS_MSG *msg);
int bt_panu_send(struct bt_panu *panu, const uint8_t *data, size_t len);
void bt_panu_stop(struct bt_panu *panu);
bool bt_panu_is_stopped(const struct bt_panu *panu);
int bt_panu_deinit(struct bt_panu *panu);
/* free L2CAP send buffers now (shared pool A8DA226C) */
unsigned bt_panu_tx_free(void);
uint32_t bt_panu_rx_count(void);  /* packets the stack handed us so far */
void bt_panu_rx_report(struct bt_panu *panu, const char *why);
void bt_panu_rx_kick(struct bt_panu *panu);

#endif
