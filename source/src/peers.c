#include "peers.h"

#include <stdio.h>
#include <string.h>

#include "bt.h"
#include "usart.h"

struct peer peers[PEERS_MAX];
volatile int peer_count;
volatile bool peers_busy;

#ifdef SGOLD
/* S65: the CA device list (src/bt_sg.h), read entry by entry with an MR of our own. */
#include "bt_sg.h"
#include "trace.h"

#define PEERS_TIMEOUT_TICKS (5 * 216)

static BT_HANDLE mr = BT_INVALID_HANDLE, old_mr = BT_INVALID_HANDLE;
static uint16_t list;
static int total, next;
static GBSTMR timer;

static void peers_timeout(GBSTMR *t) {
	(void)t;
	usart_puts("Peers: device list timed out\r\n");
	trace("Peers: timeout");
	peers_stop();
}

void peers_stop(void) {
	if (IsTimerProc(&timer))
		GBS_StopTimer(&timer);
	if (mr != BT_INVALID_HANDLE) {
		CA_MR_Stop(mr); /* released on its "stopped" event */
		old_mr = mr;
		mr = BT_INVALID_HANDLE;
	}
	peers_busy = false;
}

static void peers_next(void) {
	if (next >= total || peer_count >= PEERS_MAX) {
		peers_stop();
		return;
	}
	CA_DevList_EntryReq(list, mr, 0, next);
	GBS_StartTimerProc(&timer, PEERS_TIMEOUT_TICKS, peers_timeout);
}

void peers_start(uint16_t owner_cepid, uint32_t message_id) {
	uint16_t dev = CA_LocalDev(CA_LocalRoot(), 0);

	peers_stop();
	peer_count = 0;
	if (!ca_handle_ok(dev) || !ca_handle_ok(list = CA_DevList(dev))) {
		usart_puts("Peers: no device list\r\n");
		trace("Peers: no device list");
		return;
	}
	mr = CA_MR_New(owner_cepid, (int)message_id, "btpan-peers", 1);
	if (!ca_handle_ok(mr)) {
		mr = BT_INVALID_HANDLE;
		usart_puts("Peers: no MR\r\n");
		trace("Peers: no MR");
		return;
	}
	peers_busy = true;
	CA_DevList_CountReq(list, mr, 0);
	GBS_StartTimerProc(&timer, PEERS_TIMEOUT_TICKS, peers_timeout);
}

static void peers_entry(const uint8_t *remdev) {
	struct peer *p = &peers[peer_count];
	unsigned i;

	bt_bdaddr_to_mac((const BT_BD_ADDR *)(remdev + CA_REMDEV_ADDR), p->mac);
	for (i = 0; i < CA_REMDEV_NAME_LEN && remdev[CA_REMDEV_NAME + i]; ++i)
		p->name[i] = (char)remdev[CA_REMDEV_NAME + i];
	p->name[i] = 0;
	++peer_count;
}

/* {msg, submess = MR, data0 = event}; every event goes back with MR_Done */
void peers_handle_bta_msg(GBS_MSG *msg) {
	CA_OBJECT_MESSAGE *event = msg->data0;
	BT_HANDLE from = (BT_HANDLE)msg->submess;
	const uint8_t *e = (const uint8_t *)event;
#if BT_PAN_TRACE
	char t[64];
#endif

	if (!event)
		return;
#if BT_PAN_TRACE
	sprintf(t, "Peers: ev class=%X code=%X result=%X 1c=%02X%02X%02X%02X", event->object_class,
			event->object_message, event->result, e[0x1c], e[0x1d], e[0x1e], e[0x1f]);
	trace(t);
#endif
	if (event->object_class == CA_MR_Class() && event->object_message == CA_MR_STOPPED) {
		CA_Release(from);
		if (from == old_mr)
			old_mr = BT_INVALID_HANDLE;
		CA_MR_Done(BT_INVALID_HANDLE, event); /* as JBT after the release */
		return;
	}
	if (from == mr && event->object_class == CA_DevListClass()) {
		if (IsTimerProc(&timer))
			GBS_StopTimer(&timer);
		if (event->object_message == CA_DEVLIST_COUNT) {
			total = event->result || e[0x1d] == 0xff ? 0 : e[0x1d];
			next = 0;
			peers_next();
		} else if (event->object_message == CA_DEVLIST_ENTRY) {
			uint32_t remdev;

			memcpy(&remdev, e + 0x1c, sizeof(remdev));
			if (!event->result && remdev >= 0xA8000000u && remdev < 0xAC000000u)
				peers_entry((const uint8_t *)(uintptr_t)remdev);
			++next;
			peers_next();
		}
	}
	CA_MR_Done(from, event);
}
#else
#define PEERS_EVENT_ENTRY 0x18 /* BTA_DeviceListView_GetEntryReq answer */
#define PEERS_TIMEOUT_TICKS (5 * 216)

static BT_HANDLE oh = BT_INVALID_HANDLE;
static int list = -1, total, next, tid = 0x5e000000;
static bool pending;
static GBSTMR timer;

static void peers_next(void);

static void peers_timeout(GBSTMR *t) {
	(void)t;
	usart_puts("Peers: device list timed out\r\n");
	peers_stop();
}

void peers_stop(void) {
	if (IsTimerProc(&timer))
		GBS_StopTimer(&timer);
	if (oh != BT_INVALID_HANDLE) {
		if (list >= 0)
			BTA_DeviceListView_StopList(oh, list);
		BTA_Accessor_Delete(oh);
	}
	oh = BT_INVALID_HANDLE;
	list = -1;
	pending = false;
	peers_busy = false;
}

static void peers_next(void) {
	if (next >= total || peer_count >= PEERS_MAX) {
		peers_stop();
		return;
	}
	pending = true;
	BTA_DeviceListView_GetEntryReq(oh, list, next, ++tid);
	GBS_StartTimerProc(&timer, PEERS_TIMEOUT_TICKS, peers_timeout);
}

void peers_start(uint16_t owner_cepid, uint32_t message_id) {
	peers_stop();
	peer_count = 0;
	oh = BTA_Accessor_New(owner_cepid, message_id, 0);
	if (oh == BT_INVALID_HANDLE || oh == 0xffffu) {
		oh = BT_INVALID_HANDLE;
		usart_puts("Peers: no accessor\r\n");
		return;
	}
	peers_busy = true;
	GBS_StartTimerProc(&timer, PEERS_TIMEOUT_TICKS, peers_timeout);
}

void peers_handle_bta_msg(GBS_MSG *msg) {
	BT_ACCESSOR_GBS_PAYLOAD *payload = GBS_GetMessagePayload(msg);
	BT_ACCESSOR_MESSAGE *event;
	unsigned id;

	if (!payload || !payload->object_message || oh == BT_INVALID_HANDLE || payload->accessor != oh)
		return;
	event = payload->object_message;
	id = BTA_Accessor_MapObjectMessage(event) & 0xffu;

	if (id == BT_EVENT_ACCESSOR_STARTED_CFM && list < 0) {
		if (event->result) {
			peers_stop();
			return;
		}
		list = BTA_DeviceListView_GetList(oh, 0) & 0xffff;
		if (list == 0xffff) {
			list = -1;
			peers_stop();
			return;
		}
		total = BTA_DeviceListView_GetNumberOfEntries(oh, list);
		if (total < 0)
			total = 0;
		next = 0;
		peers_next();
		return;
	}
	if (id != PEERS_EVENT_ENTRY || !pending || event->transaction_id != (BT_TRANSACTION_ID)tid)
		return;
	pending = false;
	if (!event->result) {
		BT_REMOTE_DEVICE_INFO *remote;

		memcpy(&remote, (char *)event + 0x24, sizeof(remote));
		if (remote) {
			const BT_BD_ADDR *address = BTA_RemBtDeviceInfo_GetAddress(oh, remote);

			if (address) {
				struct peer *p = &peers[peer_count];
				const char *name = BTA_RemBtDeviceInfo_GetName(oh, remote);

				bt_bdaddr_to_mac(address, p->mac);
				strncpy(p->name, name ? name : "", sizeof(p->name) - 1);
				p->name[sizeof(p->name) - 1] = 0;
				++peer_count;
			}
		}
	}
	++next;
	peers_next();
}
#endif
