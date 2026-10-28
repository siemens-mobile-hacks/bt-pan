#ifndef BT_PANU_BT_H
#define BT_PANU_BT_H

#include <stddef.h>
#include <stdint.h>

#include "lib.h"
#include "sapp.h"

#define BT_PSM_BNEP 0x000fu
#define BT_BNEP_MIN_MTU 1691u
#define BT_SAPI_TYPE_L2CAP 0x17u
#define BT_INVALID_HANDLE 0xfffeu
#define JSAPI_CONNECTION_TYPE_L2CAP 1u
#define JSAPI_HANDLE_COUNT 5u
#define JSAPI_RX_RING_CAPACITY 0x800u
#define BTLP_CONNECTION_COUNT 9u
#define BTLP_FIRST_SDEVICE 0x0du
#define L2CAP_FIRST_DYNAMIC_CID 0x40u

typedef uint16_t BT_HANDLE;
typedef uint32_t BT_TRANSACTION_ID;

typedef struct BT_REMOTE_DEVICE_INFO BT_REMOTE_DEVICE_INFO;
typedef struct BT_DEVICE_CLASS BT_DEVICE_CLASS;

typedef struct {
	uint8_t octet[6];
} BT_BD_ADDR;

typedef struct {
	uint8_t unknown_00[8];
	BT_HANDLE object_handle;
	uint8_t unknown_0a[2];
	BT_TRANSACTION_ID transaction_id;
	uint8_t object_class;
	uint8_t object_message;
	uint8_t unknown_12[6];
	uint16_t result;
	uint8_t result_detail;
	uint8_t unknown_1b[5];
	union {
		uint32_t value;
		struct {
			uint16_t remote_device;
			uint8_t connection_type;
			uint8_t unknown_03;
		} connection_open;
		struct {
			uint16_t tx_mtu;
			uint16_t rx_mtu;
		} l2cap_parameters;
	} parameter;
} BT_ACCESSOR_MESSAGE;

typedef struct {
	BT_ACCESSOR_MESSAGE *object_message;
	BT_HANDLE accessor;
	uint16_t unknown_06;
} BT_ACCESSOR_GBS_PAYLOAD;

typedef struct {
	uint8_t *data;
	uint32_t count;
	uint32_t write_offset;
	uint32_t read_offset;
} JSApi_RingBuffer_t;

typedef struct {
	BT_HANDLE native_handle;
	uint8_t connection_type;
	uint8_t state;
	uint32_t tx_ready;
	uint32_t rx_paused;
	SApp_IO *io;
	uint32_t baudrate;
	uint32_t options;
	uint8_t xon;
	uint8_t xoff;
	uint8_t padding_1a[2];
	uint32_t v24_status;
	uint8_t connected_sdevice;
	uint8_t padding_21[3];
	JSApi_RingBuffer_t rx_ring;
	SApp_IO sapp_io;
	uint32_t flags;
} JSApi_Handle_t;

typedef struct {
	uint32_t app_handle;
	uint16_t cid;
	uint8_t profile;
	uint8_t lda_instance;
} BTLP_CONNECTION;

typedef struct {
	uint16_t cid;
	uint8_t unknown_02[10];
	uint32_t rx_state;
	uint8_t unknown_10[8];
} L2CAP_CHANNEL_STATE;

typedef struct {
	uint16_t primitive;
	uint8_t length;
	uint8_t unknown_03;
	uint16_t cid;
	uint8_t data[10];
} L2CA_READ_DATA_REQ;

enum BT_ACCESSOR_EVENT {
	BT_EVENT_ACCESSOR_STARTED_CFM = 0,
	BT_EVENT_CONNECTION_CLOSED_IND = 7,
	BT_EVENT_CONNECTION_CLOSED_CFM = 8,
	BT_EVENT_CONNECTION_OPEN_IND = 9,
	BT_EVENT_CONNECTION_OPEN_FAILED_IND = 10,
	BT_EVENT_CONNECTION_L2CAP_PARAMETERS_CFM = 13,
	BT_EVENT_SERVER_CREATED_CFM = 0x13,
	BT_EVENT_SERVER_STARTED_CFM = 0x14,
	BT_EVENT_SERVER_ACCEPT_CFM = 0x16,
	BT_EVENT_SERVER_STOPPED_CFM = 0x17,
};

#define L2CA_READ_DATA_REQ_PRIMITIVE 0x00e3u
#define L2CA_READ_DATA_REQ_LENGTH 0x10u

typedef BT_HANDLE BTA_ACCESSOR_NEW_PROC(uint16_t, uint32_t, uint32_t);
typedef uint32_t BTA_MAP_MESSAGE_PROC(BT_ACCESSOR_MESSAGE *);
typedef void BTA_ACCESSOR_DELETE_PROC(BT_HANDLE);
typedef const BT_BD_ADDR *BT_GET_LOCAL_ADDRESS_PROC(void);
typedef BT_HANDLE BTA_NEW_L2CAP_PROC(uint32_t, const BT_BD_ADDR *, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void BTA_CONN_REQUEST_PROC(BT_HANDLE, BT_HANDLE, BT_TRANSACTION_ID);
typedef uint32_t BTA_GET_SAPI_PROC(BT_HANDLE, BT_HANDLE);
typedef BT_REMOTE_DEVICE_INFO *BTA_GET_REMOTE_INFO_PROC(BT_HANDLE, BT_HANDLE);
typedef uint32_t BTA_GET_REMOTE_STATUS_PROC(BT_HANDLE, BT_REMOTE_DEVICE_INFO *, uint32_t);
typedef void BTA_ACL_DISCONNECT_PROC(BT_TRANSACTION_ID, const BT_BD_ADDR *);
typedef void BTA_REQUEST_2_PROC(BT_HANDLE, BT_TRANSACTION_ID);
typedef void BTA_REQUEST_3_PROC(BT_HANDLE, uint32_t, BT_TRANSACTION_ID);
typedef void BTA_DISCOVERY_COD_PROC(BT_HANDLE, uint32_t, uint32_t, BT_TRANSACTION_ID);
typedef const BT_BD_ADDR *BTA_GET_REMOTE_ADDRESS_PROC(BT_HANDLE, BT_REMOTE_DEVICE_INFO *);
typedef const char *BTA_GET_REMOTE_NAME_PROC(BT_HANDLE, BT_REMOTE_DEVICE_INFO *);
typedef int BTA_SET_REMOTE_NAME_PROC(BT_HANDLE, BT_REMOTE_DEVICE_INFO *, const char *);
typedef BT_DEVICE_CLASS *BTA_GET_DEVICE_CLASS_PROC(BT_HANDLE, BT_REMOTE_DEVICE_INFO *);
typedef BT_HANDLE BTA_NEW_RFCOMM_PROC(uint32_t, const BT_BD_ADDR *, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t);
typedef BT_HANDLE BTA_SERVER_NEW_PROC(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t, uint32_t);
typedef void BTA_SERVER_START_PROC(BT_HANDLE, BT_HANDLE, uint32_t, uint32_t, BT_TRANSACTION_ID);
typedef void BTA_SERVER_ACCEPT_PROC(BT_HANDLE, BT_HANDLE, int, BT_TRANSACTION_ID);
typedef int BTA_SERVICE_SEARCH_PROC(BT_HANDLE, uint32_t, uint32_t, void *, int, BT_TRANSACTION_ID);
typedef void BTLP_L2CAP_CONNECT_PROC(uint32_t, const BT_BD_ADDR *, uint32_t, uint32_t, void *);
typedef uint32_t JSAPI_CURRENT_HANDLE_PROC(BT_HANDLE);
typedef void JSAPI_INIT_HANDLES_PROC(void);
typedef int JSAPI_REGISTER_HANDLE_PROC(BT_HANDLE, uint32_t, uint32_t);
typedef uint8_t JSAPI_GET_STATE_PROC(BT_HANDLE);
typedef int JSAPI_READ_PROC(BT_HANDLE, void *, uint32_t);
typedef int JSAPI_AVAILABLE_PROC(BT_HANDLE);
typedef int JSAPI_WRITE_PROC(BT_HANDLE, const void *, uint32_t);
typedef int JSAPI_CLEAR_HANDLE_PROC(BT_HANDLE);
typedef void L2CA_READ_DATA_PROC(const L2CA_READ_DATA_REQ *);

#define BTA_Accessor_New ((BTA_ACCESSOR_NEW_PROC *)LIB_ENTRY(0x420))
#define GBS_GetMessagePayload ((void *(*)(void *))LIB_ENTRY(0x423))
#define BTA_Accessor_MapObjectMessage ((BTA_MAP_MESSAGE_PROC *)LIB_ENTRY(0x422))
#define BTA_Accessor_Delete ((BTA_ACCESSOR_DELETE_PROC *)LIB_ENTRY(0x421))
#define BT_GetLocalAddress ((BT_GET_LOCAL_ADDRESS_PROC *)LIB_ENTRY(0x42D))
#define BTA_RemBtDeviceInfo_GetAddress ((BTA_GET_REMOTE_ADDRESS_PROC *)LIB_ENTRY(0x44B))
#define BTA_RemBtDeviceInfo_GetName ((BTA_GET_REMOTE_NAME_PROC *)LIB_ENTRY(0x44C))
#define BTA_Conn_New_L2CAP ((BTA_NEW_L2CAP_PROC *)LIB_ENTRY(0x433))
#define BTA_Conn_CloseReq ((BTA_CONN_REQUEST_PROC *)LIB_ENTRY(0x42C))
#define BTA_Conn_GetL2CAPParamReq ((BTA_CONN_REQUEST_PROC *)LIB_ENTRY(0x45F))
#define BTA_Conn_GetRemDevInfo ((BTA_GET_REMOTE_INFO_PROC *)LIB_ENTRY(0x460))
#define BTA_RemBtDeviceInfo_GetStatus ((BTA_GET_REMOTE_STATUS_PROC *)LIB_ENTRY(0x461))
#define BTA_vBtAclDisconnectReq ((BTA_ACL_DISCONNECT_PROC *)LIB_ENTRY(0x462))

#ifdef SGOLD
#include "bt_sg.h"

/* swilib/bluetooth.h declares these for ELKA only; S65 has the same SDev layout (SDev_InitEndpoint
 * A123AC20 fills fn[15], type +3C, inst +40, app +44, index +48; SDEV_APP called as on EL71). */
typedef struct SDEV {
	void *fn[15];
	uint8_t type;
	int inst;
	uint8_t app;
	int index;
} SDEV;

typedef struct SDEV_APP {
	int (*open)(SDEV **ep, int dev_type, const uint8_t *info);
	void (*reg)(SDEV *dev, const void *info);
	void *unk;
	void (*close)(int index);
	int (*get_dev)(int index, int *inst);
} SDEV_APP;

#define SDev_InitEndpoint ((void (*)(SDEV *))LIB_ENTRY(0x42E))
#define RamSDevApps() ((void **)LIB_ENTRY(0x8431))
#define RamBtBufferPool() ((uint16_t *)LIB_ENTRY(0x8432))
#endif

static inline void bt_bdaddr_from_mac(BT_BD_ADDR *address, const uint8_t mac[6]) {
	unsigned int i;

	for (i = 0; i < 6; ++i)
		address->octet[i] = mac[5 - i];
}

static inline void bt_bdaddr_to_mac(const BT_BD_ADDR *address, uint8_t mac[6]) {
	unsigned int i;

	for (i = 0; i < 6; ++i)
		mac[i] = address->octet[5 - i];
}

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(BT_BD_ADDR) == 6, "BT_BD_ADDR size");
_Static_assert(sizeof(BT_ACCESSOR_MESSAGE) == 0x24, "BT_ACCESSOR_MESSAGE size");
_Static_assert(offsetof(BT_ACCESSOR_MESSAGE, object_handle) == 0x08, "BT_ACCESSOR_MESSAGE.object_handle offset");
_Static_assert(offsetof(BT_ACCESSOR_MESSAGE, transaction_id) == 0x0c, "BT_ACCESSOR_MESSAGE.transaction_id offset");
_Static_assert(offsetof(BT_ACCESSOR_MESSAGE, result) == 0x18, "BT_ACCESSOR_MESSAGE.result offset");
_Static_assert(offsetof(BT_ACCESSOR_MESSAGE, parameter) == 0x20, "BT_ACCESSOR_MESSAGE.parameter offset");
_Static_assert(sizeof(BT_ACCESSOR_GBS_PAYLOAD) == 8, "BT_ACCESSOR_GBS_PAYLOAD size");
_Static_assert(sizeof(JSApi_RingBuffer_t) == 0x10, "JSApi_RingBuffer_t size");
_Static_assert(sizeof(JSApi_Handle_t) == 0x84, "JSApi_Handle_t size");
_Static_assert(offsetof(JSApi_Handle_t, io) == 0x0c, "JSApi_Handle_t.io offset");
_Static_assert(offsetof(JSApi_Handle_t, rx_ring) == 0x24, "JSApi_Handle_t.rx_ring offset");
_Static_assert(offsetof(JSApi_Handle_t, sapp_io) == 0x34, "JSApi_Handle_t.sapp_io offset");
_Static_assert(sizeof(BTLP_CONNECTION) == 8, "BTLP_CONNECTION size");
_Static_assert(offsetof(BTLP_CONNECTION, cid) == 4, "BTLP_CONNECTION.cid offset");
_Static_assert(sizeof(L2CAP_CHANNEL_STATE) == 0x18, "L2CAP_CHANNEL_STATE size");
_Static_assert(offsetof(L2CAP_CHANNEL_STATE, rx_state) == 0x0c, "L2CAP_CHANNEL_STATE.rx_state offset");
_Static_assert(sizeof(L2CA_READ_DATA_REQ) == 0x10, "L2CA_READ_DATA_REQ size");
#endif

#endif
