#include "peers.h"

#include <stdio.h>
#include <string.h>

#include "bt.h"
#include "usart.h"

#define PEERS_EVENT_ENTRY 0x18 /* BTA_DeviceListView_GetEntryReq answer */
#define PEERS_TIMEOUT_TICKS (5 * 216)

struct peer peers[PEERS_MAX];
volatile int peer_count;
volatile bool peers_busy;

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
