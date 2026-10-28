#include <swilib.h>
#include <cfg_items.h>
#include <pmb887x.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "build_id.h"
#include "lib.h"
#include "bt_panu.h"
#include "bt_pan_netif.h"
#include "config.h"
#include "icmp_probe.h"
#include "pan_lman.h"
#include "ping.h"
#include "trace.h"
#include "peers.h"
#include "speedtest.h"
#include "usart.h"

#define GetProcGroup ((PGROUP * (*)(int)) LIB_ENTRY(0x463)) /* GBS_GetProcGroup */

/* A private process group keeps BTA messages away from the ELF helper queue. */
#define BT_PANU_PROCESS_GROUP 0x1au
#define BT_PANU_PROCESS_CEPID ((BT_PANU_PROCESS_GROUP << 8) | 0x00u)

/* Private application message IDs chosen by this PoC, not firmware ABI. */
#define BT_PANU_MESSAGE_INIT 0x6b4du
#define BT_PANU_MESSAGE_BTA 0x6b4eu
#define BT_PANU_MESSAGE_STOP 0x6b4fu /* relaunch tells the running instance to shut down */
#define BT_PANU_MESSAGE_PEERS_BTA 0x6b50u    /* device-list accessor events */
#define BT_PANU_MESSAGE_PEERS_START 0x6b51u
#define BT_PANU_MESSAGE_PEERS_STOP 0x6b52u
#define BT_PANU_MESSAGE_DIRECT_START 0x6b53u /* direct speed test runs in this process */
#define BT_PANU_MESSAGE_DIRECT_STOP 0x6b54u
#define BT_PANU_MESSAGE_PING_START 0x6b55u /* data0 = payload size; ping runs in this process */
#define BT_PANU_MESSAGE_PING_STOP 0x6b56u

static struct bt_pan_netif netif;

static struct bt_panu_config panu_config = {
	.owner_cepid = BT_PANU_PROCESS_CEPID,
	.message_id = BT_PANU_MESSAGE_BTA,
	.peer_addr = BT_PANU_HOST_BDADDR,
	.rx_mtu = BT_PANU_RX_MTU,
	.tx_mtu = BT_PANU_TX_MTU,
	.cb = &bt_pan_netif_cb,
	.user_data = &netif,
};

static const uint8_t local_ipv4[4] = BT_PAN_LOCAL_IPV4;
static const uint8_t peer_ipv4[4] = BT_PAN_PEER_IPV4;
static const uint8_t dns1_ipv4[4] = BT_PAN_DNS1_IPV4;
static const uint8_t dns2_ipv4[4] = BT_PAN_DNS2_IPV4;
static struct bt_panu panu;
static GBSTMR ping_timer;
static GBSTMR shutdown_timer;
static GBSTMR finalize_timer;
static GBSTMR watch_timer;
static GBSTMR reconnect_timer;
static bool auto_restart;       /* link lost or the RX watchdog took it down: bring it back up */
static bool reconnect_wait;     /* PAN is down, next attempt on reconnect_timer */
static unsigned auto_restarts;
static unsigned reconnect_fails; /* attempts in a row that did not get an IP */
static char log_buf[256];
static bool process_created;
static bool shutdown_started;
static bool pan_running;  /* PAN on/off, driven by the UI toggle */
static bool ui_active;    /* 1 = UI mode: the UI (MMI context) orchestrates the final exit */
static bool ui_exit_requested; /* set by finish_poc once BT is fully torn down; UI then closes+exits */
static bool exit_after_stop;   /* "Exit" while connected: close the UI once BT is down */

/* Settings: BCFG file (edit in the BCFG editor or from the elf's screen). Layout as in
 * XTask: each header is immediately followed by its value. */
typedef struct __attribute__((packed)) {
	CFG_HDR address_header; char address[20];  /* PC (NAP) "XX:XX:XX:XX:XX:XX" */
	CFG_HDR name_header; char name[48];        /* shown on screen, set by the device picker */
	CFG_HDR auto_header; unsigned reconnect; /* off by default */
} PAN_CONFIG;

static PAN_CONFIG cfg = {
	{CFG_STR_WIN1251, "PC Bluetooth address", 0, 19}, "00:00:00:00:00:00",
	{CFG_STR_UTF8, "PC name", 0, 47}, "",
	{CFG_CHECKBOX, "Auto-reconnect", 0, 1}, 0,
};
static char cfg_path[64] = "?:\\zbin\\etc\\BTPAN.bcfg";
static bool addr_ok;
#define auto_reconnect (cfg.reconnect != 0)

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* cfg -> panu_config.peer_addr (text order) */
static void config_apply(void) {
	uint8_t mac[6];
	unsigned any = 0, i;

	cfg.address[19] = 0;
	cfg.name[47] = 0;
	cfg.reconnect = !!cfg.reconnect;
	addr_ok = false;
	if (strlen(cfg.address) != 17)
		return;
	for (i = 0; i < 6; ++i) {
		int hi = hex_digit(cfg.address[3 * i]), lo = hex_digit(cfg.address[3 * i + 1]);

		if (hi < 0 || lo < 0 || (i < 5 && cfg.address[3 * i + 2] != ':'))
			return;
		mac[i] = (uint8_t)(hi << 4 | lo);
		any |= mac[i];
	}
	if (!any)
		return;
	memcpy(panu_config.peer_addr, mac, 6);
	addr_ok = true;
}

/* open/close, not GetFileStats: S65 fills a bigger FSTATS than the SDK's (D82 crashed in MMI) */
static bool file_exists(const char *path) {
	uint32_t err;
	int f = sys_open(path, A_ReadOnly + A_BIN, P_READ, &err);

	if (f < 0)
		return false;
	sys_close(f, &err);
	return true;
}

/* MMI context only: BCFG file I/O. ElfPack's default disk follows where it found ElfPack.bcfg
 * (MMC first), so it can differ from the disk the file was saved on: try it, then 0: and 4:.
 * A failed load never overwrites an existing file (it used to: the chosen PC got lost). */
static void config_load(void) {
	static const char disks[] = {0, '0', '4'};
	char def = (char)BCFG_GetDefaultDisk();
	unsigned i;

	for (i = 0; i < sizeof(disks); ++i) {
		cfg_path[0] = disks[i] ? disks[i] : def;
		if (i && cfg_path[0] == def)
			continue;
		if (file_exists(cfg_path) && BCFG_LoadConfig(cfg_path, &cfg, sizeof(cfg)) >= 0) {
			config_apply();
			return;
		}
	}
	cfg_path[0] = def;
	for (i = 1; i < sizeof(disks); ++i) {
		char alt[sizeof(cfg_path)];

		strcpy(alt, cfg_path);
		alt[0] = disks[i];
		if (file_exists(alt))
			break; /* exists but did not load: keep it, save only on the user's choice */
	}
	if (i == sizeof(disks) && !file_exists(cfg_path))
		BCFG_SaveConfig(cfg_path, &cfg, sizeof(cfg)); /* first run: create it for the editor */
	config_apply();
}

static void config_save(void) {
	BCFG_SaveConfig(cfg_path, &cfg, sizeof(cfg));
}

/* device picker */
static void save_peer_addr(const uint8_t mac[6], const char *name) {
	sprintf(cfg.address, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
			mac[5]);
	strncpy(cfg.name, name, sizeof(cfg.name) - 1);
	cfg.name[sizeof(cfg.name) - 1] = 0;
	config_save();
	config_apply();
}

static void set_auto_reconnect(bool on) {
	cfg.reconnect = on;
	config_save();
}

/* speed test server = the DHCP server (the PC), else the configured peer address */
static uint32_t pan_server_ip(void) {
	uint32_t ip;

	if (netif.dhcp_server_id)
		return netif.dhcp_server_id;
	memcpy(&ip, peer_ipv4, sizeof(ip));
	return ip;
}

static void process_messages(void);
static void finish_poc(GBSTMR *timer);
static int AppStartPan(void);
static int pan_start(void);
static void begin_shutdown(GBSTMR *timer);

static void send_ping(GBSTMR *timer) {
	uint32_t destination;

	(void)timer;
	memcpy(&destination, peer_ipv4, sizeof(destination));
	if (icmp_probe_send(&netif.module, netif.config.local_ip, destination)) {
		usart_puts("ICMP probe: send failed\r\n");
		return;
	}

	usart_puts("ICMP probe: echo request queued\r\n");
}

/*
 * RX watchdog (BTPAN process, every 5 s). Seen on device: after minutes of idle the phone
 * keeps sending but nothing from the PC arrives any more (PC sends the DNS replies, the
 * phone repeats the query). When nothing came for 10 s, ask the PC (ARP, always answered):
 * keeps the link busy and proves the receive path. No answer in 20 s: log where RX stopped
 * and kick the device; still nothing at 30 s: reconnect BT.
 */
#define WATCH_TICKS (5u * 216u)
static uint32_t watch_last;
static unsigned watch_idle, watch_beats;

static void rx_watch(GBSTMR *timer) {
	uint32_t count = bt_panu_rx_count();

	(void)timer;
	if (shutdown_started || !netif.registered)
		return;
	if (count != watch_last) {
		if (watch_idle >= 4)
			bt_panu_rx_report(&panu, "RX watchdog: receive is back\r\n");
		watch_last = count;
		watch_idle = 0;
	} else {
		++watch_idle;
	}
	if (++watch_beats % 12u == 0)
		bt_panu_rx_report(&panu, "RX watchdog: 60 s\r\n");
	if (watch_idle >= 2)
		bt_pan_netif_arp_probe(&netif, pan_server_ip());
	if (watch_idle == 4) {
		bt_panu_rx_report(&panu, "RX watchdog: nothing from the PC for 20 s, kicking\r\n");
		bt_panu_rx_kick(&panu);
	}
	if (watch_idle >= 6 && !auto_reconnect) {
		if (watch_idle == 6)
			bt_panu_rx_report(&panu, "RX watchdog: receive dead (auto-reconnect off)\r\n");
	} else if (watch_idle >= 6) {
		bt_panu_rx_report(&panu, "RX watchdog: receive dead, reconnecting\r\n");
		auto_restart = true;
		begin_shutdown(NULL);
		return;
	}
	GBS_StartTimerProc(&watch_timer, WATCH_TICKS, rx_watch);
}

static void netif_connected(struct bt_pan_netif *connected_netif) {
	(void)connected_netif;
	GBS_StartTimerProc(&ping_timer, 22, send_ping);
	reconnect_fails = 0;
	watch_last = bt_panu_rx_count();
	watch_idle = watch_beats = 0;
	GBS_StartTimerProc(&watch_timer, WATCH_TICKS, rx_watch);
}

/* The link went away by itself (out of range, PC closed it, open failed): not a stop of ours. */
static void netif_disconnected(struct bt_pan_netif *disconnected_netif) {
	(void)disconnected_netif;
	if (!shutdown_started && auto_reconnect && !exit_after_stop)
		auto_restart = true;
	begin_shutdown(NULL);
}

static struct bt_pan_netif_config netif_config = {
	.mtu = BT_PAN_MTU,
	.bearer_id = BT_PAN_BEARER_ID,
	.driver_bearer = BT_PAN_DRIVER_BEARER,
	.iface_index = BT_PAN_IFACE_INDEX,
	.connected = netif_connected,
	.disconnected = netif_disconnected,
};

static void pan_stopped(void);

#if BT_PAN_TRACE
const char *volatile trace_where = "start";
volatile unsigned trace_where_arg, trace_where_cepid, trace_where_seq;
#endif

static void process_messages_(void);

static void process_messages(void) {
	WHERE("P recv", 0);
	process_messages_();
	WHERE("P idle", 0);
}

static void process_messages_(void) {
	static int traced;
	GBS_MSG msg;

	if (traced < 40) trace("P enter");
	if (!GBS_RecActDstMessage(&msg)) {
		if (traced < 40) trace("P no message");
		return;
	}
#if BT_PAN_TRACE
	if (traced < 40) {
		char t[48];
		++traced;
		sprintf(t, "P msg %X from %X", (unsigned)msg.msg, (unsigned)msg.pid_from);
		trace(t);
	}
#endif

	sprintf(log_buf, "GBS: from=%04X msg=%08X sub=%08X data0=%p data1=%p\r\n",
			(unsigned int)msg.pid_from, (unsigned int)msg.msg, (unsigned int)msg.submess, msg.data0,
			msg.data1);
	usart_puts(log_buf);

	WHERE("P msg", (unsigned)(uint16_t)msg.msg);
	switch (GBS_MSG_ID(msg.msg)) {
		case GBS_MSG_ID(BT_PANU_MESSAGE_INIT):
			peers_stop(); /* never two accessors of ours at once */
			bt_panu_start(&panu);
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_PEERS_BTA):
			peers_handle_bta_msg(&msg);
#ifndef SGOLD
			GBS_DestroyMessage(&msg);
#endif
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_PEERS_START):
			if (!pan_running)
				peers_start(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_BTA);
			trace("P peers started");
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_PEERS_STOP):
			peers_stop();
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_DIRECT_START):
			speedtest_direct_start(&netif, pan_server_ip());
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_DIRECT_STOP):
			speedtest_direct_cancel();
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_PING_START):
			ping_start(&netif, pan_server_ip(), (unsigned)(uintptr_t)msg.data0);
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_PING_STOP):
			ping_stop();
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_BTA):
			bt_panu_handle_bta_msg(&panu, &msg);
#ifndef SGOLD
			/* Accessor events are long GBS messages owned by the receiver. (SGOLD: MR_Done
			 * in the handler gives the event back instead.) */
			GBS_DestroyMessage(&msg);
#endif
			break;
		case GBS_MSG_ID(BT_PANU_MESSAGE_STOP):
			if (reconnect_wait) {
				if (IsTimerProc(&reconnect_timer))
					GBS_StopTimer(&reconnect_timer);
				pan_stopped();
			} else {
				begin_shutdown(NULL);
			}
			break;
		default:
			break;
	}
}

static int create_process(void) {
#ifdef SGOLD
	/* S65 Nucleus priorities (lower = first, static groups A130D7E4): ENIP/LMAN 40, MMI 5A,
	 * Java/HTTP and ElfLoader's helper 6E. At 80 BTPAN never ran while Java polled ENIP for a
	 * DNS answer, so the answer that BTPAN receives never arrived (D85). Between MMI and Java. */
	static PGROUP group = {BT_PANU_PROCESS_GROUP, 0x60, 0x4000, 2};
#else
	static PGROUP group = {
		BT_PANU_PROCESS_GROUP, "T_BTPAN", 0x80, 0x4000, 2,
	};
#endif

	if (GetGBSProcAddress(BT_PANU_PROCESS_CEPID)) {
		usart_puts("BTPAN CEPID 1A00 is already running\r\n");
		return 0;
	}

	if (!GetProcGroup(BT_PANU_PROCESS_CEPID))
		CreateICL(&group);
	CreateGBSproc(BT_PANU_PROCESS_CEPID, "BTPAN", process_messages, 0x80, 0);
	if (!GetGBSProcAddress(BT_PANU_PROCESS_CEPID)) {
		usart_puts("Cannot create BTPAN GBS process\r\n");
		return 0;
	}
	process_created = true;
	return 1;
}

static void begin_shutdown(GBSTMR *timer) {
	(void)timer;
	if (shutdown_started)
		return;

	shutdown_started = true;
	usart_puts("BT PAN: shutting down\r\n");
	speedtest_direct_cancel();
	ping_stop();
	if (IsTimerProc(&ping_timer))
		GBS_StopTimer(&ping_timer);
	if (IsTimerProc(&watch_timer))
		GBS_StopTimer(&watch_timer);
	bt_pan_netif_stop(&netif);
	bt_panu_stop(&panu);
	GBS_StartTimerProc(&finalize_timer, 22, finish_poc);
}

/* 1 h session cap: with auto-reconnect it is just another reconnect */
static void session_timeout(GBSTMR *timer) {
	(void)timer;
	if (!shutdown_started && auto_reconnect && !exit_after_stop)
		auto_restart = true;
	begin_shutdown(NULL);
}

static void reconnect_now(GBSTMR *timer) {
	(void)timer;
	reconnect_wait = false;
	if (!auto_reconnect || exit_after_stop) { /* switched off while waiting */
		pan_stopped();
		return;
	}
	pan_running = false;
	if (pan_start() < 0)
		pan_stopped();
}

/* PAN is fully down and stays down */
static void pan_stopped(void) {
	reconnect_wait = false;
	auto_restart = false;
	pan_running = false;
	usart_puts("BT PAN: stopped\r\n");

	/* UI mode: BT is fully down; stay on screen for a new connection unless the user asked
	 * to leave (then the UI closes from MMI context and its onclose kills the process+elf). */
	if (ui_active) {
		if (exit_after_stop)
			ui_exit_requested = true;
		return;
	}

	if (process_created && GetGBSProcAddress(BT_PANU_PROCESS_CEPID)) {
		KillGBSproc(BT_PANU_PROCESS_CEPID);
		process_created = false;
	}

	usart_puts("BT PANU: kill_elf\r\n");
	kill_elf();
}

static void finish_poc(GBSTMR *timer) {
	(void)timer;

	if (!bt_panu_is_stopped(&panu)) {
		GBS_StartTimerProc(&finalize_timer, 22, finish_poc);
		return;
	}
	if (bt_panu_deinit(&panu)) {
		GBS_StartTimerProc(&finalize_timer, 216, finish_poc);
		return;
	}

	if (IsTimerProc(&shutdown_timer))
		GBS_StopTimer(&shutdown_timer);

	shutdown_started = false;

	if (auto_restart && auto_reconnect && !exit_after_stop) {
		/* PAN stays "on" (pan_running) while waiting; out of range every attempt fails,
		 * so back off: 3, 5, 10, 20, then every 30 s */
		static const unsigned delay_s[] = {3, 5, 10, 20, 30};
		unsigned d = delay_s[reconnect_fails < 4 ? reconnect_fails : 4];

		auto_restart = false;
		++auto_restarts;
		++reconnect_fails;
		sprintf(log_buf, "BT PAN: reconnect #%u in %u s\r\n", auto_restarts, d);
		usart_puts(log_buf);
		reconnect_wait = true;
		GBS_StartTimerProc(&reconnect_timer, 216 * d, reconnect_now);
		return;
	}
	pan_stopped();
}

/* Turn PAN on (UI / headless start). */
static int AppStartPan(void) {
	if (pan_running)
		return 0;
	reconnect_fails = 0;
	return pan_start();
}

/* Re-inits the stacks each time so a stop/start cycle is clean. */
static int pan_start(void) {
	if (!addr_ok) {
		usart_puts("BT PAN: no PC address in BTPAN.bcfg\r\n");
		return -1;
	}
	if (hooks_patch_state() == PATCH_OLD) {
		usart_puts("BT PAN: old hooks VKP in flash (no WSP availability hook): "
				   "install the one from the release patches\r\n");
		return -1;
	}
	memcpy(&netif_config.local_ip, local_ipv4, sizeof(netif_config.local_ip));
	memcpy(&netif_config.dns1, dns1_ipv4, sizeof(netif_config.dns1));
	memcpy(&netif_config.dns2, dns2_ipv4, sizeof(netif_config.dns2));
	bt_panu_init(&panu, &panu_config);
	bt_pan_netif_init(&netif, &netif_config);
	if (!GetGBSProcAddress(BT_PANU_PROCESS_CEPID) && !create_process()) {
		usart_puts("BT PAN: cannot create BTA process\r\n");
		return -1;
	}
	shutdown_started = false;
	pan_running = true;
	usart_puts("BT PAN: start\r\n");
	GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_INIT, 0, 0, 0);
	GBS_StartTimerProc(&shutdown_timer, 216 * BT_PANU_RUN_SECONDS, session_timeout);
	return 0;
}

/* Turn PAN off. Teardown runs in the BTA process (see BT_PANU_MESSAGE_STOP). */
static void AppStopPan(void) {
	if (!pan_running)
		return;
	auto_restart = false; /* the user's "off" wins over a watchdog reconnect in progress */
	GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_STOP, 0, 0, 0);
}

#define BT_PAN_MSG_SHOW 0x7B70     /* to MMI: "show your screen", from a second start */
#define BT_PAN_MSG_MAGIC 0x4E415042 /* "BPAN" in submess */

#if BT_PAN_UI
#include "app_ui.h"
#endif

/* Another copy of the elf is loaded: its BTA helper process (made at start, both builds, gone
 * only when that copy unloads) or its screen CSM ("BT PAN", after its CSM_DESC as ours). Two
 * copies would share the process, the SDev slots and the ENIP interface. */
static int AnotherRunning(void) {
	CSM_RAM *c;
	if (GetGBSProcAddress(BT_PANU_PROCESS_CEPID))
		return 1;
	for (c = CSM_root()->csm_q->csm.first; c; c = c->next) {
		const WSHDR *ws = (const WSHDR *)((const char *)c->constr + sizeof(CSM_DESC));
		static const char name[] = "BT PAN";
		int i, len = (int)sizeof(name) - 1;
		if ((uint32_t)ws->ws_malloc != (uint32_t)NAMECSM_MAGIC1 ||
			(uint32_t)ws->ws_mfree != (uint32_t)NAMECSM_MAGIC2 || ws->wsbody[0] != len)
			continue;
		for (i = 0; i < len && ws->wsbody[i + 1] == (uint16_t)name[i]; ++i) {}
		if (i == len)
			return 1;
	}
	return 0;
}

int main(char *exe, char *fname, void *p1) {
	(void)exe;
	(void)fname;
	(void)p1;

	trace("--- start " BT_PAN_BUILD_TAG);
	if (AnotherRunning()) { /* before anything that touches the running copy's state (USART) */
		/* the running copy brings its screen up (app_ui.h), this one goes quietly */
		GBS_SendMessage(MMI_CEPID, BT_PAN_MSG_SHOW, BT_PAN_MSG_MAGIC, 0, 0);
		SUBPROC((void *)kill_elf);
		return 0;
	}
	trace("1 single instance ok");
	usart_init();
	usart_set_speed(115200);
	trace("2 usart");
	usart_puts("\r\n--- BT PAN ENIP " BT_PAN_BUILD_TAG " ---\r\n");
	if (lib_check()) {
		usart_puts("BT PAN: function library has no entries for this elf\r\n");
		ShowMSG(1, (int)"BT PAN: install the function library from the release patches");
		SUBPROC((void *)kill_elf);
		return 0;
	}
	trace("4 library ok");
#ifdef SGOLD
	if (bt_sg_mtu_patched()) {
		panu_config.rx_mtu = panu_config.tx_mtu = BT_BNEP_MIN_MTU;
		bt_pan_tx_max = BT_PAN_SG_BIG_IP_MTU;
		netif_config.mtu = BT_PAN_SG_BIG_STACK_MTU;
		usart_puts("BT PAN: MTU patch found, L2CAP 1691\r\n");
	}
#endif
	pan_lman_init(); /* socket helper registration, so apps' contexts can be pinned */
	trace("5 lman init");

	config_load();
	trace("6 config");

#if BT_PAN_UI
	ui_active = true;
	if (!GetGBSProcAddress(BT_PANU_PROCESS_CEPID))
		create_process(); /* the device list needs it before the first connection */
	trace("7 process");
	AppCreateUI();      /* on/off screen; PAN starts from the toggle */
	trace("8 ui created");
#else
	AppStartPan();      /* headless: start now, off = kill_elf (proven path) */
#endif
	return 0;
}
