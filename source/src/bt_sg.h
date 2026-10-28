#ifndef BT_PANU_BT_SG_H
#define BT_PANU_BT_SG_H

/*
 * S65 sw58 (SGOLD) Bluetooth: there is no BTA accessor layer (cabtaccessor) on this firmware.
 * JSR-82 (JBT, A0CB3784) talks to the CA objects directly, and so do we:
 *   message replicator (MR) = event sink, like the EL71 accessor: MR_New(cepid, msg) sends
 *     every subscribed object event as a GBS message {msg, submess = MR, data0 = object message};
 *     each one must be handed back with MR_Done(MR, data0).
 *   L2CAP_New(MR, remdev, sapi, psm, ...) = EL71 BTA_Conn_New_L2CAP without the transaction id
 *     (the same inner call; remdev from RemDev_New(address), freed right after, as there).
 *   Conn_Close(conn, MR) closes it; events of class SapiConnClass() tell what happened.
 * All of them come from the function library, entries 4CD-4DE (library-s65-sw58-*.vkp), and
 * lib_check() refuses to start without them.
 */

#include <stdint.h>

#include "lib.h"

typedef struct {
	uint8_t unknown_00[8];
	uint16_t object_handle;
	uint8_t object_class;
	uint8_t object_message;
	uint8_t unknown_0c[8];
	uint16_t result;
	uint8_t unknown_16[6];
	uint16_t param_1c;
	uint8_t param_1e;
	uint8_t unknown_1f;
} CA_OBJECT_MESSAGE;

/* SapiConnClass() events (JBT: E_CA_BTSAPICONNECTION_*) */
enum {
	CA_SAPI_OPEN_FAILED = 0, /* result = error */
	CA_SAPI_OPENED = 1,      /* result 0 = ok; +1C server (0 for a client), +1E protocol 1/2 */
	CA_SAPI_CLOSED = 2,
	CA_SAPI_PARAMETERS = 3,  /* L2CAP: +1C, +1E */
};
#define CA_MR_STOPPED 6 /* MR_Class(): after MR_Stop, then release the MR */

#define SG_FN(number, type) ((type)LIB_ENTRY(number))
#define CA_MR_New SG_FN(0x4CD, uint16_t (*)(int cepid, int msg, const char *file, int line))
#define CA_MR_Subscribe SG_FN(0x4CE, void (*)(uint16_t mr, int code, int class_id))
#define CA_MR_Stop SG_FN(0x4CF, void (*)(uint16_t mr))
#define CA_MR_Done SG_FN(0x4D0, void (*)(uint16_t mr, void *object_message))
#define CA_MR_Class SG_FN(0x4D1, uint8_t (*)(void))
#define CA_Release SG_FN(0x4D2, void (*)(uint16_t handle))
#define CA_SapiConnClass SG_FN(0x4D3, uint8_t (*)(void))
#define CA_RemDev_New SG_FN(0x4D4, void *(*)(const void *bd_addr))
#define CA_RemDev_Free SG_FN(0x4D5, void (*)(void *remdev))
#define CA_L2CAP_New SG_FN(0x4D6, uint16_t (*)(uint16_t mr, void *remdev, int sapi, int psm, \
	int master, int encrypt, int authenticate, int rx_mtu, int tx_mtu))
#define CA_Conn_Close SG_FN(0x4D7, int (*)(uint16_t conn, uint16_t owner))
/* Device list (cabtdevicelist, as the phone's own BT device list screen hmi/cabh_devlist_csm.c
 * and JBT nativeGetNoOfDevices): requests go to the local device's list object, the answer comes
 * back to the requesting MR as an event of CA_DevListClass() without a subscription.
 *   code 0: +0x14 result, +0x1D number of entries (0xFF = error); list 0 = paired devices
 *   code 1: +0x14 result, +0x1C RemDev copy (owned by the event): +0x24 address, +0x32 name[16] */
#define CA_DevList SG_FN(0x4D8, uint16_t (*)(uint16_t local_dev))
#define CA_DevListClass SG_FN(0x4D9, uint8_t (*)(void))
#define CA_DevList_CountReq SG_FN(0x4DA, void (*)(uint16_t list, uint16_t mr, int type))
#define CA_DevList_EntryReq SG_FN(0x4DB, void (*)(uint16_t list, uint16_t mr, int type, int index))
#define CA_DEVLIST_COUNT 0
#define CA_DEVLIST_ENTRY 1
#define CA_REMDEV_ADDR 0x24
#define CA_REMDEV_NAME 0x32
#define CA_REMDEV_NAME_LEN 16

/* BT_GetLocAddr as on EL71 (A0643E5A): LocalDev(Root(), 0), address pointer of that object */
#define CA_LocalRoot SG_FN(0x4DC, uint16_t (*)(void))
#define CA_LocalDev SG_FN(0x4DD, uint16_t (*)(uint16_t root, int zero))
#define CA_LocalAddr SG_FN(0x4DE, const uint8_t *(*)(uint16_t local_dev))

static inline int ca_handle_ok(uint16_t h) {
	return (h & 0xff00u) != 0xff00u;
}

/* bt-pan-mtu_S65v58.vkp: LDAEstReq accepts MTUs up to 0x6A0 ("movs r7,#0xD4; lsls r7,#3").
 * The one fixed S65v58 address left: it tests for that patch, it calls nothing. */
static inline int bt_sg_mtu_patched(void) {
	return *(const volatile uint32_t *)0xA0B3E2BC == 0x00FF27D4u;
}

static inline const uint8_t *bt_sg_local_addr(void) {
	uint16_t dev = CA_LocalDev(CA_LocalRoot(), 0);

	return ca_handle_ok(dev) ? CA_LocalAddr(dev) : 0;
}

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(CA_OBJECT_MESSAGE) == 0x20, "CA_OBJECT_MESSAGE size");
_Static_assert(__builtin_offsetof(CA_OBJECT_MESSAGE, result) == 0x14, "CA_OBJECT_MESSAGE.result");
_Static_assert(__builtin_offsetof(CA_OBJECT_MESSAGE, param_1c) == 0x1c, "CA_OBJECT_MESSAGE.param_1c");
#endif

#endif
