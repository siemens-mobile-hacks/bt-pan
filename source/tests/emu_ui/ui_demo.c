/*
 * UI demo for elfloader3-linux: the real src/app_ui.h over a fake PAN (no BT, no ENIP, no
 * function library). Connect walks Starting -> Connecting -> Getting IP -> Connected in ~2 s,
 * then the counters grow; the speed test and ping make up numbers; the device list has 3 PCs.
 * Build: tests/emu_ui/run.sh (also runs the key scenario and saves screenshots).
 */
#include <swilib.h>
#include <cfg_items.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "build_id.h"
#include "config.h"
#include "bt_panu.h"
#include "bt_pan_netif.h"
#include "pan_lman.h"
#include "peers.h"
#include "ping.h"
#include "speedtest.h"

#define BT_PANU_PROCESS_CEPID 0x1a00u
#define BT_PANU_MESSAGE_PEERS_START 0x6b51u
#define BT_PANU_MESSAGE_PEERS_STOP 0x6b52u
#define BT_PANU_MESSAGE_DIRECT_START 0x6b53u
#define BT_PANU_MESSAGE_DIRECT_STOP 0x6b54u
#define BT_PANU_MESSAGE_PING_START 0x6b55u
#define BT_PANU_MESSAGE_PING_STOP 0x6b56u
#define BT_PAN_MSG_SHOW 0x7B70
#define BT_PAN_MSG_MAGIC 0x4E415042

static struct bt_pan_netif netif;
static struct bt_panu panu;
static struct bt_panu_config panu_config = {.peer_addr = {0x1C, 0xCE, 0x51, 0xF7, 0x35, 0xE7}};
static bool pan_running, ui_exit_requested, exit_after_stop, process_created, reconnect_wait;
static bool addr_ok = true;
static struct { char name[48]; unsigned reconnect; } cfg = {"dm-desktop", 0};
static char cfg_path[64] = "4:\\zbin\\etc\\BTPAN.bcfg";
#define auto_reconnect (cfg.reconnect != 0)

volatile uint32_t bt_pan_rx_bytes, bt_pan_tx_bytes;
struct speedtest_state speedtest[2];
struct ping_stats ping_stats;
struct peer peers[PEERS_MAX];
volatile int peer_count;
volatile bool peers_busy;

static void config_load(void) {}
void peers_stop(void) {}
int hooks_patch_state(void) { return PATCH_OK; }
unsigned bt_pan_netif_tx_max(void) { return 1500; }
unsigned ping_max_size(void) { return 1472; }
int speedtest_running(void) { return speedtest[0].state == SPT_RUN || speedtest[1].state == SPT_RUN; }
static uint32_t pan_server_ip(void) { return 0x0107a8c0u; }
static void set_auto_reconnect(bool on) { cfg.reconnect = on; }
static void save_peer_addr(const uint8_t mac[6], const char *name) {
	memcpy(panu_config.peer_addr, mac, 6);
	strncpy(cfg.name, name, sizeof(cfg.name) - 1);
	addr_ok = true;
}

/* the fake PAN and tests run on one MMI timer, 0.1 s */
static GBSTMR fake_timer;
static unsigned fake_t, conn_t, test_t, ping_t;
static void fake_tick(GBSTMR *t) {
	(void)t;
	++fake_t;
	if (pan_running) {
		unsigned d = fake_t - conn_t;
		if (panu.state == BT_PANU_STATE_STOPPING) {
			if (d > 5) {
				pan_running = false; netif.registered = false; panu.state = BT_PANU_STATE_IDLE;
				if (exit_after_stop) ui_exit_requested = true;
			}
		} else if (d < 5) panu.state = BT_PANU_STATE_STARTING;
		else if (d < 10) panu.state = BT_PANU_STATE_CONNECTING;
		else if (d < 15) { panu.state = BT_PANU_STATE_CONNECTED; netif.dhcp_state = BT_PAN_DHCP_SELECTING; }
		else {
			netif.dhcp_state = BT_PAN_DHCP_BOUND; netif.registered = true;
			netif.config.local_ip = 0x2c07a8c0u; netif.config.dns1 = 0x0107a8c0u;
			panu.neg_tx_mtu = panu.neg_rx_mtu = 1691;
			if (d < 95) { bt_pan_rx_bytes += 4321; bt_pan_tx_bytes += 987; } /* then idle */
		}
	}
	if (peers_busy && fake_t % 5 == 0) {
		static const char *names[] = {"dm-desktop", "Pixel 7", "Ноутбук"};
		int i = peer_count;
		peers[i].mac[0] = 0x1C; peers[i].mac[5] = (uint8_t)(0x10 + i);
		strcpy(peers[i].name, names[i]);
		if (++peer_count == 3) peers_busy = false;
	}
	if (speedtest[0].state == SPT_RUN) {
		unsigned d = fake_t - test_t;
		speedtest[0].progress = d * 100 / 40;
		speedtest[0].dir = d < 20 ? SPT_DL : SPT_UL;
		speedtest[0].live_bps = 80000 + d * 37;
		if (d == 20) speedtest[0].dl_bps = 84213;
		if (d >= 40) { speedtest[0].ul_bps = 61022; speedtest[0].state = SPT_DONE; }
	}
	if (ping_stats.running && fake_t - ping_t >= 10) {
		unsigned us = 30000 + (fake_t % 7) * 1700;
		ping_t = fake_t;
		++ping_stats.sent;
		if (fake_t % 50) {
			++ping_stats.received; ping_stats.sum_us += us; ping_stats.last_us = us;
			if (!ping_stats.min_us || us < ping_stats.min_us) ping_stats.min_us = us;
			if (us > ping_stats.max_us) ping_stats.max_us = us;
		} else { us = PING_LOST; ping_stats.last_us = us; }
		ping_stats.hist[ping_stats.hist_n++ % PING_HIST] = us;
	}
	GBS_StartTimerProc(&fake_timer, 22, fake_tick);
}

/* messages to the BTPAN process: done right here */
#undef GBS_SendMessage
static void fake_msg(unsigned to, unsigned msg, unsigned sub, void *d0, void *d1) {
	(void)sub; (void)d1;
	if (to != BT_PANU_PROCESS_CEPID) { GBS_SendMessage(to, msg, sub, d0, d1); return; }
	if (msg == BT_PANU_MESSAGE_DIRECT_START) test_t = fake_t;
	if (msg == BT_PANU_MESSAGE_DIRECT_STOP) speedtest[0].state = SPT_IDLE;
	if (msg == BT_PANU_MESSAGE_PING_START) { memset(&ping_stats, 0, sizeof(ping_stats)); ping_stats.running = 1; ping_t = fake_t; }
	if (msg == BT_PANU_MESSAGE_PING_STOP) ping_stats.running = 0;
}
#define GBS_SendMessage(to, msg, sub, d0, d1) fake_msg((to), (msg), (sub), (void *)(d0), (void *)(d1))

static int AppStartPan(void) { pan_running = true; conn_t = fake_t; return 0; }
static void AppStopPan(void) { panu.state = BT_PANU_STATE_STOPPING; conn_t = fake_t; }
void speedtest_stack_cancel(void) { if (speedtest[1].state == SPT_RUN) speedtest[1].state = SPT_IDLE; }
void speedtest_stack_start(uint32_t ip) { (void)ip; speedtest[1].state = SPT_ERR; speedtest[1].error = "No server on the PC"; }
void speedtest_stack_msg(GBS_MSG *m) { (void)m; }

/* idle check: UI timer polls and menu refreshes per second -> 4:\\uipolls.txt */
static unsigned demo_refresh;
static void demo_refresh_gui(void) { ++demo_refresh; RefreshGUI(); }
#define RefreshGUI demo_refresh_gui

/* UI timer arms (UiArm: once per UiPoll, plus a few on keys/open) */
static unsigned ui_polls;
static void demo_start_timer(GBSTMR *t, long ticks, GBSTMR_CALLBACK cb) { ++ui_polls; GBS_StartTimerProc(t, ticks, cb); }
#define BT_PAN_UI 1
#define BT_PAN_EMU 1 /* scr_open keeps elfloader3's "current" CSM */
static void log_process_kill(void) {}
#define GBS_StartTimerProc demo_start_timer
#include "app_ui.h"
#undef GBS_StartTimerProc

static GBSTMR log_timer;
static void demo_log(const char *line) {
	uint32_t err;
	int f = sys_open("4:\\uipolls.txt", A_WriteOnly + A_Create + A_Append + A_BIN, P_READ + P_WRITE, &err);
	if (f >= 0) { sys_write(f, line, strlen(line), &err); sys_close(f, &err); }
}
/* camera (0x14) = the firmware's DoIDLE (every CSM above idle closed, top down), side (0x15) =
 * the elf started again (main()'s message to the running copy) */
static int demo_key(int key, int m) {
	if (m != KEY_DOWN || (key != 0x14 && key != 0x15)) return KEYHOOK_NEXT;
	if (key == 0x14) {
		int ids[16], n = 0, i;
		char b[64];
		CSM_RAM *c;
		for (c = (CSM_RAM *)CSM_root()->csm_q->csm.last; c && c->id != CSM_root()->idle_id && n < 16;
			 c = (CSM_RAM *)c->prev)
			ids[n++] = c->id;
		snprintf(b, sizeof(b), "emu: DoIDLE closes %d CSM(s) above idle\n", n);
		demo_log(b);
		for (i = 0; i < n; i++) CloseCSM(ids[i]);
	} else {
		demo_log("emu: started again\n");
		GBS_SendMessage(MMI_CEPID, BT_PAN_MSG_SHOW, BT_PAN_MSG_MAGIC, 0, 0);
	}
	return KEYHOOK_BREAK;
}

/* the phone puts the daemon right above csm.first; elfloader3 appends on top: relink it */
static void emu_to_bottom(int id) {
	CSMQ *q = CSM_root()->csm_q;
	CSM_RAM *c = FindCSMbyID(id), *first = (CSM_RAM *)q->csm.first;
	if (!c || !first || first == c || first->next == c) return;
	((CSM_RAM *)c->prev)->next = c->next;
	if (c->next) ((CSM_RAM *)c->next)->prev = c->prev;
	else q->csm.last = c->prev;
	c->next = first->next;
	c->prev = first;
	if (first->next) ((CSM_RAM *)first->next)->prev = c;
	else q->csm.last = c;
	first->next = c;
}

static void log_tick(GBSTMR *t) {
	static unsigned sec, last_polls, last_refresh;
	char b[96];
	int f;
	uint32_t err;
	(void)t;
	++sec;
	f = sys_open("4:\\uipolls.txt", A_WriteOnly + A_Create + A_Append + A_BIN, P_READ + P_WRITE, &err);
	if (f >= 0) {
		int n = snprintf(b, sizeof(b), "t=%u polls=%u refresh=%u interval=%u state=%s top=%d dmn=%d scr=%d\n", sec,
						 ui_polls - last_polls, demo_refresh - last_refresh, ui_interval, UiStatus(),
						 scr[SCR_MAIN].id ? IsGuiOnTop(scr[SCR_MAIN].id) : -1, dmn_csm != NULL,
						 main_csm != NULL);
		sys_write(f, b, n, &err);
		sys_close(f, &err);
	}
	last_polls = ui_polls;
	last_refresh = demo_refresh;
	if (quitting && !dmn_csm && !main_csm) { /* the elf would be gone: let the emulator stop */
		demo_log("all closed\n");
		RemoveKeybMsgHook(demo_key);
		if (IsTimerProc(&fake_timer)) GBS_StopTimer(&fake_timer);
		return;
	}
	GBS_StartTimerProc(&log_timer, 216, log_tick);
}

int main(char *exe, char *fname, void *p1) {
	(void)exe; (void)fname; (void)p1;
	emu_to_bottom(AppCreateUI());
	AddKeybMsgHook(demo_key);
	GBS_StartTimerProc(&fake_timer, 22, fake_tick);
	GBS_StartTimerProc(&log_timer, 216, log_tick);
	return 0;
}
