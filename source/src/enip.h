#ifndef BT_PANU_ENIP_H
#define BT_PANU_ENIP_H

#include <stddef.h>
#include <stdint.h>

#include "lib.h"

typedef struct ENIP_IP_DRIVER ENIP_IP_DRIVER;
typedef struct ENIP_IPH_INTERFACE ENIP_IPH_INTERFACE;
typedef struct ENIP_IP_MODULE ENIP_IP_MODULE;
typedef struct ENIP_MODULE ENIP_MODULE;
typedef struct ENIP_NSAPI_MODULE ENIP_NSAPI_MODULE;
typedef struct ENIP_PKT ENIP_PKT;
typedef struct ENIP_PORT ENIP_PORT;
typedef struct ENIP_SOCKET ENIP_SOCKET;
typedef struct ENIP_ZDLL_HEAD ENIP_ZDLL_HEAD;
typedef struct ENIP_ZDLL_ITEM ENIP_ZDLL_ITEM;

typedef void ENIP_MODULE_PROC(ENIP_MODULE *module);
typedef void ENIP_MODULE_NOTIFY_PROC(ENIP_MODULE *module, int event);
typedef void ENIP_PORT_PROC(ENIP_PORT *port, ENIP_PKT *packet);

struct ENIP_ZDLL_ITEM {
	void *prev;
	void *next;
};

struct ENIP_ZDLL_HEAD {
	void *first;
	void *last;
};

struct ENIP_PORT {
	ENIP_ZDLL_ITEM item;
	ENIP_MODULE *module;
	ENIP_PORT_PROC *callback;
	ENIP_ZDLL_HEAD packets_queue;
};

struct ENIP_MODULE {
	uint8_t flags;
	uint8_t padding_01[3];
	ENIP_MODULE_PROC *up_proc;
	ENIP_MODULE_PROC *down_proc;
	ENIP_MODULE_PROC *run_proc;
	ENIP_MODULE_NOTIFY_PROC *notify_proc;
	uint32_t need_run_postrun;
	ENIP_MODULE_PROC *post_run_proc;
	ENIP_MODULE_PROC *pkt_avail_proc;
	uint32_t is_run_posted;
	uint32_t is_running;
	char name[10];
	uint8_t padding_32[2];
	ENIP_ZDLL_HEAD timers;
	ENIP_ZDLL_HEAD ports;
	ENIP_PORT port;
};

struct ENIP_IP_DRIVER {
	ENIP_PORT *port_tx;
	uint32_t local_ip;
	uint32_t dns1;
	uint32_t dns2;
	uint8_t ip_version;
	uint8_t padding_11[3];
	uint32_t mtu;
	uint8_t header_sizes[2];
	uint8_t padding_1a[2];
	ENIP_MODULE *module;
	ENIP_PORT *port_rx;
	int8_t iface_bearer_id;
	uint8_t unknown_25;
	int16_t iface_index;
	uint32_t unknown_28;
};

struct ENIP_IPH_INTERFACE {
	ENIP_IP_DRIVER driver;
	uint32_t unknown_2c[6];
};

struct ENIP_IP_MODULE {
	ENIP_MODULE base;
	ENIP_PORT port;
	uint32_t unknown_74;
	ENIP_ZDLL_HEAD unknown_78;
	uint32_t unknown_80;
	uint32_t unknown_84;
};

struct ENIP_NSAPI_MODULE {
	ENIP_MODULE base;
	uint32_t unknown_5c;
	ENIP_ZDLL_HEAD interfaces;
	uint32_t unknown_68;
	uint32_t unknown_6c;
};

struct ENIP_PKT {
	ENIP_ZDLL_ITEM item;
	void *data;
	void *unknown_0c;
	void *buffer;
	uint32_t unknown_14;
	uint32_t size;
	uint32_t allocation_size;
	uint8_t owners[4];
	uint8_t owner_count;
	uint8_t unused_25[3];
};

typedef struct {
	uint16_t app_flags;
	uint16_t selector;
	uint8_t bearer_id;
	uint8_t unknown_05;
	uint16_t iface_index;
} LMAN_SOCKET_DESTINATION;

typedef void ENIP_MODULE_INIT_PROC(ENIP_MODULE *, char *, ENIP_MODULE_PROC *, ENIP_MODULE_PROC *,
	ENIP_MODULE_PROC *, ENIP_MODULE_NOTIFY_PROC *, ENIP_MODULE_PROC *, ENIP_MODULE_PROC *);
typedef uint8_t ENIP_MODULE_RUN_PROC(ENIP_MODULE *, uint32_t *);
typedef void ENIP_MODULE_DOWN_PROC(ENIP_MODULE *, uint32_t);
typedef void ENIP_MODULE_PORT_INIT_PROC(ENIP_PORT *, ENIP_MODULE *);
typedef void ENIP_PORT_SEND_PROC(ENIP_PORT *, ENIP_PKT *);
typedef ENIP_PKT *ENIP_PACKET_ALLOCATE_PROC(ENIP_MODULE *, int, uint32_t, int, int);
typedef void ENIP_PACKET_RELEASE_PROC(ENIP_PKT *);
typedef int ENIP_IP_REGISTER_DRIVER_PROC(ENIP_IP_DRIVER *, int);
typedef void ENIP_IP_UNREGISTER_DRIVER_PROC(ENIP_MODULE *);
typedef int16_t LMAN_CONNECTION_OPEN_PROC(uint8_t, uint8_t);
typedef void LMAN_CONNECTION_ATTACH_PROC(uint16_t, uint16_t, uint16_t);
typedef void LMAN_CONNECTION_CLOSE_PROC(uint16_t);
typedef void *ZDLL_REMOVE_HEAD_PROC(ENIP_ZDLL_HEAD *);
typedef ENIP_IPH_INTERFACE *ENIP_FIND_IFACE_PROC(ENIP_IP_DRIVER *);
typedef int ENIP_REASSEMBLY_PROC(void *, ENIP_PKT *, uint8_t *, uint32_t, uint32_t, uint32_t, int);
typedef ENIP_SOCKET *ENIP_SOCKET_CREATE_PROC(int, int, int, int *, uint32_t, uint32_t);
typedef int ENIP_PACKET_FOR_SOCKET_PROC(ENIP_SOCKET *, uint32_t, uint32_t, ENIP_PKT **, int);
typedef int ENIP_TCP_SEND_PROC(ENIP_SOCKET *, uint32_t, const void *, uint32_t, ENIP_PKT *, int);

#define enip_module_init ((ENIP_MODULE_INIT_PROC *)LIB_ENTRY(0x464))
#define enip_module_run ((ENIP_MODULE_RUN_PROC *)LIB_ENTRY(0x465))
#define enip_module_up ((int (*)(ENIP_MODULE *))LIB_ENTRY(0x466))
#define enip_module_down ((ENIP_MODULE_DOWN_PROC *)LIB_ENTRY(0x467))
#define enip_module_port_init ((ENIP_MODULE_PORT_INIT_PROC *)LIB_ENTRY(0x468))
#define enip_port_send ((ENIP_PORT_SEND_PROC *)LIB_ENTRY(0x469))
#define enip_packet_allocate ((ENIP_PACKET_ALLOCATE_PROC *)LIB_ENTRY(0x46A))
#define enip_packet_release ((ENIP_PACKET_RELEASE_PROC *)LIB_ENTRY(0x46B))
#define enip_ip_register_driver ((ENIP_IP_REGISTER_DRIVER_PROC *)LIB_ENTRY(0x46C))
#define enip_ip_unregister_driver ((ENIP_IP_UNREGISTER_DRIVER_PROC *)LIB_ENTRY(0x46D))
#define enip_iph_unregister_driver enip_ip_unregister_driver
#define enip_iph_find_IF_for_bearer ((ENIP_IPH_INTERFACE *(*)(uint32_t, uint16_t))LIB_ENTRY(0x46E))
#define lman_connection_open ((LMAN_CONNECTION_OPEN_PROC *)LIB_ENTRY(0x43E))
#define lman_connection_attach_IF ((LMAN_CONNECTION_ATTACH_PROC *)LIB_ENTRY(0x46F))
#define lman_connection_close ((LMAN_CONNECTION_CLOSE_PROC *)LIB_ENTRY(0x442))
#define zDLL_RemHead ((ZDLL_REMOVE_HEAD_PROC *)LIB_ENTRY(0x470))

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(ENIP_ZDLL_ITEM) == 8, "ENIP_ZDLL_ITEM size");
_Static_assert(sizeof(ENIP_ZDLL_HEAD) == 8, "ENIP_ZDLL_HEAD size");
_Static_assert(sizeof(ENIP_PORT) == 0x18, "ENIP_PORT size");
_Static_assert(offsetof(ENIP_PORT, callback) == 0x0c, "ENIP_PORT.callback offset");
_Static_assert(offsetof(ENIP_PORT, packets_queue) == 0x10, "ENIP_PORT.packets_queue offset");
_Static_assert(sizeof(ENIP_MODULE) == 0x5c, "ENIP_MODULE size");
_Static_assert(offsetof(ENIP_MODULE, port) == 0x44, "ENIP_MODULE.port offset");
_Static_assert(sizeof(ENIP_IP_DRIVER) == 0x2c, "ENIP_IP_DRIVER size");
_Static_assert(offsetof(ENIP_IP_DRIVER, mtu) == 0x14, "ENIP_IP_DRIVER.mtu offset");
_Static_assert(offsetof(ENIP_IP_DRIVER, port_rx) == 0x20, "ENIP_IP_DRIVER.port_rx offset");
_Static_assert(sizeof(ENIP_IPH_INTERFACE) == 0x44, "ENIP_IPH_INTERFACE size");
_Static_assert(sizeof(ENIP_IP_MODULE) == 0x88, "ENIP_IP_MODULE size");
_Static_assert(sizeof(ENIP_NSAPI_MODULE) == 0x70, "ENIP_NSAPI_MODULE size");
_Static_assert(sizeof(ENIP_PKT) == 0x28, "ENIP_PKT size");
_Static_assert(offsetof(ENIP_PKT, buffer) == 0x10, "ENIP_PKT.buffer offset");
_Static_assert(offsetof(ENIP_PKT, size) == 0x18, "ENIP_PKT.size offset");
_Static_assert(offsetof(ENIP_PKT, allocation_size) == 0x1c, "ENIP_PKT.allocation_size offset");
_Static_assert(offsetof(ENIP_PKT, owners) == 0x20, "ENIP_PKT.owners offset");
_Static_assert(offsetof(ENIP_PKT, owner_count) == 0x24, "ENIP_PKT.owner_count offset");
_Static_assert(sizeof(LMAN_SOCKET_DESTINATION) == 8, "LMAN_SOCKET_DESTINATION size");
#endif

#endif
