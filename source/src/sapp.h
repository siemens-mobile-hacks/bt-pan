#ifndef BT_PANU_SAPP_H
#define BT_PANU_SAPP_H

#include <stddef.h>
#include <stdint.h>

#include "fw.h"

typedef struct SApp_IO SApp_IO;

typedef uint32_t SAPP_READ_DATA_PROC(void *data, uint32_t length, uint32_t instance, uint32_t flags);
typedef uint32_t SAPP_SEND_DATA_PROC(const void *data, uint32_t length, uint32_t sdevice);
typedef void SAPP_DRIVER_CALLBACK_PROC(uint32_t sdevice);
typedef uint32_t SAPP_GET_VALUE_PROC(uint32_t sdevice);
typedef void SAPP_SET_VALUE_PROC(uint32_t value, uint32_t sdevice);
typedef uint32_t SAPP_GET_LIST_PROC(uint32_t *values, uint32_t capacity, uint32_t sdevice);
typedef void SAPP_SET_FLOW_CONTROL_PROC(uint32_t rx, uint32_t tx, uint32_t xon, uint32_t xoff,
	uint32_t sdevice);
typedef void SAPP_FLUSH_PROC(uint32_t direction, uint32_t sdevice);

typedef int VIO_OPEN_IO_SAPP_PROC(SApp_IO **io, uint32_t sdevice, void *connection_info);
typedef void VIO_REGISTER_COM_HANDLE_PROC(SApp_IO *io, void *connection_info);
typedef void VIO_CHANGE_COM_HANDLE_PROC(SApp_IO *io, void *connection_info);
typedef void VIO_CLOSE_IO_PROC(uint32_t instance);
typedef uint8_t VIO_GET_CONNECTED_SDEV_PROC(uint32_t instance, uint32_t *sdevice);

struct SApp_IO {
	SAPP_READ_DATA_PROC *ReadData;
	SAPP_DRIVER_CALLBACK_PROC *DrvReadDataCallback;
	SAPP_SEND_DATA_PROC *SendData;
	SAPP_DRIVER_CALLBACK_PROC *DrvWriteDataCallback;
	SAPP_GET_VALUE_PROC *GetV24Status;
	SAPP_SET_VALUE_PROC *SetV24Status;
	SAPP_DRIVER_CALLBACK_PROC *unknown_18;
	SAPP_SET_VALUE_PROC *SetEscape;
	SAPP_DRIVER_CALLBACK_PROC *unknown_20;
	SAPP_SET_FLOW_CONTROL_PROC *SetFlowControl;
	SAPP_SET_VALUE_PROC *unknown_28;
	SAPP_GET_VALUE_PROC *GetBaudRate;
	SAPP_SET_VALUE_PROC *SetBaudRate;
	SAPP_GET_LIST_PROC *GetBaudrateList;
	SAPP_FLUSH_PROC *Flush;
	uint8_t connected_sdevice;
	uint8_t padding_3d[3];
	uint32_t SDeviceInstanz;
	uint8_t handler_id;
	uint8_t padding_45[3];
	uint32_t id;
};

typedef struct {
	VIO_OPEN_IO_SAPP_PROC *OpenIOSApp;
	VIO_REGISTER_COM_HANDLE_PROC *RegisterComHandle;
	VIO_CHANGE_COM_HANDLE_PROC *ChangeComHandle;
	VIO_CLOSE_IO_PROC *CloseIO;
	VIO_GET_CONNECTED_SDEV_PROC *GetConnectedSDev;
} VioHandlerMethods;

typedef void SAPP_INIT_IO_PROC(SApp_IO *io);
typedef void SAPP_REGISTER_PROC(int handler_id, VioHandlerMethods *methods);
typedef int CONN_SDEV_SAPP_PROC(uint32_t sdevice, uint32_t handler_id, void *connection_info);
typedef int CHANGE_CONN_SAPP_PROC(uint32_t handler_id, uint32_t instance, uint32_t new_handler_id,
	void *connection_info);
typedef int SERIAL_CAN_OPEN_SAPP_PROC(uint32_t handler_id, uint32_t instance);
typedef void CA_SAS_SAPP_INIT_PROC(void);

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(SApp_IO) == 0x4c, "SApp_IO size");
_Static_assert(offsetof(SApp_IO, ReadData) == 0x00, "SApp_IO.ReadData offset");
_Static_assert(offsetof(SApp_IO, DrvReadDataCallback) == 0x04, "SApp_IO.DrvReadDataCallback offset");
_Static_assert(offsetof(SApp_IO, SendData) == 0x08, "SApp_IO.SendData offset");
_Static_assert(offsetof(SApp_IO, DrvWriteDataCallback) == 0x0c, "SApp_IO.DrvWriteDataCallback offset");
_Static_assert(offsetof(SApp_IO, GetV24Status) == 0x10, "SApp_IO.GetV24Status offset");
_Static_assert(offsetof(SApp_IO, SetV24Status) == 0x14, "SApp_IO.SetV24Status offset");
_Static_assert(offsetof(SApp_IO, SetEscape) == 0x1c, "SApp_IO.SetEscape offset");
_Static_assert(offsetof(SApp_IO, SetFlowControl) == 0x24, "SApp_IO.SetFlowControl offset");
_Static_assert(offsetof(SApp_IO, GetBaudRate) == 0x2c, "SApp_IO.GetBaudRate offset");
_Static_assert(offsetof(SApp_IO, SetBaudRate) == 0x30, "SApp_IO.SetBaudRate offset");
_Static_assert(offsetof(SApp_IO, GetBaudrateList) == 0x34, "SApp_IO.GetBaudrateList offset");
_Static_assert(offsetof(SApp_IO, Flush) == 0x38, "SApp_IO.Flush offset");
_Static_assert(offsetof(SApp_IO, connected_sdevice) == 0x3c, "SApp_IO.connected_sdevice offset");
_Static_assert(offsetof(SApp_IO, SDeviceInstanz) == 0x40, "SApp_IO.SDeviceInstanz offset");
_Static_assert(offsetof(SApp_IO, handler_id) == 0x44, "SApp_IO.handler_id offset");
_Static_assert(offsetof(SApp_IO, id) == 0x48, "SApp_IO.id offset");
_Static_assert(sizeof(VioHandlerMethods) == 0x14, "VioHandlerMethods size");
_Static_assert(offsetof(VioHandlerMethods, OpenIOSApp) == 0x00, "VioHandlerMethods.OpenIOSApp offset");
_Static_assert(offsetof(VioHandlerMethods, CloseIO) == 0x0c, "VioHandlerMethods.CloseIO offset");
_Static_assert(offsetof(VioHandlerMethods, GetConnectedSDev) == 0x10, "VioHandlerMethods.GetConnectedSDev offset");
#endif

#define SApp_Init_IO ((SAPP_INIT_IO_PROC *)FW_T(0xa04c6ce8u, 0xa04c0920u))
#define SApp_Register ((SAPP_REGISTER_PROC *)FW_T(0xa04c6f34u, 0xa04c0b6cu))
#define ConnSDevSApp ((CONN_SDEV_SAPP_PROC *)FW_T(0xa04c700cu, 0xa04c0c44u))
#define CHConnSApp ((CHANGE_CONN_SAPP_PROC *)FW_T(0xa04c734cu, 0xa04c0f84u))
#define Serial_CanOpenSApp ((SERIAL_CAN_OPEN_SAPP_PROC *)FW_T(0xa04c786cu, 0xa04c14a4u))
#define Ca_SasSApp_Init ((CA_SAS_SAPP_INIT_PROC *)FW_T(0xa065bb72u, 0xa0659f42u))

#endif
