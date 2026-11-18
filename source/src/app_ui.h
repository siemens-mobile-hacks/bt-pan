#pragma once
/*
 * BT PAN screen = the firmware's own menus (CreateMenu, as ncdu / etabdemo / s65-player), no
 * self-drawn GUI any more (D130):
 *
 *   BT PAN Dxxx      Connect/Disconnect, Status, [hook patch hint], PC, Auto-reconnect,
 *                    IP, Gateway, DNS, MTU (online only), Received, Sent (+ speed), Speed test...,
 *                    Ping..., Minimize, Exit
 *   Speed test       Direct (BT, UDP) start/stop + its results, Via stack (TCP) the same
 *   Ping             start/stop, size, target, last/replies/lost/average/min-max/history
 *   Choose PC        the paired devices (name, MAC)
 *
 * The firmware draws everything (header, list, cursor, softkeys, scrolling); we only give the
 * item texts. Live values: a 0.25 s MMI timer compares the shown values with the current ones
 * (no sprintf) and re-syncs only the menu that is on top: state changes at once, the counters at
 * most once a second. Never REDRAW() (the old screen did up to 3 per second and per key: the
 * whole ELKA screen blinked, see STATUS.txt D130).
 *
 * PAN itself runs in the separate 0x1A00 GBS process; this screen drives AppStartPan()/
 * AppStopPan() (main.c) and posts requests to that process. Included at the end of main.c.
 */
#include <swilib.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { CSM_RAM csm; int gui_id; } MAIN_CSM;

#define UI_POLL_FAST 54u        /* ~0.25 s: our screen on top and a state in transition */
#define UI_POLL_SLOW 216u       /* 1 s otherwise (connected and idle, minimized, under a message) */
#define UI_LIVE_TICKS 216u      /* counters/speeds on screen at most once a second */
#define BT_PAN_SCREEN_NAME "BT PAN"
#define BT_PAN_DAEMON_NAME "BT PAN (link)" /* no GUI, below idle */

static MAIN_CSM *main_csm;     /* the screen CSM, NULL when closed (the link stays up) */
static CSM_RAM *dmn_csm;       /* the daemon CSM (below idle), NULL once closed */
static int dmn_id;
static bool quitting;          /* Exit: the screen closes, then the daemon, then the elf */
static unsigned quit_ticks;
static bool scr_reopen_hidden, scr_hide; /* D154: closed by DoIDLE -> made again below idle (XTask lists it) */
static bool scr_pending;       /* the screen's CreateCSM was asked for (NSG: an MMI command, run later) */
static GBSTMR ui_timer;
static unsigned ui_interval;   /* period the timer is armed with */
static unsigned ui_ticks;      /* sum of the periods: the poll's clock */
static unsigned ui_live_at;    /* ui_ticks of the last counter/speed update */
static bool ui_in_poll;
static WSHDR *ui_tmp;          /* scratch for item texts (MMI only) */
static int ui_patch;           /* hooks_patch_state(): flash, does not change while running */
static int ping_size_i;        /* index into ping_sizes */
static const unsigned ping_sizes[] = {56, 512, 1472};

/* traffic speed, once a second from the poll */
static uint32_t rate_last_rx, rate_last_tx;
static unsigned rate_rx, rate_tx;

static void AppExit(void);
static void AppMinimize(void);
static void scr_open(void);

/* ------------------------------------------------------------------------- formatting */

static void FmtRate(char *buf, int size, unsigned n) {
	if (n < 1000) snprintf(buf, size, "%u B/s", n);
	else if (n < 1000000) snprintf(buf, size, "%u.%u KB/s", n / 1000, n % 1000 / 100);
	else snprintf(buf, size, "%u.%u MB/s", n / 1000000, n % 1000000 / 100000);
}
static void FmtBytes(char *buf, int size, unsigned n) {
	if (n < 1000) snprintf(buf, size, "%u B", n);
	else if (n < 1000000) snprintf(buf, size, "%u.%u KB", n / 1000, n % 1000 / 100);
	else snprintf(buf, size, "%u.%u MB", n / 1000000, n % 1000000 / 100000);
}
static void FmtMac(char *buf, int size, const uint8_t *m) {
	snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static void FmtIp(char *buf, int size, const char *label, uint32_t ip) {
	snprintf(buf, size, "%s%u.%u.%u.%u", label, (unsigned)(ip & 0xffu), (unsigned)((ip >> 8) & 0xffu),
			 (unsigned)((ip >> 16) & 0xffu), (unsigned)((ip >> 24) & 0xffu));
}
static void FmtRtt(char *buf, int size, unsigned us) {
	if (us == PING_LOST) snprintf(buf, size, "timeout");
	else if (us < 10000) snprintf(buf, size, "%u.%u ms", us / 1000, us % 1000 / 100);
	else snprintf(buf, size, "%u ms", (us + 500) / 1000);
}

static const char *UiStatus(void) {
	if (!pan_running && ui_patch == PATCH_OLD) return "Old hook patch";
	if (!pan_running && !addr_ok) return "Choose a PC";
	if (!pan_running) return "Disconnected";
	if (reconnect_wait) return "Reconnecting...";
	switch (panu.state) {
		case BT_PANU_STATE_STARTING: return "Starting BT...";
		case BT_PANU_STATE_CONNECTING: return "Connecting...";
		case BT_PANU_STATE_CONNECTED:
			if (netif.registered) return "Connected";
			if (netif.dhcp_state == BT_PAN_DHCP_SELECTING || netif.dhcp_state == BT_PAN_DHCP_REQUESTING)
				return "Getting IP...";
			return "Connecting...";
		case BT_PANU_STATE_STOPPING: return "Disconnecting...";
		default: return "Waiting...";
	}
}

static int Online(void) { return pan_running && netif.registered; }
static int Busy(void) { return pan_running && panu.state == BT_PANU_STATE_STOPPING; }

static unsigned PingSize(void) {
	unsigned size = ping_sizes[ping_size_i], max = ping_max_size();
	return size < max ? size : max;
}

/* ------------------------------------------------------------------------- menus */

/* Every screen is a list of row ids built from the state; the cursor follows its row id when
 * rows come and go. */
enum { SCR_MAIN, SCR_TEST, SCR_PING, SCR_PICK, SCR_N };
#define ROWS_MAX 24
typedef struct {
	int id;                    /* GUI id, 0 = not open */
	int n;                     /* rows */
	unsigned char row[ROWS_MAX];
	unsigned sig_state, sig_live;
	const char *left;          /* left softkey label shown */
} SCREEN;
static SCREEN scr[SCR_N];

enum {
	M_CONNECT, M_STATUS, M_HINT, M_PC, M_AUTO, M_IP, M_GW, M_DNS, M_MTU, M_RX, M_TX,
	M_TEST, M_PING, M_MIN, M_EXIT, M_DBG0, M_DBG1, M_DBG2, M_DBG3
};
enum { T_BTN0, T_RUN0, T_DL0, T_UL0, T_BTN1, T_RUN1, T_DL1, T_UL1, T_HINT };
enum { P_BTN, P_SIZE, P_TO, P_ERR, P_LAST, P_REPLIES, P_LOST, P_AVG, P_MINMAX, P_HIST };

static int Rows(int s, unsigned char *r) {
	int n = 0, i;
	switch (s) {
		case SCR_MAIN:
			r[n++] = M_CONNECT; r[n++] = M_STATUS;
#ifdef BT_PAN_DBG
			r[n++] = M_DBG0; r[n++] = M_DBG1; r[n++] = M_DBG2; r[n++] = M_DBG3;
#endif
			if (!pan_running && ui_patch != PATCH_OK) r[n++] = M_HINT;
			r[n++] = M_PC; r[n++] = M_AUTO;
			if (Online()) {
				r[n++] = M_IP; r[n++] = M_GW;
				if (netif.config.dns1) r[n++] = M_DNS;
				r[n++] = M_MTU;
			}
			r[n++] = M_RX; r[n++] = M_TX;
			r[n++] = M_TEST; r[n++] = M_PING; r[n++] = M_MIN;
			r[n++] = M_EXIT;
			break;
		case SCR_TEST:
			for (i = 0; i < 2; ++i) {
				int st = speedtest[i].state, b = i ? T_BTN1 : T_BTN0;
				r[n++] = b;
				if (st == SPT_RUN || st == SPT_ERR) r[n++] = b + 1;
				if (st != SPT_IDLE) { r[n++] = b + 2; r[n++] = b + 3; }
			}
			if (!Online()) r[n++] = T_HINT;
			break;
		case SCR_PING: {
			struct ping_stats *p = &ping_stats;
			r[n++] = P_BTN; r[n++] = P_SIZE;
			if (Online() || p->sent) r[n++] = P_TO;
			if (p->error || (!Online() && !p->sent)) r[n++] = P_ERR;
			if (p->sent) {
				r[n++] = P_LAST; r[n++] = P_REPLIES; r[n++] = P_LOST;
				if (p->received) { r[n++] = P_AVG; r[n++] = P_MINMAX; }
				if (p->hist_n) r[n++] = P_HIST;
			}
			break;
		}
		default:
			n = peer_count ? peer_count : 1;
			if (n > ROWS_MAX) n = ROWS_MAX;
			for (i = 0; i < n; ++i) r[i] = (unsigned char)i;
			break;
	}
	return n;
}

static int RowAt(int s, int item) {
	return item >= 0 && item < scr[s].n ? scr[s].row[item] : -1;
}

/* signatures of what a screen shows: "state" = rows and texts that change seldom (shown at
 * once), "live" = counters (shown at most once a second) */
#define MIX(s, v) ((s) = (s) * 31u + (unsigned)(v))
static void Sig(int s, unsigned *st, unsigned *live) {
	unsigned a = 0, b = 0, i;
	MIX(a, pan_running); MIX(a, panu.state); MIX(a, reconnect_wait); MIX(a, netif.registered);
	MIX(a, Busy());
	switch (s) {
		case SCR_MAIN:
			MIX(a, netif.dhcp_state); MIX(a, addr_ok); MIX(a, auto_reconnect); MIX(a, cfg.name[0]);
			MIX(a, panu_config.peer_addr[5]); MIX(a, panu_config.peer_addr[4]); MIX(a, ping_stats.running);
			if (Online()) {
				MIX(a, netif.config.local_ip); MIX(a, netif.config.dns1); MIX(a, pan_server_ip());
				MIX(a, panu.neg_tx_mtu); MIX(a, panu.neg_rx_mtu);
			}
			MIX(b, bt_pan_rx_bytes / 100u); MIX(b, bt_pan_tx_bytes / 100u);
#ifdef BT_PAN_DBG
			MIX(b, dbg_msgs); MIX(b, dbg_watch); MIX(b, bt_panu_rx_count()); MIX(b, panu.state);
			MIX(b, panu.close_polls); MIX(b, panu.drain_polls); MIX(b, bt_panu_tx_free());
#endif
			MIX(b, rate_rx / 100u); MIX(b, rate_tx / 100u);
			break;
		case SCR_TEST:
			for (i = 0; i < 2; ++i) {
				MIX(a, speedtest[i].state); MIX(a, speedtest[i].dir); MIX(a, (uintptr_t)speedtest[i].error);
				MIX(a, speedtest[i].dl_bps); MIX(a, speedtest[i].ul_bps);
				MIX(b, speedtest[i].progress); MIX(b, speedtest[i].live_bps / 100u);
			}
			MIX(a, speedtest_running());
			break;
		case SCR_PING:
			MIX(a, ping_stats.running); MIX(a, ping_stats.sent != 0); MIX(a, ping_stats.received != 0);
			MIX(a, ping_stats.hist_n != 0); MIX(a, (uintptr_t)ping_stats.error); MIX(a, ping_size_i);
			MIX(b, ping_stats.sent); MIX(b, ping_stats.received); MIX(b, ping_stats.last_us);
			break;
		default:
			MIX(a, peer_count); MIX(a, peers_busy);
			break;
	}
	*st = a;
	*live = b;
}

static void ItemText(void *gui, int item_n, const char *text) {
	void *item = AllocMenuItem(gui);
	WSHDR *w = AllocMenuWS(gui, 128);
	wsprintf(w, "%t", text);
	SetMenuItemText(gui, item, w, item_n);
}

static void MainItem(void *gui, int item_n, void *user) {
	char b[64], v[24];
	(void)user;
	switch (RowAt(SCR_MAIN, item_n)) {
		case M_CONNECT:
			ItemText(gui, item_n, pan_running ? (Busy() ? "Please wait..." : "Disconnect") : "Connect");
			return;
		case M_STATUS: snprintf(b, sizeof(b), "Status: %s", UiStatus()); break;
		case M_DBG0: case M_DBG1: case M_DBG2:
			bt_panu_dbg_line(&panu, RowAt(SCR_MAIN, item_n) - M_DBG0, b, sizeof(b));
			break;
		case M_DBG3:
			snprintf(b, sizeof(b), "msg%u %X w%u i%u run%d stop%d rc%d", dbg_msgs, dbg_last_msg, dbg_watch,
					 watch_idle, pan_running, shutdown_started, reconnect_wait);
			break;
		case M_HINT:
			ItemText(gui, item_n, ui_patch == PATCH_OLD ? "Undo the old hook patch, install the new one"
													   : "No hook patch: the browser won't work");
			return;
		case M_PC: {
			void *item = AllocMenuItem(gui);
			WSHDR *w = AllocMenuWS(gui, 128);
			if (!addr_ok) wsprintf(w, "%t", "PC: choose...");
			else {
				FmtMac(v, sizeof(v), panu_config.peer_addr);
				if (cfg.name[0] && ui_tmp) {
					wsprintf(w, "%t", "PC: ");
					utf8_2ws(ui_tmp, cfg.name, 60);
					wstrcat(w, ui_tmp);
					wsprintf(ui_tmp, " (%t)", v);
					wstrcat(w, ui_tmp);
				} else wsprintf(w, "PC: %t", v);
			}
			SetMenuItemText(gui, item, w, item_n);
			return;
		}
		case M_AUTO: snprintf(b, sizeof(b), "Auto-reconnect: %s", auto_reconnect ? "on" : "off"); break;
		case M_IP: FmtIp(b, sizeof(b), "IP: ", netif.config.local_ip); break;
		case M_GW: FmtIp(b, sizeof(b), "Gateway: ", pan_server_ip()); break;
		case M_DNS: FmtIp(b, sizeof(b), "DNS: ", netif.config.dns1); break;
		case M_MTU:
			/* L2CAP tx/rx MTU (EL71: negotiated; 1691 = the MTU patch works) and the IP limit */
			snprintf(b, sizeof(b), "MTU: L2CAP %u/%u, IP %u", (unsigned)panu.neg_tx_mtu,
					 (unsigned)panu.neg_rx_mtu, bt_pan_netif_tx_max());
			break;
		case M_RX: case M_TX: {
			int rx = RowAt(SCR_MAIN, item_n) == M_RX;
			int l = snprintf(b, sizeof(b), rx ? "Received: " : "Sent: ");
			FmtBytes(b + l, sizeof(b) - l, rx ? bt_pan_rx_bytes : bt_pan_tx_bytes);
			if (Online()) {
				FmtRate(v, sizeof(v), rx ? rate_rx : rate_tx);
				l = strlen(b);
				snprintf(b + l, sizeof(b) - l, ", %s", v);
			}
			break;
		}
		case M_TEST: ItemText(gui, item_n, "Speed test..."); return;
		case M_PING: ItemText(gui, item_n, ping_stats.running ? "Ping... (running)" : "Ping..."); return;
		case M_MIN: ItemText(gui, item_n, "Minimize"); return;
		default: ItemText(gui, item_n, pan_running ? "Disconnect and exit" : "Exit"); return;
	}
	ItemText(gui, item_n, b);
}

static void TestItem(void *gui, int item_n, void *user) {
	static const char *titles[] = {"Direct (BT, UDP)", "Via stack (TCP)"};
	int r = RowAt(SCR_TEST, item_n), i = r >= T_BTN1 && r <= T_UL1;
	struct speedtest_state *t = &speedtest[i];
	char b[64], v[24];
	(void)user;
	if (r == T_HINT) { ItemText(gui, item_n, "Connect first"); return; }
	switch (r - (i ? T_BTN1 : T_BTN0)) {
		case 0: snprintf(b, sizeof(b), "%s: %s", titles[i], t->state == SPT_RUN ? "stop" : "start"); break;
		case 1:
			if (t->state == SPT_ERR) snprintf(b, sizeof(b), "  %s", t->error ? t->error : "Error");
			else snprintf(b, sizeof(b), "  %s %u%%", t->dir == SPT_DL ? "Download" : "Upload", t->progress);
			break;
		case 2: {
			unsigned dl = t->state == SPT_RUN && t->dir == SPT_DL ? t->live_bps : t->dl_bps;
			if (dl || t->state != SPT_ERR) FmtRate(v, sizeof(v), dl); else snprintf(v, sizeof(v), "-");
			snprintf(b, sizeof(b), "  Download: %s", v);
			break;
		}
		default:
			if (t->state == SPT_RUN && t->dir == SPT_DL) snprintf(v, sizeof(v), "...");
			else FmtRate(v, sizeof(v), t->state == SPT_RUN ? t->live_bps : t->ul_bps);
			snprintf(b, sizeof(b), "  Upload: %s", v);
			break;
	}
	ItemText(gui, item_n, b);
}

static void PingItem(void *gui, int item_n, void *user) {
	struct ping_stats *p = &ping_stats;
	char b[64], v[24];
	unsigned lost = p->sent - p->received;
	(void)user;
	switch (RowAt(SCR_PING, item_n)) {
		case P_BTN: ItemText(gui, item_n, p->running ? "Stop" : "Ping the PC"); return;
		case P_SIZE: snprintf(b, sizeof(b), "Size: %u B", PingSize()); break;
		case P_TO: FmtIp(b, sizeof(b), "To: ", pan_server_ip()); break;
		case P_ERR: ItemText(gui, item_n, p->error ? p->error : "Connect first"); return;
		case P_LAST:
			FmtRtt(v, sizeof(v), p->last_us);
			snprintf(b, sizeof(b), "Last: %s", p->hist_n ? v : "...");
			break;
		case P_REPLIES: snprintf(b, sizeof(b), "Replies: %u / %u", p->received, p->sent); break;
		case P_LOST: snprintf(b, sizeof(b), "Lost: %u (%u%%)", lost, p->sent ? lost * 100u / p->sent : 0); break;
		case P_AVG:
			FmtRtt(v, sizeof(v), p->received ? p->sum_us / p->received : 0);
			snprintf(b, sizeof(b), "Average: %s", v);
			break;
		case P_MINMAX: {
			char lo[24];
			FmtRtt(lo, sizeof(lo), p->min_us);
			FmtRtt(v, sizeof(v), p->max_us);
			if (strlen(lo) > 3) lo[strlen(lo) - 3] = 0; /* "12-40 ms" */
			snprintf(b, sizeof(b), "Min-max: %s-%s", lo, v);
			break;
		}
		default: { /* the last round trips in ms, newest first; x = lost */
			unsigned n = p->hist_n < 8 ? p->hist_n : 8, i;
			int l = snprintf(b, sizeof(b), "Last %u:", n);
			for (i = 0; i < n && l < (int)sizeof(b) - 8; ++i) {
				unsigned us = p->hist[(p->hist_n - 1 - i) % PING_HIST];
				if (us == PING_LOST) l += snprintf(b + l, sizeof(b) - l, " x");
				else l += snprintf(b + l, sizeof(b) - l, " %u", (us + 500) / 1000);
			}
			break;
		}
	}
	ItemText(gui, item_n, b);
}

static void PickItem(void *gui, int item_n, void *user) {
	char mac[24];
	void *item;
	WSHDR *w;
	(void)user;
	if (!peer_count || item_n >= peer_count) {
		ItemText(gui, item_n, peers_busy ? "Reading list..." : "List is empty: left = refresh");
		return;
	}
	item = AllocMenuItem(gui);
	w = AllocMenuWS(gui, 128);
	FmtMac(mac, sizeof(mac), peers[item_n].mac);
	if (peers[item_n].name[0] && ui_tmp) {
		utf8_2ws(w, peers[item_n].name, 60);
		wsprintf(ui_tmp, " (%t)", mac);
		wstrcat(w, ui_tmp);
	} else wsprintf(w, "Device %t", mac);
	SetMenuItemText(gui, item, w, item_n);
}

/* left softkey per row (the right one is fixed: Exit on the main screen, Back elsewhere) */
static const char *LeftLabel(int s, int item) {
	int r = RowAt(s, item);
	switch (s) {
		case SCR_MAIN:
			if (r == M_CONNECT) return pan_running ? (Busy() ? "" : "Disconnect") : "Connect";
			if (r == M_PC) return pan_running ? "" : "Choose";
			if (r == M_AUTO) return "Change";
			if (r == M_TEST || r == M_PING) return "Open";
			if (r == M_MIN || r == M_EXIT) return "Select";
			return "";
		case SCR_TEST:
			if (r == T_BTN0 || r == T_BTN1) return speedtest[r == T_BTN1].state == SPT_RUN ? "Stop" : "Start";
			return "";
		case SCR_PING:
			if (r == P_BTN) return ping_stats.running ? "Stop" : "Start";
			if (r == P_SIZE) return ping_stats.running ? "" : "Change";
			return "";
		default:
			return peer_count ? "Select" : peers_busy ? "" : "Refresh";
	}
}

static void SoftkeysUpdate(int s, void *gui) {
	static SOFTKEY_DESC left[SCR_N], right[SCR_N];
	const char *l = LeftLabel(s, GetCurMenuItem(gui));
	if (l == scr[s].left) return;  /* the same text: nothing to tell the firmware */
	scr[s].left = l;
	left[s].key1 = 0x0018; left[s].lgp_id = (int)l;
	right[s].key1 = 0x0001; right[s].lgp_id = (int)(s == SCR_MAIN ? "Exit" : "Back");
	SetMenuSoftKey(gui, &left[s], SET_LEFT_SOFTKEY);
	SetMenuSoftKey(gui, &right[s], SET_RIGHT_SOFTKEY);
}

static void UiKick(void);

/* rebuild the rows of a screen and give the menu its texts again; the cursor stays on its row */
static void Sync(int s) {
	SCREEN *c = &scr[s];
	void *gui = c->id ? FindGUIbyId(c->id, NULL) : NULL;
	int cur, old, i, n;
	unsigned char row[ROWS_MAX];
	if (!gui) return;
	cur = GetCurMenuItem(gui);
	old = RowAt(s, cur);
	n = Rows(s, row);
	for (i = 0; i < n && old >= 0; ++i)
		if (row[i] == old) break;
	if (old >= 0 && i < n) cur = i;
	if (cur >= n) cur = n - 1;
	if (cur < 0) cur = 0;
	memcpy(c->row, row, n);
	c->n = n;
	Sig(s, &c->sig_state, &c->sig_live);
	Menu_SetItemCountDyn(gui, n);
	SetCursorToMenuItem(gui, cur);
	c->left = NULL; /* the row under the cursor may have a new label */
	SoftkeysUpdate(s, gui);
	if (IsGuiOnTop(c->id)) RefreshGUI();
	UiKick();
}

static int ui_opening = -1;   /* the screen whose CreateMenu runs (its hooks come before the id) */

static int ScreenOf(GUI *gui) {
	int s;
	for (s = 0; s < SCR_N; ++s)
		if (scr[s].id && FindGUIbyId(scr[s].id, NULL) == (void *)gui) return s;
	return ui_opening >= 0 ? ui_opening : SCR_MAIN;
}

static void Hook(GUI *gui, int cmd) {
	int s = ScreenOf(gui);
	if (cmd == UI_CMD_FOCUS) {
		/* back on top (after a submenu, a message, another app): fresh rows and texts */
		scr[s].n = Rows(s, scr[s].row);
		Sig(s, &scr[s].sig_state, &scr[s].sig_live);
		Menu_SetItemCountDyn(gui, scr[s].n);
		scr[s].left = NULL;
	} else if (cmd == UI_CMD_REDRAW) {
		SoftkeysUpdate(s, gui);
	} else if (cmd == UI_CMD_DESTROY && s != SCR_MAIN && scr[s].id) {
		/* closed not by our Back (SubKey zeroes the id first) */
		if (s == SCR_PICK) GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_STOP, 0, 0, 0);
		scr[s].id = 0;
	}
}

/* Key of a menu: 'S' = select (left soft / centre), 'B' = back (right soft), else the key, 0 =
 * nothing. On S65 the softkeys come as an action (msg->keys = the SOFTKEY_DESC key: 0x18/0x3D
 * select, 0x01 back), KEY_DOWN RIGHT_SOFT alone did not come (s65-player, phone 2026-11-13). */
static int MenuAct(GUI_MSG *msg) {
	int k = msg->gbsmsg->submess;
#ifdef SGOLD
	if (msg->keys == 0x18 || msg->keys == 0x3D) return 'S';
	if (msg->keys == 0x01) return 'B';
#endif
	if (msg->gbsmsg->msg != KEY_DOWN) return 0;
	if (k == LEFT_SOFT || k == ENTER_BUTTON) return 'S';
	if (k == RIGHT_SOFT) return 'B';
	return k;
}

static HEADER_DESC hdr[SCR_N] = {
	{{0, 0, 0, 0}, NULL, (int)"BT PAN", (int)BT_PAN_BUILD_TAG},
	{{0, 0, 0, 0}, NULL, (int)"Speed test", LGP_NULL},
	{{0, 0, 0, 0}, NULL, (int)"Ping", LGP_NULL},
	{{0, 0, 0, 0}, NULL, (int)"Choose PC", LGP_NULL},
};
static const int softkeys[] = {SET_LEFT_SOFTKEY, SET_RIGHT_SOFTKEY, SET_MIDDLE_SOFTKEY};
static const SOFTKEY_DESC softkeys_d[] = {
	{0x0018, 0x0000, (int)""},
	{0x0001, 0x0000, (int)"Back"},
	{0x003D, 0x0000, (int)LGP_DOIT_PIC}
};
static const SOFTKEYSTAB softkeys_tab = {softkeys_d, 0};

static int MainKey(GUI *gui, GUI_MSG *msg);
static int SubKey(GUI *gui, GUI_MSG *msg);
static const MENU_DESC menu_d[SCR_N] = {
	{8, MainKey, Hook, NULL, softkeys, &softkeys_tab, MENU_FLAGS_ENABLE_TEXT_SCROLLING, MainItem, NULL, NULL, 0},
	{8, SubKey, Hook, NULL, softkeys, &softkeys_tab, MENU_FLAGS_ENABLE_TEXT_SCROLLING, TestItem, NULL, NULL, 0},
	{8, SubKey, Hook, NULL, softkeys, &softkeys_tab, MENU_FLAGS_ENABLE_TEXT_SCROLLING, PingItem, NULL, NULL, 0},
	{8, SubKey, Hook, NULL, softkeys, &softkeys_tab, MENU_FLAGS_ENABLE_TEXT_SCROLLING, PickItem, NULL, NULL, 0},
};

/* Only from a key handler of our menus or our CSM's oncreate: CreateMenu attaches the GUI to the
 * CSM whose message is being processed (s65-player: from a foreign timer -> MMI data abort). */
static void Open(int s) {
	HEADER_DESC *h = &hdr[s];
	if (scr[s].id) return;
#ifdef SGOLD
	h->rc.x = 0; h->rc.y = YDISP; h->rc.x2 = ScreenW() - 1; h->rc.y2 = YDISP + HeaderH();
#else
	memcpy(&h->rc, GetHeaderRECT(), sizeof(RECT));
#endif
	scr[s].n = Rows(s, scr[s].row);
	Sig(s, &scr[s].sig_state, &scr[s].sig_live);
	scr[s].left = NULL;
	ui_opening = s;
	scr[s].id = CreateMenu(0, 0, &menu_d[s], h, 0, scr[s].n, s == SCR_MAIN ? main_csm : NULL, NULL);
	ui_opening = -1;
}

/* ------------------------------------------------------------------------- actions */

static void PickerOpen(void) {
	peer_count = 0;
	peers_busy = true; /* until the BTPAN process answers */
	GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_START, 0, 0, 0);
	if (scr[SCR_PICK].id) Sync(SCR_PICK);
	else Open(SCR_PICK);
}

static void TestToggle(int which) {
	if (speedtest[which].state == SPT_RUN) {
		if (which == SPEEDTEST_DIRECT)
			GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_DIRECT_STOP, 0, 0, 0);
		else
			speedtest_stack_cancel();
		return;
	}
	if (!Online()) { ShowMSG(1, (int)"Connect first"); return; }
	if (speedtest_running()) return;
	if (which == SPEEDTEST_DIRECT) {
		speedtest[which].state = SPT_RUN; /* shown at once; the BTPAN process starts it */
		speedtest[which].progress = 0;
		speedtest[which].dir = SPT_DL;
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_DIRECT_START, 0, 0, 0);
	} else {
		speedtest_stack_start(pan_server_ip());
	}
}

static void PingAction(int r) {
	if (r == P_SIZE) {
		if (!ping_stats.running) ping_size_i = (ping_size_i + 1) % (int)(sizeof(ping_sizes) / sizeof(ping_sizes[0]));
		return;
	}
	if (r != P_BTN) return;
	if (ping_stats.running) {
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PING_STOP, 0, 0, 0);
		ping_stats.running = 0; /* shown at once */
	} else if (Online()) {
		ping_stats.running = 1; /* shown at once; the BTPAN process starts it */
		ping_stats.error = NULL;
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PING_START, 0, (void *)PingSize(), 0);
	} else ShowMSG(1, (int)"Connect first");
}

static void ConnectToggle(void) {
	if (Busy()) return;
	if (pan_running) {
		speedtest_stack_cancel();
		AppStopPan();
	} else if (ui_patch == PATCH_OLD) {
		ShowMSG(1, (int)"Undo the old hook patch and install the new one");
	} else if (addr_ok) {
		bt_pan_rx_bytes = bt_pan_tx_bytes = 0;
		rate_last_rx = rate_last_tx = 0;
		rate_rx = rate_tx = 0;
		AppStartPan();
	} else {
		PickerOpen();
	}
}

/* menu onkey: 0 = default (cursor), -1 = handled, 1 = close the menu */
static int MainKey(GUI *gui, GUI_MSG *msg) {
	int a = MenuAct(msg);
	if (a == RED_BUTTON) { AppMinimize(); return -1; }
	if (a == 'B') { AppExit(); return -1; }
	if (a == GREEN_BUTTON) { ConnectToggle(); Sync(SCR_MAIN); return -1; }
	if (a != 'S') return 0;
	switch (RowAt(SCR_MAIN, GetCurMenuItem(gui))) {
		case M_CONNECT: ConnectToggle(); break;
		case M_PC:
			if (pan_running) ShowMSG(1, (int)"Disconnect first");
			else PickerOpen();
			return -1;
		case M_AUTO: set_auto_reconnect(!auto_reconnect); break;
		case M_TEST: Open(SCR_TEST); return -1;
		case M_PING: Open(SCR_PING); return -1;
		case M_MIN: AppMinimize(); return -1;
		case M_EXIT: AppExit(); return -1;
		default: return -1;
	}
	Sync(SCR_MAIN);
	return -1;
}

static int SubKey(GUI *gui, GUI_MSG *msg) {
	int s = ScreenOf(gui), a = MenuAct(msg), item = GetCurMenuItem(gui), r = RowAt(s, item);
	if (a == RED_BUTTON) { AppMinimize(); return -1; }
	if (a == 'B') {
		if (s == SCR_PICK) GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_STOP, 0, 0, 0);
		scr[s].id = 0;
		return 1;
	}
	if (a != 'S') return 0;
	if (s == SCR_TEST) {
		if (r == T_BTN0 || r == T_BTN1) TestToggle(r == T_BTN1);
	} else if (s == SCR_PING) {
		PingAction(r);
	} else if (!peer_count) {
		if (!peers_busy) PickerOpen();
		return -1;
	} else if (item < peer_count) {
		save_peer_addr(peers[item].mac, peers[item].name);
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_STOP, 0, 0, 0);
		scr[s].id = 0;
		return 1;
	}
	Sync(s);
	return -1;
}

/* ------------------------------------------------------------------------- CSM */

static void UiClose(void) {
	int s;
	for (s = SCR_N - 1; s > SCR_MAIN; --s)
		if (scr[s].id) { GeneralFunc_flag1(scr[s].id, 1); scr[s].id = 0; }
	if (main_csm && scr[SCR_MAIN].id) GeneralFunc_flag1(scr[SCR_MAIN].id, 1);
	else if (main_csm) main_csm->csm.state = CSM_STATE_CLOSED;
}

static void AppQuit(void);

/* "Exit": disconnect first if needed (our SDev app must leave the stack's table before the
 * elf unloads); finish_poc then sets ui_exit_requested and the daemon's poll quits. */
static void AppExit(void) {
	speedtest_stack_cancel();
	if (pan_running) { exit_after_stop = true; AppStopPan(); return; }
	AppQuit();
}

/* Fast polling only while one of our screens is on top and something is changing on its own
 * (connecting, disconnecting, reconnect wait, PC list, speed test); otherwise once a second:
 * connected and idle, minimized or covered = one MMI timer a second. */
static bool UiFast(void) {
	int s, top = 0;
	for (s = 0; s < SCR_N; ++s)
		if (scr[s].id && IsGuiOnTop(scr[s].id)) top = 1;
	if (!top) return false;
	return Busy() || reconnect_wait || (pan_running && !Online()) || peers_busy || speedtest_running() ||
		   ui_exit_requested || quitting;
}

static void UiPoll(GBSTMR *timer);

static void UiArm(unsigned ticks) {
	if (IsTimerProc(&ui_timer)) GBS_StopTimer(&ui_timer);
	ui_interval = ticks;
	GBS_StartTimerProc(&ui_timer, ticks, UiPoll);
}

/* after a key action: a transition just began, follow it at the fast rate */
static void UiKick(void) {
	if (!ui_in_poll && dmn_csm && ui_interval != UI_POLL_FAST && UiFast()) UiArm(UI_POLL_FAST);
}

/* MMI timer of the daemon CSM (runs with or without the screen): only compares numbers; texts
 * are made by the firmware's redraw of the menu on top, and only after a change. Exit: the screen
 * goes first, then the daemon (its onclose kills the process and the elf). */
static void UiPoll(GBSTMR *timer) {
	int s, live_due;
	(void)timer;
	ui_ticks += ui_interval;
	live_due = ui_ticks - ui_live_at >= UI_LIVE_TICKS;
	if (ui_exit_requested) { ui_exit_requested = false; AppQuit(); }
	if (quitting) {
		if (main_csm && ++quit_ticks <= 8) { UiArm(UI_POLL_FAST); return; } /* <= 2 s for the screen */
		if (dmn_csm) CloseCSM(dmn_id);
		return;
	}
	if (scr_hide && main_csm) { scr_hide = false; AppMinimize(); }
	if (scr_reopen_hidden && !main_csm && !scr_pending) {
		scr_reopen_hidden = false;
		scr_hide = true;
		scr_open();
	}
	if (!pan_running && speedtest[SPEEDTEST_STACK].state == SPT_RUN) speedtest_stack_cancel();
	if (live_due) {
		uint32_t rx = bt_pan_rx_bytes, tx = bt_pan_tx_bytes;
		unsigned dt = ui_ticks - ui_live_at;
		rate_rx = rx >= rate_last_rx ? (unsigned)((uint64_t)(rx - rate_last_rx) * 216u / dt) : 0;
		rate_tx = tx >= rate_last_tx ? (unsigned)((uint64_t)(tx - rate_last_tx) * 216u / dt) : 0;
		rate_last_rx = rx;
		rate_last_tx = tx;
		ui_live_at = ui_ticks;
	}
	ui_in_poll = true;
	for (s = 0; s < SCR_N; ++s) {
		unsigned st, live;
		if (!scr[s].id || !IsGuiOnTop(scr[s].id)) continue; /* hidden: nothing to do */
		Sig(s, &st, &live);
		if (st != scr[s].sig_state || (live != scr[s].sig_live && live_due))
			Sync(s);
	}
	ui_in_poll = false;
	UiArm(UiFast() ? UI_POLL_FAST : UI_POLL_SLOW);
}

/* Two CSMs (D153, as radio D92 / btfiles D16). The firmware closes every CSM above the idle one
 * on DoIDLE: the red key on another screen (A0959400), a call dialled from a firmware number
 * screen, S65 going to sleep. One CSM with the screen and the link died that way, and the BT
 * internet with it. Now:
 *  - the daemon CSM: no GUI, made right above csm.first (below idle, out of DoIDLE's reach); it
 *    owns the poll timer, the broadcast MMI messages (second start, BCFG editor, the stack speed
 *    test's sockets) and the exit; its onMessage always returns 1 (0x6401 idle timer ignored);
 *    only "Exit" closes it (the PAN link, the BTPAN process and the ENIP netif live outside any
 *    CSM anyway: their messages go to the 1A00 process, sockets to ElfLoader's helper);
 *  - the screen CSM: the menus; may be closed by anyone at any time, the link stays; starting
 *    the elf again opens it again (or moves it to the top). Red key in it = minimize. */

static void scr_oncreate(CSM_RAM *data) {
	MAIN_CSM *csm = (MAIN_CSM *)data;
	main_csm = csm;
	scr_pending = false;
	csm->csm.state = CSM_STATE_OPEN;
	csm->csm.unk1 = 0;
	if (!ui_tmp) ui_tmp = AllocWS(64);
	zeromem(scr, sizeof(scr));
	Open(SCR_MAIN);
	csm->gui_id = scr[SCR_MAIN].id;
	if (dmn_csm && !quitting) UiArm(UI_POLL_FAST);
}

/* closed by Exit or from outside (DoIDLE): only the menus go; PAN, the daemon and its timer stay */
static void scr_onclose(CSM_RAM *data) {
	(void)data;
	if (scr[SCR_PICK].id && !quitting)
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_STOP, 0, 0, 0);
	main_csm = NULL;
	scr_pending = false;
	if (!quitting) { scr_reopen_hidden = true; if (dmn_csm) UiArm(UI_POLL_FAST); } /* D154: back hidden */
	zeromem(scr, sizeof(scr));
	if (ui_tmp) { FreeWS(ui_tmp); ui_tmp = NULL; }
}

/* Move a CSM: an MMI command (as XTask's CSMtoTop, sieelf/MySMSYS CSMswaper.c), but simpler:
 * the CSM is taken out of the list and put back right below CSM parent_id, or last (= on top)
 * for -1; the others stay where they are. XTask instead moves the CSMs above ours down to "under
 * idle" as it was at XTask's start; taken at call time that is often our minimized CSM itself
 * (D42: nothing happened). Then the top CSM's GUI gets the focus. */
#ifdef NEWSGOLD
#define MMICMD_ID(cmd) ((cmd)->id)
#else
#define MMICMD_ID(cmd) ((int)(cmd)->cmd_csm)
#endif

static int do_csm_move(CSMQ *csm_q, void *_cmd) {
	MMICMD *cmd = _cmd;
	CSM_RAM *work = FindCSM(csm_q, MMICMD_ID(cmd));
	CSM_RAM *before = cmd->parent_id == -1 ? NULL : FindCSM(csm_q, cmd->parent_id);
	int *gui;
	if (!work || work == before || (cmd->parent_id != -1 && !before))
		return 1;
	if (before ? work->next == before : work == (CSM_RAM *)csm_q->csm.last)
		return 1; /* already there */
	UnfocusGUI();
	/* out of the list */
	if (work->prev)
		((CSM_RAM *)work->prev)->next = work->next;
	else
		csm_q->csm.first = work->next;
	if (work->next)
		((CSM_RAM *)work->next)->prev = work->prev;
	else
		csm_q->csm.last = work->prev;
	/* back in: below "before", or last */
	if (before) {
		work->next = before;
		work->prev = before->prev;
		if (before->prev)
			((CSM_RAM *)before->prev)->next = work;
		else
			csm_q->csm.first = work;
		before->prev = work;
	} else {
		work->prev = csm_q->csm.last;
		work->next = NULL;
		((CSM_RAM *)csm_q->csm.last)->next = work;
		csm_q->csm.last = work;
	}
	if ((gui = ((CSM_RAM *)csm_q->csm.last)->gui_ll.last))
		FocusGUI(gui[3]);
	GBS_SendMessage(MMI_CEPID, MSG_CSM_DESTROYED, 0, 30002, 0); /* as XTask: the screens redraw */
	return 1;
}

static void csm_move(int id, int below_id) {
	CSMQ *csm_q = CSM_root()->csm_q;
	MMICMD *cmd;
	if (!FindCSMbyID(id) || !(cmd = malloc(sizeof(MMICMD))))
		return;
	zeromem(cmd, sizeof(MMICMD));
	cmd->csm_q = csm_q;
	cmd->flag1 = 5;
#ifdef NEWSGOLD
	cmd->prio = 0;
	cmd->id = id;
#else
	cmd->cmd_csm = (CSM_RAM *)id; /* SGOLD has no id field (as XTask) */
#endif
	cmd->parent_id = below_id;
	cmd->proc = do_csm_move;
	/* to the end of the MMI's command list; the MMI runs it and frees it */
	cmd->next = NULL;
	cmd->prev = csm_q->cmd.last;
	if (csm_q->cmd.last)
		((MMICMD *)csm_q->cmd.last)->next = cmd;
	else
		csm_q->cmd.first = cmd;
	csm_q->cmd.last = cmd;
}

/* started again: our screen on top */
static void csm_to_top(int id) { csm_move(id, -1); }

/* red button: our screen right below the idle screen (as XTask hides), PAN keeps running */
static void AppMinimize(void) {
	if (main_csm) csm_move(main_csm->csm.id, CSM_root()->idle_id);
}

static int scrcsm_onmessage(CSM_RAM *data, GBS_MSG *msg) {
	MAIN_CSM *csm = (MAIN_CSM *)data;
	if (msg->msg == MSG_GUI_DESTROYED) {
		int s;
		for (s = SCR_MAIN + 1; s < SCR_N; ++s)
			if ((int)msg->data0 == scr[s].id) scr[s].id = 0;
		if ((int)msg->data0 == csm->gui_id) {
			csm->gui_id = scr[SCR_MAIN].id = 0;
			csm->csm.state = CSM_STATE_CLOSED;
		}
	}
	return 1;
}

static void UiIconBar(short *num) { (void)num; } /* no taskbar icon */

static const int ui_minus11 = -11;
static uint16_t maincsm_name_body[32], dmncsm_name_body[32];
static struct {
	CSM_DESC desc;
	WSHDR name;
	struct { char tag[8]; void (*cb)(short *); } iconbar;
} MAINCSM = {
	.desc = {
		scrcsm_onmessage,
		scr_oncreate,
#ifdef NEWSGOLD
		0, 0, 0, 0,
#endif
		scr_onclose,
		sizeof(MAIN_CSM),
		1, &ui_minus11
	},
	.name = { maincsm_name_body, NAMECSM_MAGIC1, NAMECSM_MAGIC2, 0x0, 31, 0 },
	.iconbar = { "IconBar", UiIconBar }
};

/* the screen on top: made if closed, else moved there */
static void scr_open(void) {
	MAIN_CSM storage;
	CSMQ *q = CSM_root()->csm_q;
	CSM_RAM *save;
	if (main_csm) { csm_to_top(main_csm->csm.id); return; }
	if (scr_pending || quitting) return;
	scr_pending = true; /* NSG: CreateCSM is an MMI command, run later */
	zeromem(&storage, sizeof(storage));
	wsprintf(&MAINCSM.name, BT_PAN_SCREEN_NAME);
	/* the new CSM goes right above the one handling the current message (A0A149B6): from the
	 * daemon's onMessage that is below idle, so above the top one */
	save = q->current_msg_processing_csm;
	q->current_msg_processing_csm = q->csm.last;
	CreateCSM(&MAINCSM.desc, &storage, 0);
#ifndef BT_PAN_EMU /* elfloader3 keeps "current" = the last CSM made and gives it every new GUI */
	q->current_msg_processing_csm = save;
#else
	(void)save;
#endif
}

/* Exit (no link any more): the screen goes, then the daemon (UiPoll), then the elf (quit_wait) */
static void AppQuit(void) {
	if (quitting) return;
	quitting = true;
	quit_ticks = 0;
	UiClose();
	if (dmn_csm) UiArm(UI_POLL_FAST);
}

/* after the daemon closed: the elf goes once the link is down (our SDev app out of the stack's
 * table, finish_poc done) and the screen CSM is gone (its code must still be there) */
static GBSTMR quit_timer;
static void quit_wait(GBSTMR *timer) {
	(void)timer;
	if (pan_running || main_csm) {
		if (main_csm && ++quit_ticks > 16) main_csm->csm.state = CSM_STATE_CLOSED;
		GBS_StartTimerProc(&quit_timer, UI_POLL_FAST, quit_wait);
		return;
	}
	/* the device-list accessor and the BTA helper process must go before the elf unloads */
	if (process_created && GetGBSProcAddress(BT_PANU_PROCESS_CEPID)) {
		peers_stop(); /* a queued message would die with the process */
		KillGBSproc(BT_PANU_PROCESS_CEPID);
		process_created = false;
	}
	kill_elf();
}

/* Exit, or closed by anyone else (then PAN is stopped here: nothing but Exit should get here) */
static void dmn_onclose(CSM_RAM *data) {
	(void)data;
	dmn_csm = NULL;
	quitting = true;
	if (IsTimerProc(&ui_timer)) GBS_StopTimer(&ui_timer);
	UiClose();
	speedtest_stack_cancel();
	if (pan_running) { exit_after_stop = true; AppStopPan(); }
	quit_ticks = 0;
	quit_wait(NULL);
}

static void dmn_oncreate(CSM_RAM *data) {
	dmn_csm = data;
	dmn_id = data->id;
	data->state = CSM_STATE_OPEN;
	data->unk1 = 0;
	ui_patch = hooks_patch_state();
	ui_ticks = ui_live_at = 0;
	UiArm(UI_POLL_FAST);
}

static int dmn_onmessage(CSM_RAM *data, GBS_MSG *msg) {
	(void)data;
	if (GBS_MSG_ID(msg->msg) == GBS_MSG_ID(BT_PAN_MSG_SHOW) && msg->submess == BT_PAN_MSG_MAGIC) { /* started again */
		scr_reopen_hidden = scr_hide = false; /* D154: shown, not hidden */
		scr_open();
		return 1;
	}
	if (msg->msg == MSG_RECONFIGURE_REQ && msg->data0 && !strcmpi(msg->data0, cfg_path)) {
		config_load(); /* edited in the BCFG editor; a running link keeps its address */
		Sync(SCR_MAIN);
	}
	speedtest_stack_msg(msg);
	return 1; /* never closes by itself (MSG_IDLETMR_EXPIRED 0x6401 too) */
}

static struct {
	CSM_DESC desc;
	WSHDR name;
} DMNCSM = {
	.desc = {
		dmn_onmessage,
		dmn_oncreate,
#ifdef NEWSGOLD
		0, 0, 0, 0,
#endif
		dmn_onclose,
		sizeof(CSM_RAM),
		1, &ui_minus11
	},
	.name = { dmncsm_name_body, NAMECSM_MAGIC1, NAMECSM_MAGIC2, 0x0, 31, 0 },
};

/* main(): the daemon at the bottom of the CSM list (as radio / btfiles / cpumon), then the
 * screen on top. Returns the daemon's id. */
static int AppCreateUI(void) {
	CSM_RAM storage, *save;
	CSMQ *q;
	int id;
	zeromem(&storage, sizeof(storage));
	wsprintf(&DMNCSM.name, BT_PAN_DAEMON_NAME);
	LockSched();
	q = CSM_root()->csm_q;
	save = q->current_msg_processing_csm;
	q->current_msg_processing_csm = q->csm.first;
	id = CreateCSM(&DMNCSM.desc, &storage, 0);
	q->current_msg_processing_csm = save;
	UnlockSched();
	scr_open();
	return id;
}
