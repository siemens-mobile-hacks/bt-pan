#include "bt_panu.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "bnep.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define BT_PANU_ACL_DISCONNECT_POLL_TICKS 216
#define BT_PANU_ACL_DISCONNECT_SETTLE_TICKS (2 * 216)
#define BT_PANU_RX_POLL_TICKS 5
#define BT_PANU_DRAIN_TICKS 22
#define BT_PANU_DRAIN_MAX 14   /* ~1.4 s for the send buffers to come back */
#define BT_PANU_CLOSE_MAX 5    /* ~5 s for the close event before giving up */

static const uint8_t zero_addr[6];

/*
 * Transport: SDev (the layer JSApi/JSR-82 itself sits on). While we run, our app replaces
 * SDEV_APPS[0x0F] (accessor/JSR-82 app; JSApi_InitHandles puts JSApi there) and SDEV_APPS[0x05]
 * (accessor client connections with SAPI 0x16..0x19). With a stub left in 0x0F the open is
 * refused at once (result 0x2000 layer 0x0E, our app never called) - seen on device with only 0x05 replaced. On device the PAN channel
 * binds to app 0x0F.
 * The stack calls ep.fn[0] in its own process for every received L2CAP SDU: we queue it in
 * rx_ring and the 0x1A00 process drains it. Writes go through the device's fn[2]. If fn[0]
 * refuses a packet the device holds it until fn[1] is called (DevDeliver).
 */
#define SDEV_APP_SERVER 0x0F
#define SDEV_APP_CLIENT 0x05
#define RX_RING_SIZE 0x8000u

static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head, rx_tail; /* head: stack callback, tail: our poll */
static volatile int rx_held;
static SDEV ep;
static SDEV *volatile sdev;
static volatile int ep_busy;
static const uint8_t app_ids[2] = {SDEV_APP_SERVER, SDEV_APP_CLIENT};
static SDEV_APP app[2], *saved_app[2];
static volatile uint16_t *bt_pool;
/* receive path progress (rx_follow, rx_guard, the watchdog): stack callback -> ring -> our drain */
static volatile uint32_t rx_calls;
static uint32_t rx_drained, rx_polls;
static uint32_t tx_ok, tx_fail;	/* debug screen (BT_PAN_DBG) */

#define POOL_RX_FREE() (bt_pool ? bt_pool[0] : 0)
#define POOL_TX_FREE() (bt_pool ? bt_pool[1] : 0)

static void ring_write(uint32_t h, const uint8_t *src, uint32_t n) {
	uint32_t at = h % RX_RING_SIZE, first = RX_RING_SIZE - at;

	if (first > n)
		first = n;
	memcpy(rx_ring + at, src, first);
	if (n > first)
		memcpy(rx_ring, src + first, n - first);
}

static void ring_read(uint32_t t, uint8_t *dst, uint32_t n) {
	uint32_t at = t % RX_RING_SIZE, first = RX_RING_SIZE - at;

	if (first > n)
		first = n;
	memcpy(dst, rx_ring + at, first);
	if (n > first)
		memcpy(dst + first, rx_ring, n - first);
}

/* Event-driven receive (SGOLD, config.rx_message != 0): the stack callback posts one message to
 * the owner process (BTPAN) when the ring gets data, instead of a 5-tick poll timer that ran 43
 * times a second and on S65 got lost now and then (D106 trace: 21 restarts in 3 min). */
static volatile uint16_t rx_wake_cepid;
static volatile uint32_t rx_wake_msg;
static volatile int rx_wake_pending;

static void rx_wake(void) {
	if (!rx_wake_msg || rx_wake_pending)
		return;
	rx_wake_pending = 1;
	GBS_SendMessage(rx_wake_cepid, rx_wake_msg, 0, 0, 0);
}

static int sdev_rx(const uint8_t *data, int len, int index) {
	uint32_t h;
	uint8_t hdr[2];

	(void)index;
	if (len <= 0)
		return 0;
	++rx_calls;
	if (len > (int)BT_PANU_RX_BUF_SIZE)
		return len; /* cannot exceed our rxMTU; take it so the channel keeps moving */
	if (rx_head - rx_tail + 2u + (uint32_t)len > RX_RING_SIZE) {
		rx_held = 1;
		rx_wake();
		return 0;
	}
	h = rx_head;
	hdr[0] = (uint8_t)len;
	hdr[1] = (uint8_t)(len >> 8);
	ring_write(h, hdr, 2);
	ring_write(h + 2, data, (uint32_t)len);
	rx_head = h + 2 + (uint32_t)len;
	rx_wake();
	return len;
}

static void *sdev_nop(int index) {
	(void)index;
	return NULL;
}

static int sdev_open(int app_id, SDEV **out, int dev_type, const uint8_t *info) {
	(void)dev_type;
	(void)info;
	if (ep_busy) {
		*out = NULL;
		return 0;
	}
	ep_busy = 1;
	ep.app = (uint8_t)app_id; /* the stack resets these when it unbinds */
	ep.index = 0;
	rx_head = rx_tail = 0;
	rx_held = 0;
	*out = &ep;
	return 1;
}

static int sdev_open_server(SDEV **out, int dev_type, const uint8_t *info) {
	return sdev_open(SDEV_APP_SERVER, out, dev_type, info);
}

static int sdev_open_client(SDEV **out, int dev_type, const uint8_t *info) {
	return sdev_open(SDEV_APP_CLIENT, out, dev_type, info);
}

static void sdev_register(SDEV *dev, const void *info) {
	(void)info;
	sdev = dev;
}

/* may come twice for one connection */
static void sdev_close(int index) {
	(void)index;
	sdev = NULL;
	ep_busy = 0;
}

static int sdev_get_dev(int index, int *inst) {
	SDEV *dev = sdev;

	(void)index;
	if (!dev)
		return 0xff;
	*inst = dev->inst;
	return dev->type;
}

static int bt_panu_install_app(struct bt_panu *panu) {
	void **apps = RamSDevApps();

	if (panu->app_installed)
		return 0;
	SDev_InitEndpoint(&ep);
	ep.fn[0] = (void *)sdev_rx;
	ep.fn[3] = (void *)sdev_nop;
	ep.fn[6] = (void *)sdev_nop;
	ep.fn[8] = (void *)sdev_nop;
	ep.app = SDEV_APP_CLIENT;
	ep.index = 0;
	sdev = NULL;
	ep_busy = 0;

	for (int i = 0; i < 2; i++) {
		int id = app_ids[i];

		saved_app[i] = apps[id]; /* the stubs, or JSApi if Java has used BT */
		if (!saved_app[i]) { /* no app table */
			while (i--)
				apps[app_ids[i]] = saved_app[i];
			return -1;
		}
		app[i] = *saved_app[i]; /* unbind (moving a device to another app) stays as it was */
		app[i].open = id == SDEV_APP_SERVER ? sdev_open_server : sdev_open_client;
		app[i].reg = sdev_register;
		app[i].close = sdev_close;
		app[i].get_dev = sdev_get_dev;
		apps[id] = &app[i];
	}
	panu->app_installed = true;
	return 0;
}

/* only once no connection uses our endpoint any more */
static void bt_panu_restore_app(struct bt_panu *panu) {
	void **apps;

	if (!panu->app_installed)
		return;
	apps = RamSDevApps();
	for (int i = 0; i < 2; i++) {
		apps[app_ids[i]] = saved_app[i];
		saved_app[i] = NULL;
	}
	sdev = NULL;
	ep_busy = 0;
	panu->app_installed = false;
}

/* the device kept a packet back: let it deliver again (L2CAP: BTLP_vDataToAppCallback) */
static void bt_panu_deliver(void) {
	SDEV *dev = sdev;

	rx_held = 0;
	if (dev)
		((void (*)(int))dev->fn[1])(dev->inst);
}

unsigned bt_panu_tx_free(void) {
	return POOL_TX_FREE();
}

static void bt_panu_stop_timer(GBSTMR *timer) {
	if (IsTimerProc(timer))
		GBS_StopTimer(timer);
}

int bt_panu_send(struct bt_panu *panu, const uint8_t *data, size_t len) {
	SDEV *dev = sdev;
	int written;

	if (panu->conn == BT_INVALID_HANDLE || !dev || panu->close_sent)
		return 0;

	/* L2CAP takes a whole SDU or nothing */
	written = ((int (*)(const uint8_t *, int, int))dev->fn[2])(data, (int)len, dev->inst);
	if (written > 0)
		++tx_ok;
	else
		++tx_fail;
	return written < 0 ? 0 : written;
}

static int bt_panu_handle_filter_request(struct bt_panu *panu, const uint8_t *packet, size_t len) {
	const struct bnep_set_filter_req *request;
	uint8_t response[sizeof(struct bnep_control_rsp)];
	size_t response_len;
	uint8_t response_type;

	request = (const struct bnep_set_filter_req *)packet;
	if (!bnep_validate_filter_request(request, len))
		return 0;

	if (request->ctrl == BNEP_FILTER_NET_TYPE_SET) {
		response_type = BNEP_FILTER_NET_TYPE_RSP;
	} else {
		response_type = BNEP_FILTER_MULT_ADDR_RSP;
	}

	response_len = bnep_encode_control_response(response, sizeof(response), response_type,
											BNEP_FILTER_UNSUPPORTED_REQ);
	bt_panu_send(panu, response, response_len);
	return 1;
}

static void bt_panu_handle_packet(struct bt_panu *panu, const uint8_t *packet, size_t len) {
	const struct bnep_control_rsp *response;
	uint16_t response_code;

	response = (const struct bnep_control_rsp *)packet;
	if (bnep_validate_setup_response(response, len)) {
		response_code = ntohs(response->resp);
		if (response_code == 0 && !panu->setup_accepted) {
			panu->setup_accepted = true;
			if (panu->config.cb && panu->config.cb->connected)
				panu->config.cb->connected(panu, panu->config.user_data);
		}
		return;
	}

	if (bt_panu_handle_filter_request(panu, packet, len))
		return;

	if (panu->config.cb && panu->config.cb->recv)
		panu->config.cb->recv(panu, packet, len, panu->config.user_data);
}

/* Take queued packets out of rx_ring. drop: closing, discard them. */
static void bt_panu_drain_ring(struct bt_panu *panu, bool drop) {
	unsigned int packets = 0;

	while (rx_head != rx_tail && packets++ < 32) {
		uint32_t t = rx_tail;
		uint8_t hdr[2];
		uint32_t n;

		ring_read(t, hdr, 2);
		n = hdr[0] | (uint32_t)hdr[1] << 8;
		ring_read(t + 2, panu->rx_buf, n);
		rx_tail = t + 2 + n;
		++rx_drained;
		if (!drop)
			bt_panu_handle_packet(panu, panu->rx_buf, n);
		if (!drop && panu->state != BT_PANU_STATE_CONNECTED)
			break;
		/* D137: our ACKs wait in this process's queue; let them out, rx_follow posts the rest */
		if (!drop && panu->config.rx_yield && *panu->config.rx_yield)
			break;
	}
	/* The device holds received buffers until fn[1]: pump it after a refusal, and while the
	 * receive pool sits below its level at open (one fn[1] need not drain the backlog). */
	if ((rx_held || POOL_RX_FREE() < panu->pool_rx_base) && rx_head - rx_tail < RX_RING_SIZE / 2)
		bt_panu_deliver();
}

static bool bt_panu_rx_active(struct bt_panu *panu) {
	return panu->state == BT_PANU_STATE_CONNECTED && panu->conn != BT_INVALID_HANDLE;
}

/* wake mode: after a drain, keep a short follow-up poll only while something is pending
 * (ring not empty, a refusal, the device's pool below its level); stop after 4 polls in a
 * row with nothing new from the stack - the next packet posts a message again */
static unsigned rx_follow_idle;
static uint32_t rx_follow_calls;

static void bt_panu_poll_rx(GBSTMR *timer);

static void bt_panu_rx_follow(struct bt_panu *panu) {
	if (rx_head != rx_tail) {
		rx_follow_idle = 0;
		rx_wake(); /* more than one batch: continue through the queue, others' messages first */
		return;
	}
	if (!rx_held && POOL_RX_FREE() >= panu->pool_rx_base)
		return;
	if (rx_calls != rx_follow_calls) {
		rx_follow_calls = rx_calls;
		rx_follow_idle = 0;
	} else if (++rx_follow_idle > 4) {
		return;
	}
	if (!IsTimerProc(&panu->rx_timer))
		GBS_StartTimerProc(&panu->rx_timer, BT_PANU_RX_POLL_TICKS, bt_panu_poll_rx);
}

static void bt_panu_poll_rx(GBSTMR *timer) {
	struct bt_panu *panu = container_of(timer, struct bt_panu, rx_timer);

	if (!bt_panu_rx_active(panu))
		return;
	bt_panu_drain_ring(panu, false);
	++rx_polls;
	if (!bt_panu_rx_active(panu))
		return;
	if (rx_wake_msg)
		bt_panu_rx_follow(panu);
	else
		GBS_StartTimerProc(&panu->rx_timer, BT_PANU_RX_POLL_TICKS, bt_panu_poll_rx);
}

/* the owner process got config.rx_message */
void bt_panu_rx_event(struct bt_panu *panu) {
	rx_wake_pending = 0;
	if (!bt_panu_rx_active(panu))
		return;
	rx_follow_idle = 0;
	bt_panu_drain_ring(panu, false);
	++rx_polls;
	if (bt_panu_rx_active(panu))
		bt_panu_rx_follow(panu);
}

/* Called once a second / every 5 s from BTPAN. Poll mode (EL71): no poll since the last call
 * -> the GBS timer restart got lost (S65 D97: after ~300 KB) -> poll here, it re-arms.
 * Wake mode (SGOLD): data waiting but nothing drained since the last call (a lost message)
 * -> drain here; the device's pool below its level with no traffic -> one fn[1]. */
void bt_panu_rx_guard(struct bt_panu *panu) {
	static uint32_t last_polls = 0xffffffffu, last_drained = 0xffffffffu, last_calls;

	if (!bt_panu_rx_active(panu)) {
		last_polls = last_drained = 0xffffffffu;
		return;
	}
	if (rx_wake_msg) {
		if ((rx_head != rx_tail || rx_held) && rx_drained == last_drained) {
			rx_wake_pending = 0;
			bt_panu_rx_event(panu);
		} else if (POOL_RX_FREE() < panu->pool_rx_base && rx_calls == last_calls) {
			bt_panu_deliver();
		}
		last_drained = rx_drained;
		last_calls = rx_calls;
		return;
	}
	if (rx_polls == last_polls) {
		if (IsTimerProc(&panu->rx_timer))
			GBS_StopTimer(&panu->rx_timer);
		bt_panu_poll_rx(&panu->rx_timer);
	}
	last_polls = rx_polls;
}

uint32_t bt_panu_rx_count(void) {
	return rx_calls;
}

/* let the device hand over anything it holds back, then drain */
void bt_panu_rx_kick(struct bt_panu *panu) {
	bt_panu_deliver();
	if (panu->state == BT_PANU_STATE_CONNECTED)
		bt_panu_drain_ring(panu, false);
}

static void bt_panu_send_setup(GBSTMR *timer) {
	struct bt_panu *panu = container_of(timer, struct bt_panu, setup_timer);
	uint8_t setup[8];
	size_t len;

	bool sent;

	if (panu->state != BT_PANU_STATE_CONNECTED || panu->setup_accepted)
		return;
	if (panu->setup_attempts >= 5)
		return; /* no response */

	len = bnep_encode_setup_request(setup, sizeof(setup), BNEP_SVC_NAP, BNEP_SVC_PANU);
	++panu->setup_attempts;
	sent = bt_panu_send(panu, setup, len) == (int)len;
	/* resend until the response arrives; the device may bind a moment after event 9 */
	GBS_StartTimerProc(&panu->setup_timer, sent ? 216 : 22, bt_panu_send_setup);
}

static void bt_panu_connection_opened(struct bt_panu *panu, BT_HANDLE conn, unsigned result,
									  unsigned type) {
	bool stopping = panu->state == BT_PANU_STATE_STOPPING;

	(void)type;
	if (result != 0)
		return;

	panu->conn = conn;
#ifndef SGOLD
	panu->remote_device = BTA_Conn_GetRemDevInfo(panu->accessor, panu->conn);
#endif
	if (stopping)
		return;

	panu->state = BT_PANU_STATE_CONNECTED;
#ifdef SGOLD
	panu->neg_tx_mtu = panu->config.tx_mtu; /* no parameters event on the CA layer */
	panu->neg_rx_mtu = panu->config.rx_mtu;
#else
	panu->neg_tx_mtu = panu->neg_rx_mtu = 0; /* until L2CAP_PARAMETERS_CFM */
#endif
	panu->setup_attempts = 0;
	panu->setup_accepted = false;
	panu->pool_tx_base = POOL_TX_FREE();
	panu->pool_rx_base = POOL_RX_FREE();
#ifndef SGOLD
	BTA_Conn_GetL2CAPParamReq(panu->accessor, panu->conn, ++panu->transaction_id);
#endif
	rx_wake_cepid = panu->config.owner_cepid;
	rx_wake_pending = 0;
	rx_wake_msg = panu->config.rx_message; /* 0: the periodic poll */
	if (rx_wake_msg)
		rx_wake(); /* anything that came with the open */
	else
		GBS_StartTimerProc(&panu->rx_timer, BT_PANU_RX_POLL_TICKS, bt_panu_poll_rx);
	GBS_StartTimerProc(&panu->setup_timer, 22, bt_panu_send_setup);
}

static void bt_panu_connect(struct bt_panu *panu) {
	BT_BD_ADDR peer_addr;

	if (panu->state == BT_PANU_STATE_CONNECTING || panu->state == BT_PANU_STATE_CONNECTED ||
		panu->state == BT_PANU_STATE_STOPPING)
		return;

	if (bt_panu_install_app(panu)) {
		panu->state = BT_PANU_STATE_IDLE;
		return;
	}

	panu->state = BT_PANU_STATE_CONNECTING;
	panu->acl_disconnect_requested = false;
	bt_bdaddr_from_mac(&peer_addr, panu->config.peer_addr);

	/* Phone as PANU connects to the host NAP (enc=0 auth=0). */
#ifdef SGOLD
	{
		void *remdev = CA_RemDev_New(&peer_addr);

		if (!remdev) {
			panu->state = BT_PANU_STATE_IDLE;
			return;
		}
		panu->conn = CA_L2CAP_New(panu->accessor, remdev, BT_SAPI_TYPE_L2CAP, BT_PSM_BNEP, 0, 0, 0,
								  panu->config.rx_mtu, panu->config.tx_mtu);
		CA_RemDev_Free(remdev);
	}
#else
	panu->conn =
		BTA_Conn_New_L2CAP(panu->accessor, &peer_addr, BT_SAPI_TYPE_L2CAP, BT_PSM_BNEP, 0, 0, 0,
						   panu->config.rx_mtu, panu->config.tx_mtu, ++panu->transaction_id);
#endif
	if (panu->conn == BT_INVALID_HANDLE || panu->conn == 0xffffu) {
		panu->conn = BT_INVALID_HANDLE;
		panu->state = BT_PANU_STATE_IDLE;
	}
}

#ifdef SGOLD
/* No ACL disconnect request found on S65: the stack drops the ACL link itself when idle. */
static void bt_panu_disconnect_acl(struct bt_panu *panu) {
	(void)panu;
}
#else
static void bt_panu_acl_disconnect_timeout(GBSTMR *timer) {
	struct bt_panu *panu = container_of(timer, struct bt_panu, acl_timer);
	uint32_t status;

	if (!panu->acl_disconnect_pending)
		return;
	if (!panu->remote_device) {
		panu->acl_disconnect_pending = false;
		return;
	}
	if (panu->acl_disconnect_settling) {
		panu->acl_disconnect_pending = false;
		panu->acl_disconnect_settling = false;
		panu->remote_device = NULL;
		return;
	}

	status = BTA_RemBtDeviceInfo_GetStatus(panu->accessor, panu->remote_device, 0);
	if (status != 2) { /* ACL gone: settle */
		panu->acl_disconnect_settling = true;
		GBS_StartTimerProc(&panu->acl_timer, BT_PANU_ACL_DISCONNECT_SETTLE_TICKS,
						   bt_panu_acl_disconnect_timeout);
		return;
	}

	GBS_StartTimerProc(&panu->acl_timer, BT_PANU_ACL_DISCONNECT_POLL_TICKS,
					   bt_panu_acl_disconnect_timeout);
}

static void bt_panu_disconnect_acl(struct bt_panu *panu) {
	BT_BD_ADDR address;

	if (panu->acl_disconnect_requested)
		return;
	if (!panu->remote_device)
		return;

	panu->acl_disconnect_requested = true;
	panu->acl_disconnect_pending = true;
	panu->acl_disconnect_settling = false;
	bt_bdaddr_from_mac(&address, panu->config.peer_addr);
	BTA_vBtAclDisconnectReq(++panu->transaction_id, &address);
	GBS_StartTimerProc(&panu->acl_timer, BT_PANU_ACL_DISCONNECT_POLL_TICKS,
					   bt_panu_acl_disconnect_timeout);
}
#endif

/*
 * Closing: stop writing, keep draining received packets, and close only once the shared send
 * pool is back to its level at open - send buffers still queued when a channel closes are lost
 * until the phone reboots. Then wait for the close event, but not forever.
 */
static void bt_panu_close_step(GBSTMR *timer) {
	struct bt_panu *panu = container_of(timer, struct bt_panu, close_timer);

	if (panu->state != BT_PANU_STATE_STOPPING || panu->conn == BT_INVALID_HANDLE)
		return;

	bt_panu_drain_ring(panu, true);
	if (!panu->close_sent) {
		if (POOL_TX_FREE() < panu->pool_tx_base && ++panu->drain_polls < BT_PANU_DRAIN_MAX) {
			GBS_StartTimerProc(&panu->close_timer, BT_PANU_DRAIN_TICKS, bt_panu_close_step);
			return;
		}
		panu->close_sent = true;
#ifdef SGOLD
		CA_Conn_Close(panu->conn, panu->accessor);
#else
		BTA_Conn_CloseReq(panu->accessor, panu->conn, ++panu->transaction_id);
#endif
		GBS_StartTimerProc(&panu->close_timer, 216, bt_panu_close_step);
		return;
	}

	if (++panu->close_polls < BT_PANU_CLOSE_MAX) {
		GBS_StartTimerProc(&panu->close_timer, 216, bt_panu_close_step);
		return;
	}
	panu->conn = BT_INVALID_HANDLE;
	bt_panu_disconnect_acl(panu);
}

void bt_panu_init(struct bt_panu *panu, const struct bt_panu_config *config) {
	memset(panu, 0, sizeof(*panu));
	panu->config = *config;
	panu->accessor = BT_INVALID_HANDLE;
	panu->conn = BT_INVALID_HANDLE;
	panu->transaction_id = 0xb7000000u;
	panu->state = BT_PANU_STATE_IDLE;
}

int bt_panu_start(struct bt_panu *panu) {
	const BT_BD_ADDR *local_addr;

	if (panu->config.rx_mtu > sizeof(panu->rx_buf))
		return -1;

	bt_pool = RamBtBufferPool();
#ifdef SGOLD
	panu->accessor = CA_MR_New(panu->config.owner_cepid, (int)panu->config.message_id, "btpan", 1);
	if (!ca_handle_ok(panu->accessor)) {
		panu->accessor = BT_INVALID_HANDLE;
		return -1;
	}
	/* as JBT: all events of SAPI connections (open failed, opened, closed, parameters) */
	{
		static const uint8_t codes[] = {CA_SAPI_PARAMETERS, CA_SAPI_CLOSED, CA_SAPI_OPEN_FAILED,
										CA_SAPI_OPENED};
		unsigned i;

		for (i = 0; i < sizeof(codes); ++i)
			CA_MR_Subscribe(panu->accessor, codes[i], CA_SapiConnClass());
	}
	local_addr = (const BT_BD_ADDR *)bt_sg_local_addr();
#else
	panu->accessor = BTA_Accessor_New(panu->config.owner_cepid, panu->config.message_id, 0);
	if (panu->accessor == BT_INVALID_HANDLE || panu->accessor == 0xffffu)
		return -1;

	local_addr = BT_GetLocalAddress();
#endif
	if (!local_addr)
		return -1;
	bt_bdaddr_to_mac(local_addr, panu->local_addr);

	if (!memcmp(panu->config.peer_addr, zero_addr, sizeof(zero_addr)))
		return -1; /* no PC address */

	panu->state = BT_PANU_STATE_STARTING;
#ifdef SGOLD
	bt_panu_connect(panu); /* an MR has no start confirmation */
#endif
	return 0;
}

static void bt_panu_open_failed(struct bt_panu *panu) {
	panu->conn = BT_INVALID_HANDLE;
	panu->remote_device = NULL;
	if (panu->state != BT_PANU_STATE_STOPPING)
		panu->state = BT_PANU_STATE_IDLE;
	if (panu->config.cb && panu->config.cb->disconnected)
		panu->config.cb->disconnected(panu, panu->config.user_data);
}

static void bt_panu_closed(struct bt_panu *panu) {
	rx_wake_msg = 0;
	bt_panu_stop_timer(&panu->rx_timer);
	bt_panu_stop_timer(&panu->setup_timer);
	bt_panu_stop_timer(&panu->close_timer);
	panu->conn = BT_INVALID_HANDLE;
	bt_panu_disconnect_acl(panu);
	if (panu->state != BT_PANU_STATE_STOPPING)
		panu->state = BT_PANU_STATE_IDLE;
	if (panu->config.cb && panu->config.cb->disconnected)
		panu->config.cb->disconnected(panu, panu->config.user_data);
}

#ifdef SGOLD
/* {msg, submess = MR, data0 = object message}; every one goes back to the MR (MR_Done). */
void bt_panu_handle_bta_msg(struct bt_panu *panu, GBS_MSG *msg) {
	CA_OBJECT_MESSAGE *event = msg->data0;
	BT_HANDLE mr = (BT_HANDLE)msg->submess;

	if (!event)
		return;
	if (mr != panu->accessor) {
		CA_MR_Done(mr, event);
		return;
	}
	if (event->object_class == CA_MR_Class()) {
		if (event->object_message == CA_MR_STOPPED) {
			CA_Release(panu->accessor);
			panu->accessor = BT_INVALID_HANDLE; /* JBT hands the event back with FFFE too */
			panu->mr_stopping = false;
		}
	} else if (event->object_class == CA_SapiConnClass() && panu->conn != BT_INVALID_HANDLE &&
			   event->object_handle == panu->conn) { /* the class is shared with Java's */
		switch (event->object_message) {
			case CA_SAPI_OPEN_FAILED:
				bt_panu_open_failed(panu);
				break;
			case CA_SAPI_OPENED:
				if (event->result)
					bt_panu_open_failed(panu);
				else
					bt_panu_connection_opened(panu, event->object_handle, 0, event->param_1e);
				break;
			case CA_SAPI_CLOSED:
				bt_panu_closed(panu);
				break;
			default:
				break;
		}
	}
	CA_MR_Done(panu->accessor, event);
}
#else
void bt_panu_handle_bta_msg(struct bt_panu *panu, GBS_MSG *msg) {
	BT_ACCESSOR_GBS_PAYLOAD *payload = GBS_GetMessagePayload(msg);
	BT_ACCESSOR_MESSAGE *event;
	uint32_t application_message;
	uint8_t event_id;

	if (!payload)
		return;
	event = payload->object_message;
	if (payload->accessor != panu->accessor || !event)
		return;

	application_message = BTA_Accessor_MapObjectMessage(event);
	event_id = (uint8_t)application_message;

	switch (event_id) {
		case BT_EVENT_ACCESSOR_STARTED_CFM:
			if (event->result == 0) {
				bt_panu_connect(panu);
			} else {
				panu->state = BT_PANU_STATE_IDLE; /* accessor start failed */
			}
			break;
		case BT_EVENT_CONNECTION_OPEN_IND:
			bt_panu_connection_opened(panu, event->object_handle, event->result,
									  event->parameter.connection_open.connection_type);
			break;
		case BT_EVENT_CONNECTION_OPEN_FAILED_IND:
			bt_panu_open_failed(panu);
			break;
		case BT_EVENT_CONNECTION_L2CAP_PARAMETERS_CFM:
			panu->neg_tx_mtu = (uint16_t)event->parameter.l2cap_parameters.tx_mtu;
			panu->neg_rx_mtu = (uint16_t)event->parameter.l2cap_parameters.rx_mtu;
			break;
		case BT_EVENT_CONNECTION_CLOSED_IND:
		case BT_EVENT_CONNECTION_CLOSED_CFM:
			bt_panu_closed(panu);
			break;
		default:
			break;
	}
}
#endif

void bt_panu_stop(struct bt_panu *panu) {
	if (panu->state == BT_PANU_STATE_STOPPING)
		return;

	panu->state = BT_PANU_STATE_STOPPING;
	bt_panu_stop_timer(&panu->setup_timer);
	rx_wake_msg = 0;
	bt_panu_stop_timer(&panu->rx_timer);

	if (panu->conn == BT_INVALID_HANDLE || panu->accessor == BT_INVALID_HANDLE ||
		panu->accessor == 0xffffu)
		return;

	/* drain before the L2CAP close */
	panu->drain_polls = panu->close_polls = 0;
	panu->close_sent = false;
	GBS_StartTimerProc(&panu->close_timer, 1, bt_panu_close_step);
}

bool bt_panu_is_stopped(const struct bt_panu *panu) {
	return panu->state == BT_PANU_STATE_STOPPING && panu->conn == BT_INVALID_HANDLE &&
		   !panu->acl_disconnect_pending;
}

int bt_panu_deinit(struct bt_panu *panu) {
	if (panu->conn != BT_INVALID_HANDLE)
		return -1; /* refuse to orphan a live BT connection */

	bt_panu_stop_timer(&panu->setup_timer);
	rx_wake_msg = 0;
	bt_panu_stop_timer(&panu->rx_timer);
	bt_panu_stop_timer(&panu->close_timer);
	bt_panu_stop_timer(&panu->acl_timer);
	bt_panu_restore_app(panu);
#ifdef SGOLD
	/* MR_Stop, then the MR's own event releases it (bt_panu_handle_bta_msg); called again until
	 * then. ~5 s without the event: leave the MR, its messages die with our process. */
	if (panu->accessor != BT_INVALID_HANDLE) {
		if (!panu->mr_stopping) {
			panu->mr_stopping = true;
			panu->mr_polls = 0;
			CA_MR_Stop(panu->accessor);
			return 1;
		}
		if (++panu->mr_polls < 5)
			return 1;
		panu->accessor = BT_INVALID_HANDLE;
		panu->mr_stopping = false;
	}
#else
	if (panu->accessor != BT_INVALID_HANDLE && panu->accessor != 0xffffu) {
		BTA_Accessor_Delete(panu->accessor);
		panu->accessor = BT_INVALID_HANDLE;
	}
#endif
	panu->state = BT_PANU_STATE_IDLE;
	return 0;
}

/* debug screen lines (BT_PAN_DBG builds show them; cheap counters are always there) */
void bt_panu_dbg_line(struct bt_panu *panu, int k, char *b, unsigned n) {
	if (k == 0)
		snprintf(b, n, "st%d cs%d dp%u cp%u held%d ring%u", (int)panu->state, (int)panu->close_sent,
				 panu->drain_polls, panu->close_polls, (int)rx_held, (unsigned)(rx_head - rx_tail));
	else if (k == 1)
		snprintf(b, n, "cb%u dr%u pl%u tx%u/%u", (unsigned)rx_calls, (unsigned)rx_drained,
				 (unsigned)rx_polls, (unsigned)tx_ok, (unsigned)tx_fail);
	else
		snprintf(b, n, "pool tx%u/%u rx%u/%u", POOL_TX_FREE(), panu->pool_tx_base, POOL_RX_FREE(),
				 panu->pool_rx_base);
}
