#ifndef BT_PANU_ENIP_H
#define BT_PANU_ENIP_H

#include <stddef.h>
#include <stdint.h>

#include "fw.h"

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

#define enip_module_init ((ENIP_MODULE_INIT_PROC *)FW_T(0xa09798aeu, 0xa096fb2au))
#define enip_module_run ((ENIP_MODULE_RUN_PROC *)FW_T(0xa0979900u, 0xa096fb7cu))
#define enip_module_post_run ((ENIP_MODULE_PROC *)FW_T(0xa0979958u, 0xa096fbd4u))
#define enip_module_up ((int (*)(ENIP_MODULE *))FW_T(0xa097997eu, 0xa096fbfau))
#define enip_module_down ((ENIP_MODULE_DOWN_PROC *)FW_T(0xa097999au, 0xa096fc16u))
#define enip_module_nsapi_post_init ((void (*)(ENIP_NSAPI_MODULE *))FW_T(0xa097aed4u, 0xa0971150u))
#define enip_module_port_init ((ENIP_MODULE_PORT_INIT_PROC *)FW_T(0xa097c1beu, 0xa097243au))
#define enip_port_send ((ENIP_PORT_SEND_PROC *)FW_T(0xa097c1e2u, 0xa097245eu))
#define enip_port_handle_pkt ((ENIP_PORT_SEND_PROC *)FW_T(0xa097c300u, 0xa097257cu))
#define enip_memory_allocate ((void *(*)(uint32_t, int))FW_T(0xa097b88eu, 0xa0971b0au))
#define enip_memory_release ((void (*)(void *))FW_T(0xa097b8d8u, 0xa0971b54u))
#define enip_packet_allocate ((ENIP_PACKET_ALLOCATE_PROC *)FW_T(0xa097b468u, 0xa09716e4u))
#define enip_packet_release ((ENIP_PACKET_RELEASE_PROC *)FW_T(0xa097b72au, 0xa09719a6u))
#define enip_packet_max_size ((int (*)(void))FW_T(0xa097b91eu, 0xa0971b9au))
#define enip_ip_init ((int (*)(void *))FW_T(0xa0978ff0u, 0xa096f26cu))
#define enip_ip_register_driver ((ENIP_IP_REGISTER_DRIVER_PROC *)FW_T(0xa097908eu, 0xa096f30au))
#define enip_ip_unregister_driver ((ENIP_IP_UNREGISTER_DRIVER_PROC *)FW_T(0xa0979132u, 0xa096f3aeu))
#define enip_iph_unregister_driver enip_ip_unregister_driver
#define enip_find_iface_by_driver ((ENIP_FIND_IFACE_PROC *)FW_T(0xa0a62f84u, 0xa0a591d8u))
#define enip_iph_calc_chk_and_port_send ((void (*)(ENIP_PORT **, ENIP_PKT *, uint32_t))FW_T(0xa0a65334u, 0xa0a5b588u))
#define enip_iph_compute_checksum ((uint16_t (*)(uint16_t, const void *, uint16_t))FW_T(0xa0b12ac4u, 0xa0b06444u))
#define enip_iph_find_IF_for_bearer ((ENIP_IPH_INTERFACE *(*)(uint32_t, uint16_t))FW_T(0xa0a654e2u, 0xa0a5b736u))
#define enip_iph_find_IF_for_socket ((ENIP_IPH_INTERFACE *(*)(ENIP_SOCKET *))FW_T(0xa0a65862u, 0xa0a5bab6u))
#define enip_iph_frame_read ((void (*)(ENIP_PKT *))FW_T(0xa0a6553eu, 0xa0a5b792u))
#define enip_iph_manage_reassembly ((ENIP_REASSEMBLY_PROC *)FW_T(0xa0a65a88u, 0xa0a5bcdcu))
#define enip_iph_packet_avail ((void (*)(void))FW_T(0xa0a65854u, 0xa0a5baa8u))
#define enip_iph_rheader_init ((void (*)(void *))FW_T(0xa0a65a50u, 0xa0a5bca4u))
#define enip_iph_run ((void (*)(ENIP_IP_MODULE *))FW_T(0xa0a6574cu, 0xa0a5b9a0u))
#define enip_iph_write ((int (*)(void *, ENIP_PKT *, uint32_t))FW_T(0xa0a65362u, 0xa0a5b5b6u))
#define enip_siph_crypt_frame_read ((int (*)(uint32_t))FW_T(0xa0a653b2u, 0xa0a5b606u))
#define enip_siph_find_IF ((ENIP_IPH_INTERFACE *(*)(int))FW_T(0xa0a65514u, 0xa0a5b768u))
#define enip_icmp_frame_read ((void (*)(uint32_t, ENIP_PKT *))FW_T(0xa0b12910u, 0xa0b06290u))
#define enip_udph_frame_read ((void (*)(void *, ENIP_PKT *))FW_T(0xa0a6a940u, 0xa0a60b94u))
#define enip_init_csd_iface ((int (*)(void *, ENIP_IP_DRIVER *))FW_T(0xa0a67966u, 0xa0a5dbbau))
#define enip_nsapi_init_iface ((int (*)(ENIP_MODULE *))FW_T(0xa097b128u, 0xa09713a4u))
#define enip_nsapi_init_module ((int (*)(ENIP_NSAPI_MODULE *))FW_T(0xa097b0d2u, 0xa097134eu))
#define ENIP_PDP_NET_UP_CNF ((void (*)(uint16_t, int))FW_T(0xa097a258u, 0xa09704d4u))
#define ENIP_PDP_NET_UP_REJ_IND ((void (*)(uint16_t, int))FW_T(0xa097a510u, 0xa097078cu))
#define ENIP_PDP_NET_DOWN_IND ((void (*)(uint16_t, int))FW_T(0xa097a5fau, 0xa0970876u))
#define ENIP_PDP_START_REQ ((void (*)(int, int))FW_T(0xa0979e24u, 0xa09700a0u))
#define ENIP_PDP_STOP_REQ ((void (*)(uint16_t, int))FW_T(0xa097a06eu, 0xa09702eau))
#define ENIP_PDP_START_CSD_INT_REQ ((void (*)(int))FW_T(0xa097a878u, 0xa0970af4u))
#define enip_sock_create ((int (*)(int, int, int, int *, uint32_t, uint16_t *))FW_T(0xa097e36eu, 0xa09745eau))
#define enip_socket_connect ((int (*)(int, int, int, void *, uint32_t))FW_T(0xa097f31au, 0xa0975596u))
#define enip_sockh_IF_init ((void (*)(ENIP_SOCKET *))FW_T(0xa097f5b8u, 0xa0975834u))
#define enip_sockh_create_internal ((ENIP_SOCKET_CREATE_PROC *)FW_T(0xa097f7d2u, 0xa0975a4eu))
#define enip_sockh_from_handle ((int (*)(int, ENIP_SOCKET **))FW_T(0xa097f9b6u, 0xa0975c32u))
#define enip_sockh_autobind_IF ((int (*)(ENIP_SOCKET *, int))FW_T(0xa097fa18u, 0xa0975c94u))
#define enip_sockh_packet_allocate ((ENIP_PACKET_FOR_SOCKET_PROC *)FW_T(0xa097fb42u, 0xa0975dbeu))
#define enip_sockh_dgram_connect ((int (*)(ENIP_SOCKET *, int, uint32_t, uint16_t))FW_T(0xa097fcdau, 0xa0975f56u))
#define enip_sockh_from_ipheader ((ENIP_SOCKET *(*)(int, const uint8_t *))FW_T(0xa097fd9cu, 0xa0976018u))
#define enip_sockh_prepare_send ((void *(*)(int, ENIP_SOCKET *, uint32_t))FW_T(0xa097931cu, 0xa096f598u))
#define enip_sockh_tcp_packet_allocate ((ENIP_PKT *(*)(ENIP_SOCKET *))FW_T(0xa0a68254u, 0xa0a5e4a8u))
#define enip_tcph_sock_init ((int (*)(ENIP_SOCKET *))FW_T(0xa0a6946cu, 0xa0a5f6c0u))
#define enip_tcph_sock_connect ((int (*)(ENIP_SOCKET *, int, uint32_t, uint16_t, uint32_t))FW_T(0xa0a69ef6u, 0xa0a6014au))
#define enip_tcph_send_segment ((ENIP_TCP_SEND_PROC *)FW_T(0xa0a68344u, 0xa0a5e598u))
#define enip_tcph_write_segment ((void (*)(ENIP_SOCKET *, ENIP_PKT *, int))FW_T(0xa0a6803eu, 0xa0a5e292u))
#define enip_tcph_change_sock_state ((void (*)(ENIP_SOCKET *, uint32_t))FW_T(0xa0a6876cu, 0xa0a5e9c0u))
#define enip_tcph_frame_read ((void (*)(ENIP_SOCKET *, ENIP_PKT *))FW_T(0xa0a695c6u, 0xa0a5f81au))
#define enip_tcph_run ((void (*)(void))FW_T(0xa0a6a090u, 0xa0a602e4u))
#define enip_tcph_get_SACK_option_from_list ((uint32_t (*)(ENIP_SOCKET *, uint32_t, uint8_t *))FW_T(0xa0a6828cu, 0xa0a5e4e0u))
#define enip_tcph_update_SACK_block_list ((void (*)(ENIP_SOCKET *))FW_T(0xa0a682f2u, 0xa0a5e546u))
#define enip_timer_init ((void (*)(void *, uint32_t))FW_T(0xa0a6a694u, 0xa0a608e8u))
#define enip_timer_cancel ((void (*)(void *))FW_T(0xa0a6a6a0u, 0xa0a608f4u))
#define enip_timer_start ((void (*)(ENIP_ZDLL_ITEM *, uint32_t))FW_T(0xa0a6a6b8u, 0xa0a6090cu))
#define LMAN_LINK_UP_CNF ((void (*)(void))FW_T(0xa097590cu, 0xa096bb88u))
#define lman_app_register ((int16_t (*)(uint16_t))FW_T(0xa0976adcu, 0xa096cd58u))
#define lman_app_free ((void (*)(uint16_t))FW_T(0xa097691cu, 0xa096cb98u))
#define lman_app_set_bearer ((int (*)(uint16_t, const void *, int))FW_T(0xa09773f6u, 0xa096d672u))
#define lman_app_set_connection ((void (*)(uint16_t, uint16_t, uint32_t))FW_T(0xa0976b70u, 0xa096cdecu))
#define lman_set_shit lman_app_set_connection
#define lman_connection_open ((LMAN_CONNECTION_OPEN_PROC *)FW_T(0xa0976970u, 0xa096cbecu))
#define lman_connection_attach_IF ((LMAN_CONNECTION_ATTACH_PROC *)FW_T(0xa09772ceu, 0xa096d54au))
#define lman_connection_close ((LMAN_CONNECTION_CLOSE_PROC *)FW_T(0xa0976874u, 0xa096caf0u))
#define lman_get_active_connection_id ((int16_t (*)(uint16_t, uint16_t))FW_T(0xa0976d6cu, 0xa096cfe8u))
#define lman_get_ap_id_for_cepid_2 ((int16_t (*)(uint16_t))FW_T(0xa097605cu, 0xa096c2d8u))
#define lman_GetBearerIdForCi ((uint8_t (*)(uint16_t))FW_T(0xa0976610u, 0xa096c88cu))
#define lman_get_socket_destination ((int (*)(uint16_t, LMAN_SOCKET_DESTINATION *))FW_T(0xa097667eu, 0xa096c8fau))
#define zDLL_Init ((void (*)(ENIP_ZDLL_HEAD *))FW_T(0xa0a6ac1cu, 0xa0a60e70u))
#define zDLL_RemHead ((ZDLL_REMOVE_HEAD_PROC *)FW_T(0xa0a6ac9eu, 0xa0a60ef2u))

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
