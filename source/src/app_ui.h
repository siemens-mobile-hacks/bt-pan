#pragma once
/*
 * BT PAN screen, two tabs (left/right or 1/2):
 *   Связь - connect/disconnect, PC (device picker from the paired list), IP, traffic;
 *   Тест  - speed test directly over BT (UDP) and through the phone's stack (TCP);
 *   Ping  - ICMP echo to the PC once a second (src/ping.c), RTT, losses, a bar graph.
 * Link and Ping scroll, so they fit 132x176 (S65) as well as 240x320.
 * PAN itself runs in the separate 0x1A00 GBS process; this screen drives AppStartPan()/
 * AppStopPan() (main.c) and posts requests to that process. Included at the end of main.c.
 */
#include <swilib.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { CSM_RAM csm; int gui_id; } MAIN_CSM;
typedef struct { GUI base; WSHDR *ws; } MAIN_GUI;

#define UI_POLL_TICKS 22u       /* ~0.1 s */
#define UI_REDRAW_MIN 3u        /* <= ~3 REDRAW/s: see "no REDRAW per frame" */
#define BT_PAN_SCREEN_NAME "BT PAN"

static MAIN_CSM *main_csm;
static WSHDR *ws;
static RECT gui_rect;
static GBSTMR ui_timer;
static int gui_focused;
static int ui_redraw;
static unsigned ui_polls;
static unsigned ui_last_redraw;
#define UI_TABS 3
static int ui_tab;          /* 0 Link, 1 Test, 2 Ping */
static int ui_row[UI_TABS]; /* selected row per tab */
static int ping_size_i;     /* index into ping_sizes */
static const unsigned ping_sizes[] = {56, 512, 1472};
static int ui_picker;       /* device list open */
static int picker_row, picker_top;

static void AppExit(void);
static void AppMinimize(void);

#define RGB(r,g,b) {r,g,b,100}
static const char C_BG[4] = RGB(15,21,31), C_PANEL[4] = RGB(27,37,51);
static const char C_LINE[4] = RGB(48,62,80), C_TEXT[4] = RGB(235,241,248);
static const char C_DIM[4] = RGB(153,169,189), C_BLUE[4] = RGB(82,174,255);
static const char C_OK[4] = RGB(87,219,151), C_WAIT[4] = RGB(255,192,84);
static const char C_ERR[4] = RGB(255,113,117), C_NONE[4] = {0,0,0,0};
static unsigned ui_state_sig;

/* text, shortened with "..." when it does not fit */
static void UiFit(int x1, int y1, int x2, int y2, int font, int align, const char *color) {
	int available = x2 - x1 + 1;
	if (Get_WS_width(ws, font) > available) {
		unsigned n = ws->wsbody[0];
		if (n > (unsigned)ws->maxlen - 3) n = ws->maxlen - 3;
		do {
			if (n) --n;
			ws->wsbody[n + 1] = ws->wsbody[n + 2] = ws->wsbody[n + 3] = '.';
			ws->wsbody[0] = n + 3;
		} while (n && Get_WS_width(ws, font) > available);
	}
	DrawString(ws, x1, y1, x2, y2, font, align, color, C_NONE);
}
static void UiText(const char *s, int x1, int y1, int x2, int y2, int font, int align,
				   const char *color) {
	wsprintf(ws, "%t", s);
	UiFit(x1, y1, x2, y2, font, align, color);
}
static void UiTextUTF8(const char *s, int x1, int y1, int x2, int y2, int font, const char *color) {
	utf8_2ws(ws, s, 128);
	UiFit(x1, y1, x2, y2, font, TEXT_ALIGNLEFT, color);
}

static int FontS(void) { return GetFontYSIZE(FONT_SMALL_BOLD); }
static int FooterH(void) { return FontS() + 6; }
static int ContentTop(void) { return YDISP + GetFontYSIZE(FONT_MEDIUM_BOLD) + FontS() + 14; }
static int ContentBottom(void) { return ScreenH() - FooterH() - 3; }
static int RowH(void) { return FontS() * 2 + 8; }

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

static void UiStatus(char *buf, int n, const char **color) {
	if (!pan_running && hooks_patch_state() == PATCH_OLD) {
		snprintf(buf, n, "Old hook patch"); *color = C_ERR; return;
	}
	if (!pan_running && !addr_ok) { snprintf(buf, n, "Choose a PC"); *color = C_WAIT; return; }
	if (!pan_running) { snprintf(buf, n, "Disconnected"); *color = C_DIM; return; }
	if (reconnect_wait) { snprintf(buf, n, "Reconnecting..."); *color = C_WAIT; return; }
	switch (panu.state) {
		case BT_PANU_STATE_STARTING:
			snprintf(buf, n, "Starting BT..."); *color = C_WAIT; return;
		case BT_PANU_STATE_CONNECTING:
			snprintf(buf, n, "Connecting..."); *color = C_WAIT; return;
		case BT_PANU_STATE_CONNECTED:
			if (netif.registered) { snprintf(buf, n, "Connected"); *color = C_OK; return; }
			if (netif.dhcp_state == BT_PAN_DHCP_SELECTING ||
				netif.dhcp_state == BT_PAN_DHCP_REQUESTING) {
				snprintf(buf, n, "Getting IP..."); *color = C_WAIT; return;
			}
			snprintf(buf, n, "Connecting..."); *color = C_WAIT; return;
		case BT_PANU_STATE_STOPPING:
			snprintf(buf, n, "Disconnecting..."); *color = C_WAIT; return;
		default:
			snprintf(buf, n, "Waiting..."); *color = C_DIM; return;
	}
}

static int Online(void) { return pan_running && netif.registered; }
static int Busy(void) { return pan_running && panu.state == BT_PANU_STATE_STOPPING; }

/* A number that changes whenever anything shown on screen changes. */
static unsigned UiSig(void) {
	unsigned s = (unsigned)pan_running, i;
	s = s * 31u + (unsigned)panu.state * 2u + (unsigned)reconnect_wait;
	s = s * 31u + (unsigned)netif.registered;
	s = s * 31u + (unsigned)netif.dhcp_state;
	s = s * 31u + (unsigned)addr_ok * 2u + (unsigned)auto_reconnect;
	if (netif.registered) s = s * 31u + netif.config.local_ip;
	s = s * 31u + bt_pan_rx_bytes / 1000u;
	s = s * 31u + bt_pan_tx_bytes / 1000u;
	s = s * 31u + (unsigned)peer_count * 2u + (unsigned)peers_busy;
	for (i = 0; i < 2; ++i) {
		s = s * 31u + (unsigned)speedtest[i].state;
		s = s * 31u + speedtest[i].progress;
		s = s * 31u + speedtest[i].live_bps / 100u;
	}
	s = s * 31u + (unsigned)ping_stats.running * 2u + ping_stats.sent;
	s = s * 31u + ping_stats.hist_n + (unsigned)ping_size_i;
	s = s * 31u + (unsigned)(uintptr_t)ping_stats.error;
	return s;
}

static void Softkeys(const char *left, const char *right) {
	int fs = FontS(), y = ScreenH() - FooterH(), mid = ScreenW() / 2, ty = y + 3;
	DrawRectangle(0, y, ScreenW() - 1, ScreenH() - 1, 0, C_PANEL, C_PANEL);
	UiText(left, 6, ty, mid - 5, ty + fs - 1, FONT_SMALL_BOLD, TEXT_ALIGNLEFT, C_TEXT);
	UiText(right, mid + 5, ty, ScreenW() - 7, ty + fs - 1, FONT_SMALL_BOLD, TEXT_ALIGNRIGHT, C_DIM);
}

static void DrawHeader(void) {
	static const char *tabs[] = {"Link", "Test", "Ping"};
	int w = ScreenW(), fm = GetFontYSIZE(FONT_MEDIUM_BOLD), fs = FontS(), y, tw = w / UI_TABS, i;
	UiText("BT PAN", 7, YDISP + 2, w - 53, YDISP + fm + 1, FONT_MEDIUM_BOLD, TEXT_ALIGNLEFT, C_TEXT);
	UiText(BT_PAN_BUILD_TAG, w - 60, YDISP + 4, w - 7, YDISP + fs + 3, FONT_SMALL, TEXT_ALIGNRIGHT,
		   C_DIM);
	y = YDISP + fm + 6;
	for (i = 0; i < UI_TABS; ++i) {
		const char *c = ui_tab == i ? C_BLUE : C_DIM, *line = ui_tab == i ? C_BLUE : C_LINE;
		UiText(tabs[i], i * tw + 3, y, (i + 1) * tw - 4, y + fs - 1, FONT_SMALL_BOLD,
			   TEXT_ALIGNMIDDLE, c);
		DrawRectangle(i * tw + 5, y + fs + 2, (i + 1) * tw - 6, y + fs + 3, 0, line, line);
	}
}

/* full-width button; selected = filled */
static void Button(const char *label, int y, int selected, int enabled) {
	int w = ScreenW(), fs = FontS();
	const char *fill = !enabled ? C_LINE : selected ? C_BLUE : C_PANEL;
	DrawRoundedFrame(7, y, w - 8, y + fs + 9, 4, 4, 0, fill, fill);
	UiText(label, 14, y + 5, w - 15, y + fs + 4, FONT_SMALL_BOLD, TEXT_ALIGNMIDDLE,
		   !enabled ? C_DIM : selected ? C_BG : C_TEXT);
}

/* one-line row: label left, value right; selected = filled */
static void Row1(const char *label, const char *value, int utf8, int y, int selected, int enabled) {
	int w = ScreenW(), fs = FontS(), lw;
	const char *fg = selected ? C_BG : enabled ? C_TEXT : C_DIM;
	if (selected) DrawRoundedFrame(4, y, w - 5, y + fs + 7, 4, 4, 0, C_BLUE, C_BLUE);
	wsprintf(ws, "%t", label);
	lw = Get_WS_width(ws, FONT_SMALL_BOLD);
	DrawString(ws, 10, y + 4, w - 10, y + fs + 3, FONT_SMALL_BOLD, TEXT_ALIGNLEFT, fg, C_NONE);
	if (utf8) utf8_2ws(ws, value, 128); else wsprintf(ws, "%t", value);
	UiFit(10 + lw + 8, y + 4, w - 10, y + fs + 3, FONT_SMALL, TEXT_ALIGNRIGHT, selected ? C_BG : C_DIM);
}


/* Tabs "Link" and "Ping" scroll: items are laid out in virtual coordinates (0 = content top),
 * and only items that fit completely in the content area are drawn, so nothing runs over the
 * header or the softkeys. The last row of a tab is "details": it scrolls to the end. */
#define LINK_ROWS 4
#define PING_ROWS 3
static int tab_scroll[UI_TABS];
static int lk_draw, lk_top, lk_bottom, lk_sel_y, lk_sel_h;

/* place an item of height h at virtual y; 1 = draw it now at *sy */
static int Place(int y, int h, int row, int *sy) {
	if (row >= 0 && row == ui_row[ui_tab]) { lk_sel_y = y; lk_sel_h = h; }
	*sy = lk_top + y - tab_scroll[ui_tab];
	return lk_draw && *sy + h > lk_top - 3 && *sy < lk_bottom + 3;
}

static void FmtIp(char *buf, int size, const char *label, uint32_t ip) {
	snprintf(buf, size, "%s%u.%u.%u.%u", label, (unsigned)(ip & 0xffu), (unsigned)((ip >> 8) & 0xffu),
			 (unsigned)((ip >> 16) & 0xffu), (unsigned)((ip >> 24) & 0xffu));
}

static int LinkItems(void) {
	int w = ScreenW(), fs = FontS(), fm = GetFontYSIZE(FONT_MEDIUM_BOLD), y = 0, sy;
	char buf[48], num[24], mac[24];
	const char *col;

	UiStatus(buf, sizeof(buf), &col);
	if (Place(y, fm + 1, -1, &sy)) UiText(buf, 9, sy, w - 10, sy + fm, FONT_MEDIUM_BOLD, TEXT_ALIGNLEFT, col);
	y += fm + 4;
	if (Online()) FmtIp(buf, sizeof(buf), "IP: ", netif.config.local_ip);
	else snprintf(buf, sizeof(buf), "IP: -");
	if (Place(y, fs, -1, &sy)) UiText(buf, 9, sy, w - 10, sy + fs - 1, FONT_SMALL_BOLD, TEXT_ALIGNLEFT, Online() ? C_TEXT : C_DIM);
	y += fs + 6;
	if (!pan_running && hooks_patch_state() != PATCH_OK) {
		static const char *const old[] = {"Undo the old hook patch", "and install the new one"};
		static const char *const none[] = {"No hook patch:", "the browser won't work"};
		const char *const *t = hooks_patch_state() == PATCH_OLD ? old : none;
		int i;
		for (i = 0; i < 2; ++i) {
			if (Place(y, fs, -1, &sy)) UiText(t[i], 9, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, hooks_patch_state() == PATCH_OLD ? C_ERR : C_WAIT);
			y += fs + 2;
		}
		y += 4;
	}

	if (Place(y, fs + 10, 0, &sy))
		Button(pan_running ? (Busy() ? "Please wait..." : "Disconnect") : "Connect", sy,
			   ui_row[0] == 0, !Busy() && (pan_running || (addr_ok && hooks_patch_state() != PATCH_OLD)));
	y += fs + 17;

	FmtMac(mac, sizeof(mac), panu_config.peer_addr);
	if (Place(y, fs + 8, 1, &sy)) {
		if (!addr_ok) Row1("Computer", "choose", 0, sy, ui_row[0] == 1, !pan_running);
		else if (cfg.name[0]) Row1("Computer", cfg.name, 1, sy, ui_row[0] == 1, !pan_running);
		else Row1("Computer", mac, 0, sy, ui_row[0] == 1, !pan_running);
	}
	y += fs + 10;
	if (Place(y, fs + 8, 2, &sy))
		Row1("Auto-reconnect", auto_reconnect ? "on" : "off", 0, sy, ui_row[0] == 2, 1);
	y += fs + 12;

	if (addr_ok && cfg.name[0]) {
		snprintf(buf, sizeof(buf), "PC: %s", mac);
		if (Place(y, fs, -1, &sy)) UiText(buf, 9, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM);
		y += fs + 3;
	}
	if (Online()) {
		FmtIp(buf, sizeof(buf), "Gateway: ", pan_server_ip());
		if (Place(y, fs, -1, &sy)) UiText(buf, 9, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM);
		y += fs + 3;
		if (netif.config.dns1) {
			FmtIp(buf, sizeof(buf), "DNS: ", netif.config.dns1);
			if (Place(y, fs, -1, &sy)) UiText(buf, 9, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM);
			y += fs + 3;
		}
	}
	FmtBytes(num, sizeof(num), bt_pan_rx_bytes);
	snprintf(buf, sizeof(buf), "Received: %s", num);
	if (Place(y, fs, 3, &sy)) UiText(buf, 9, sy, w - 10, sy + fs - 1, FONT_SMALL_BOLD, TEXT_ALIGNLEFT, C_BLUE);
	y += fs + 2;
	FmtBytes(num, sizeof(num), bt_pan_tx_bytes);
	snprintf(buf, sizeof(buf), "Sent: %s", num);
	if (Place(y, fs, -1, &sy)) UiText(buf, 9, sy, w - 10, sy + fs - 1, FONT_SMALL_BOLD, TEXT_ALIGNLEFT, C_OK);
	y += fs + 2;
	return y;
}

/* end = 1: show the end whatever is selected (a running ping: the numbers, Stop on the softkey) */
static void DrawScrolled(int (*items)(void), int rows, int end) {
	int total, view, max, *scroll = &tab_scroll[ui_tab], row = ui_row[ui_tab];

	lk_top = ContentTop() + 3;
	lk_bottom = ContentBottom();
	view = lk_bottom - lk_top;
	lk_draw = 0;
	lk_sel_y = 0; lk_sel_h = 0;
	total = items();
	max = total > view ? total - view : 0;
	if (end || row == rows - 1) *scroll = max;  /* the details at the end */
	else {
		if (lk_sel_y < *scroll) *scroll = lk_sel_y;
		if (lk_sel_y + lk_sel_h > *scroll + view) *scroll = lk_sel_y + lk_sel_h - view;
		if (row == 0) *scroll = 0; /* what is above the first button */
	}
	if (*scroll > max) *scroll = max;
	if (*scroll < 0) *scroll = 0;
	lk_draw = 1;
	items();
	if (max) { /* scroll bar */
		int bh = view * view / total, by = lk_top + (view - bh) * *scroll / max;
		DrawRectangle(ScreenW() - 3, lk_top, ScreenW() - 2, lk_bottom, 0, C_PANEL, C_PANEL);
		DrawRectangle(ScreenW() - 3, by, ScreenW() - 2, by + bh, 0, C_DIM, C_DIM);
	}
}

static void DrawLink(void) {
	DrawScrolled(LinkItems, LINK_ROWS, 0);
	Softkeys(ui_row[0] == 1 ? "Select" : ui_row[0] == 2 ? "Change" : ui_row[0] == 3 ? ""
			 : pan_running ? "Disconnect" : "Connect", "Exit");
}

/* label left, value right, not selectable */
static void Pair(const char *label, const char *value, int sy, const char *color) {
	int w = ScreenW(), fs = FontS(), lw;
	wsprintf(ws, "%t", label);
	lw = Get_WS_width(ws, FONT_SMALL);
	DrawString(ws, 10, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM, C_NONE);
	wsprintf(ws, "%t", value);
	UiFit(10 + lw + 6, sy, w - 10, sy + fs - 1, FONT_SMALL_BOLD, TEXT_ALIGNRIGHT, color);
}

/* one test's results below its button, laid out like the Link tab (Place) */
static int TestResult(int which, int y) {
	struct speedtest_state *t = &speedtest[which];
	int w = ScreenW(), fs = FontS(), sy;
	char buf[48], r[24];
	if (t->state == SPT_IDLE) {
		if (Place(y, fs, -1, &sy)) UiText("Not run yet", 12, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM);
		return y + fs + 2;
	}
	if (t->state == SPT_ERR) {
		if (Place(y, fs, -1, &sy)) UiText(t->error ? t->error : "Error", 12, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_ERR);
		y += fs + 2;
	}
	if (t->state == SPT_RUN) {
		if (Place(y, fs + 3, -1, &sy)) {
			int x0 = 12, x1 = w - 10, fill = x0 + (x1 - x0) * (int)t->progress / 100;
			DrawRoundedFrame(x0, sy, x1, sy + fs + 2, 3, 3, 0, C_PANEL, C_PANEL);
			if (fill > x0 + 1) DrawRoundedFrame(x0, sy, fill, sy + fs + 2, 3, 3, 0, C_OK, C_OK);
			snprintf(buf, sizeof(buf), "%s %u%%", t->dir == SPT_DL ? "download" : "upload", t->progress);
			UiText(buf, x0, sy + 1, x1, sy + fs, FONT_SMALL_BOLD, TEXT_ALIGNMIDDLE, C_TEXT);
		}
		y += fs + 6;
	}
	{
		unsigned dl = t->state == SPT_RUN && t->dir == SPT_DL ? t->live_bps : t->dl_bps;
		if (dl || t->state != SPT_ERR) FmtRate(r, sizeof(r), dl); else snprintf(r, sizeof(r), "-");
		if (Place(y, fs, -1, &sy)) Pair("Download", r, sy, C_TEXT);
		y += fs + 2;
	}
	if (t->state == SPT_RUN && t->dir == SPT_DL) snprintf(r, sizeof(r), "...");
	else FmtRate(r, sizeof(r), t->state == SPT_RUN ? t->live_bps : t->ul_bps);
	if (Place(y, fs, -1, &sy)) Pair("Upload", r, sy, C_TEXT);
	return y + fs + 2;
}

static int TestItems(void) {
	static const char *titles[] = {"Direct (BT, UDP)", "Via stack (TCP)"};
	int fs = FontS(), y = 0, sy, i, running = speedtest_running();
	for (i = 0; i < 2; ++i) {
		const char *label = speedtest[i].state == SPT_RUN ? "Stop" : titles[i];
		if (Place(y, fs + 10, i, &sy))
			Button(label, sy, ui_row[1] == i, Online() && (!running || speedtest[i].state == SPT_RUN));
		y += fs + 14;
		y = TestResult(i, y) + 6;
	}
	if (!Online()) {
		if (Place(y, fs, -1, &sy)) UiText("Connect first", 9, sy, ScreenW() - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM);
		y += fs + 2;
	}
	return y;
}

static void DrawTest(void) {
	DrawScrolled(TestItems, 2, 0); /* the second test: scrolled to the end, its results visible */
	Softkeys(speedtest[ui_row[1]].state == SPT_RUN ? "Stop" : "Start", "Exit");
}

static unsigned PingSize(void) {
	unsigned size = ping_sizes[ping_size_i], max = ping_max_size();
	return size < max ? size : max;
}

static void FmtRtt(char *buf, int size, unsigned us) {
	if (us == PING_LOST) snprintf(buf, size, "timeout");
	else if (us < 10000) snprintf(buf, size, "%u.%u ms", us / 1000, us % 1000 / 100);
	else snprintf(buf, size, "%u ms", (us + 500) / 1000);
}

static int PingItems(void) {
	struct ping_stats *p = &ping_stats;
	int w = ScreenW(), fs = FontS(), fm = GetFontYSIZE(FONT_MEDIUM_BOLD), y = 0, sy, gh;
	char buf[40], v[24];
	unsigned n, i, lost = p->sent - p->received;

	if (Place(y, fs + 10, 0, &sy))
		Button(p->running ? "Stop" : "Ping the PC", sy, ui_row[2] == 0, p->running || Online());
	y += fs + 17;
	snprintf(v, sizeof(v), "%u B", PingSize());
	if (Place(y, fs + 8, 1, &sy)) Row1("Size", v, 0, sy, ui_row[2] == 1, !p->running);
	y += fs + 12;
	if (Online() || p->sent) {
		FmtIp(buf, sizeof(buf), "To ", pan_server_ip());
		if (Place(y, fs, -1, &sy)) UiText(buf, 9, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM);
		y += fs + 3;
	}
	if (p->error) {
		if (Place(y, fs, -1, &sy)) UiText(p->error, 9, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_ERR);
		y += fs + 3;
	} else if (!Online() && !p->sent) {
		if (Place(y, fs, -1, &sy)) UiText("Connect first", 9, sy, w - 10, sy + fs - 1, FONT_SMALL, TEXT_ALIGNLEFT, C_DIM);
		y += fs + 3;
	}
	if (!p->sent) return y;

	/* last round trip, big */
	FmtRtt(v, sizeof(v), p->last_us);
	if (Place(y, fm + 1, 2, &sy))
		UiText(p->hist_n ? v : "...", 9, sy, w - 10, sy + fm, FONT_MEDIUM_BOLD, TEXT_ALIGNLEFT,
			   !p->hist_n ? C_DIM : p->last_us == PING_LOST ? C_ERR : C_OK);
	y += fm + 4;
	snprintf(v, sizeof(v), "%u / %u", p->received, p->sent);
	if (Place(y, fs, -1, &sy)) Pair("Replies", v, sy, C_TEXT);
	y += fs + 2;
	snprintf(v, sizeof(v), "%u (%u%%)", lost, p->sent ? lost * 100u / p->sent : 0);
	if (Place(y, fs, -1, &sy)) Pair("Lost", v, sy, lost ? C_ERR : C_TEXT);
	y += fs + 2;
	if (p->received) {
		FmtRtt(v, sizeof(v), p->sum_us / p->received);
		if (Place(y, fs, -1, &sy)) Pair("Average", v, sy, C_TEXT);
		y += fs + 2;
		FmtRtt(buf, sizeof(buf), p->min_us);
		FmtRtt(v, sizeof(v), p->max_us);
		snprintf(buf + strlen(buf) - 3, sizeof(buf) - strlen(buf) + 3, "-%s", v); /* "12-40 ms" */
		if (Place(y, fs, -1, &sy)) Pair("Min-max", buf, sy, C_TEXT);
		y += fs + 2;
	}

	/* the last round trips as bars, newest right; lost = red full height */
	gh = fs * 2 + 4;
	n = p->hist_n < PING_HIST ? p->hist_n : PING_HIST;
	if (n && Place(y + 3, gh, -1, &sy)) {
		int x0 = 9, x1 = w - 10, bw = (x1 - x0 + 1) / (int)PING_HIST, top = 1;
		unsigned scale = 1;
		if (bw < 2) bw = 2;
		for (i = 0; i < n; ++i) {
			unsigned us = p->hist[(p->hist_n - 1 - i) % PING_HIST];
			if (us != PING_LOST && us > scale) scale = us;
		}
		DrawRectangle(x0, sy, x1, sy + gh - 1, 0, C_PANEL, C_PANEL);
		for (i = 0; i < n && x1 - (int)(i + 1) * bw + 1 >= x0; ++i) {
			unsigned us = p->hist[(p->hist_n - 1 - i) % PING_HIST];
			int bx1 = x1 - (int)i * bw, bx0 = bx1 - bw + 2, h;
			if (us == PING_LOST) h = gh - 2;
			else h = top + (int)((uint64_t)us * (unsigned)(gh - 2 - top) / scale);
			DrawRectangle(bx0, sy + gh - 1 - h, bx1, sy + gh - 2, 0,
						  us == PING_LOST ? C_ERR : C_BLUE, us == PING_LOST ? C_ERR : C_BLUE);
		}
	}
	y += gh + 5;
	return y;
}

static void DrawPing(void) {
	DrawScrolled(PingItems, PING_ROWS, ping_stats.running && ui_row[2] == 0);
	Softkeys(ui_row[2] == 1 ? (ping_stats.running ? "" : "Change") : ui_row[2] == 2 ? ""
			 : ping_stats.running ? "Stop" : "Start", "Exit");
}

static void PingAction(void) {
	if (ui_row[2] == 1) {
		if (!ping_stats.running) ping_size_i = (ping_size_i + 1) % (int)(sizeof(ping_sizes) / sizeof(ping_sizes[0]));
		return;
	}
	if (ui_row[2] != 0) return;
	if (ping_stats.running) {
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PING_STOP, 0, 0, 0);
		ping_stats.running = 0; /* shown at once */
	} else if (Online()) {
		ping_stats.running = 1; /* shown at once; the BTPAN process starts it */
		ping_stats.error = NULL;
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PING_START, 0, (void *)PingSize(), 0);
	}
}

static void DrawPicker(void) {
	int w = ScreenW(), fs = FontS(), y = ContentTop() + 3, visible, i;
	char mac[24];
	UiText(peers_busy ? "Reading list..." : peer_count ? "Paired devices" : "List is empty",
		   8, y, w - 8, y + fs, FONT_SMALL_BOLD, TEXT_ALIGNLEFT, C_TEXT);
	y += fs + 6;
	visible = (ContentBottom() - y - 4) / RowH();
	if (visible < 1) visible = 1;
	if (picker_row >= peer_count) picker_row = peer_count ? peer_count - 1 : 0;
	if (picker_row < picker_top) picker_top = picker_row;
	if (picker_row >= picker_top + visible) picker_top = picker_row - visible + 1;
	for (i = picker_top; i < peer_count && i < picker_top + visible; ++i) {
		FmtMac(mac, sizeof(mac), peers[i].mac);
		if (i == picker_row) DrawRoundedFrame(4, y, w - 5, y + RowH() - 3, 4, 4, 0, C_BLUE, C_BLUE);
		if (peers[i].name[0]) UiTextUTF8(peers[i].name, 10, y + 2, w - 10, y + fs + 1, FONT_SMALL_BOLD, i == picker_row ? C_BG : C_TEXT);
		else UiText("Device", 10, y + 2, w - 10, y + fs + 1, FONT_SMALL_BOLD, TEXT_ALIGNLEFT, i == picker_row ? C_BG : C_TEXT);
		UiText(mac, 10, y + fs + 4, w - 10, y + fs * 2 + 3, FONT_SMALL, TEXT_ALIGNLEFT, i == picker_row ? C_BG : C_DIM);
		y += RowH();
	}
	Softkeys(peer_count ? "Select" : "Refresh", "Back");
}

static void gui_onredraw(GUI *data) {
	static int traced;
	(void)data;
	if (traced < 2) trace(traced++ ? "R second redraw" : "R first redraw");
	DrawRectangle(0, YDISP, ScreenW() - 1, ScreenH() - 1, 0, C_BG, C_BG);
	if (ui_picker) DrawPicker();
	else if (ui_tab == 0) DrawLink();
	else if (ui_tab == 1) DrawTest();
	else DrawPing();
	/* header last: covers a scrolled item cut at the top */
	DrawRectangle(0, YDISP, ScreenW() - 1, ContentTop() + 2, 0, C_BG, C_BG);
	DrawHeader();
}

static void gui_oncreate(GUI *data, malloc_func_t alloc) {
	(void)alloc;
	trace("G gui oncreate");
	data->state = CSM_GUI_STATE_UNFOCUSED;
	((MAIN_GUI *)data)->ws = ws = AllocWS(256);
	gui_focused = 1;  /* assume focus until onunfocus says otherwise */
}
static void gui_onfocus(GUI *data, malloc_func_t alloc, mfree_func_t free_fn) {
	(void)alloc; (void)free_fn;
	data->state = CSM_GUI_STATE_FOCUSED; gui_focused = 1; ui_redraw = 1;
}
static void gui_onunfocus(GUI *data, mfree_func_t free_fn) {
	(void)free_fn;
	data->state = CSM_GUI_STATE_UNFOCUSED; gui_focused = 0;
}
static void gui_onclose(GUI *data, mfree_func_t free_fn) {
	(void)free_fn;
	data->state = CSM_GUI_STATE_CLOSED; FreeWS(((MAIN_GUI *)data)->ws); ws = NULL; gui_focused = 0;
}
static int gui_method8(void) { return 0; }
static int gui_method9(void) { return 0; }

static void PickerOpen(void) {
	ui_picker = 1;
	picker_row = picker_top = 0;
	peer_count = 0;
	peers_busy = true; /* until the BTPAN process answers */
	GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_START, 0, 0, 0);
}
static void PickerClose(void) {
	ui_picker = 0;
	GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_PEERS_STOP, 0, 0, 0);
}

static void TestToggle(int which) {
	if (speedtest[which].state == SPT_RUN) {
		if (which == SPEEDTEST_DIRECT)
			GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_DIRECT_STOP, 0, 0, 0);
		else
			speedtest_stack_cancel();
		return;
	}
	if (!Online() || speedtest_running()) return;
	if (which == SPEEDTEST_DIRECT) {
		speedtest[which].state = SPT_RUN; /* shown at once; the BTPAN process starts it */
		speedtest[which].progress = 0;
		speedtest[which].dir = SPT_DL;
		GBS_SendMessage(BT_PANU_PROCESS_CEPID, BT_PANU_MESSAGE_DIRECT_START, 0, 0, 0);
	} else {
		speedtest_stack_start(pan_server_ip());
	}
}

static void LinkAction(void) {
	if (ui_row[0] == 3) return; /* details: nothing to press */
	if (ui_row[0] == 2) {
		set_auto_reconnect(!auto_reconnect);
		return;
	}
	if (ui_row[0] == 1) {
		if (!pan_running) PickerOpen();
		return;
	}
	if (Busy()) return;
	if (pan_running) {
		speedtest_stack_cancel();
		AppStopPan();
	} else if (addr_ok) {
		bt_pan_rx_bytes = bt_pan_tx_bytes = 0;
		AppStartPan();
	} else {
		ui_row[0] = 1;
		PickerOpen();
	}
}

static int gui_onkey(GUI *data, GUI_MSG *msg) {
	int key = msg->gbsmsg->submess, type = msg->gbsmsg->msg;
	(void)data;
	if (type != KEY_DOWN && type != LONG_PRESS) return 0;
	if (ui_picker) {
		if (key == RIGHT_SOFT || key == RED_BUTTON) PickerClose();
		else if (key == UP_BUTTON) { if (picker_row > 0) --picker_row; }
		else if (key == DOWN_BUTTON) { if (picker_row + 1 < peer_count) ++picker_row; }
		else if (key == LEFT_SOFT || key == ENTER_BUTTON) {
			if (!peer_count) { if (!peers_busy) PickerOpen(); }
			else if (picker_row < peer_count) {
				save_peer_addr(peers[picker_row].mac, peers[picker_row].name);
				PickerClose();
			}
		}
		ui_redraw = 1;
		return 0;
	}
	if (type != KEY_DOWN) return 0;
	if (key == RED_BUTTON) AppMinimize();   /* "Exit" (right soft) leaves */
	else if (key == RIGHT_SOFT) AppExit();
	else if (key == LEFT_BUTTON || key == RIGHT_BUTTON)
		ui_tab = (ui_tab + (key == RIGHT_BUTTON ? 1 : UI_TABS - 1)) % UI_TABS;
	else if (key >= '1' && key < '1' + UI_TABS) ui_tab = key - '1';
	else if (key == UP_BUTTON || key == DOWN_BUTTON) {
		int rows = ui_tab == 0 ? LINK_ROWS : ui_tab == 1 ? 2 : PING_ROWS;
		ui_row[ui_tab] = (ui_row[ui_tab] + (key == DOWN_BUTTON ? 1 : rows - 1)) % rows;
	}
	else if (key == LEFT_SOFT || key == ENTER_BUTTON || key == GREEN_BUTTON) {
		if (ui_tab == 0) LinkAction(); else if (ui_tab == 1) TestToggle(ui_row[1]); else PingAction();
	}
	ui_redraw = 1;
	return 0;
}

static GUI_METHODS gui_methods = {
	gui_onredraw, gui_oncreate, gui_onclose, gui_onfocus, gui_onunfocus, gui_onkey,
	NULL, kill_data, gui_method8, gui_method9, NULL
};

static void UiShow(void) {
	if (!main_csm) return;
	if (!main_csm->gui_id) {
		MAIN_GUI *gui = malloc(sizeof(*gui));
		if (!gui) return;
		zeromem(gui, sizeof(*gui));
		StoreXYWHtoRECT(&gui_rect, 0, YDISP, ScreenW(), ScreenH() - YDISP);
		gui->base.canvas = &gui_rect;
		gui->base.methods = &gui_methods;
		gui->base.item_ll.data_mfree = mfree_adr();
		main_csm->gui_id = CreateGUI(gui);
	}
	ui_redraw = 1;
}

static void UiClose(void) {
	if (main_csm && main_csm->gui_id) GeneralFunc_flag1(main_csm->gui_id, 1);
	else if (main_csm) main_csm->csm.state = CSM_STATE_CLOSED;
}

/* "Exit": disconnect first if needed (our SDev app must leave the stack's table before the
 * elf unloads); finish_poc then sets ui_exit_requested. */
static void AppExit(void) {
	speedtest_stack_cancel();
	if (pan_running) { exit_after_stop = true; AppStopPan(); return; }
	UiClose();
}

static void UiPoll(GBSTMR *timer) {
	unsigned sig;
	(void)timer;
	++ui_polls;
#if BT_PAN_TRACE
	{ /* each ~1 s from MMI: where our code was last (see trace.h) */
		static unsigned lines, last_seq = ~0u, same;
		char t[96];

		if (ui_polls % (216u / UI_POLL_TICKS ? 216u / UI_POLL_TICKS : 1u) == 0 && lines < 500 &&
			(trace_where_seq != last_seq || ++same % 5 == 0)) {
			++lines;
			sprintf(t, "W %u seq=%u %s %X in %X", ui_polls, trace_where_seq, trace_where,
					trace_where_arg, trace_where_cepid);
			trace(t);
			last_seq = trace_where_seq;
		}
	}
#endif
	if (ui_exit_requested) { ui_exit_requested = false; UiClose(); return; }
	if (!pan_running && speedtest[SPEEDTEST_STACK].state == SPT_RUN) speedtest_stack_cancel();
	sig = UiSig();
	if (sig != ui_state_sig) { ui_state_sig = sig; ui_redraw = 1; }
	if (ui_redraw && gui_focused && (unsigned)(ui_polls - ui_last_redraw) >= UI_REDRAW_MIN) {
		ui_redraw = 0; ui_last_redraw = ui_polls; REDRAW();
	}
	GBS_StartTimerProc(&ui_timer, UI_POLL_TICKS, UiPoll);
}

static void maincsm_oncreate(CSM_RAM *data) {
	MAIN_CSM *csm = (MAIN_CSM *)data;
	trace("C csm oncreate");
	main_csm = csm;
	csm->csm.state = CSM_STATE_OPEN;
	csm->gui_id = 0;
	UiShow();
	GBS_StartTimerProc(&ui_timer, UI_POLL_TICKS, UiPoll);
	trace("C csm open");
}

static void maincsm_onclose(CSM_RAM *data) {
	(void)data;
	if (IsTimerProc(&ui_timer)) GBS_StopTimer(&ui_timer);
	main_csm = NULL;
	speedtest_stack_cancel();
	/* Closed from outside while PAN runs: our SDev app is still in the stack's table, so the
	 * elf must not unload yet. Stop like the headless build: finish_poc kills the BTA process
	 * and the elf once BT is down and the app is restored. */
	if (pan_running) {
		ui_active = false;
		AppStopPan();
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

static int maincsm_onmessage(CSM_RAM *data, GBS_MSG *msg) {
	MAIN_CSM *csm = (MAIN_CSM *)data;
	if (GBS_MSG_ID(msg->msg) == GBS_MSG_ID(BT_PAN_MSG_SHOW) && msg->submess == BT_PAN_MSG_MAGIC) { /* started again */
		usart_puts("BT PAN: started again, screen to the top\r\n");
		csm_to_top(csm->csm.id);
		return 1;
	}
	if (msg->msg == MSG_GUI_DESTROYED && (int)msg->data0 == csm->gui_id) {
		csm->gui_id = 0;
		csm->csm.state = CSM_STATE_CLOSED;
	}
	if (msg->msg == MSG_RECONFIGURE_REQ && msg->data0 && !strcmpi(msg->data0, cfg_path)) {
		config_load(); /* edited in the BCFG editor; a running link keeps its address */
		ui_redraw = 1;
	}
	speedtest_stack_msg(msg);
	return 1;
}

static void UiIconBar(short *num) { (void)num; } /* no taskbar icon */

static const int ui_minus11 = -11;
static uint16_t maincsm_name_body[32];
static struct {
	CSM_DESC desc;
	WSHDR name;
	struct { char tag[8]; void (*cb)(short *); } iconbar;
} MAINCSM = {
	.desc = {
		maincsm_onmessage,
		maincsm_oncreate,
#ifdef NEWSGOLD
		0, 0, 0, 0,
#endif
		maincsm_onclose,
		sizeof(MAIN_CSM),
		1, &ui_minus11
	},
	.name = { maincsm_name_body, NAMECSM_MAGIC1, NAMECSM_MAGIC2, 0x0, 31, 0 },
	.iconbar = { "IconBar", UiIconBar }
};

static void AppCreateUI(void) {
	MAIN_CSM storage;
	zeromem(&storage, sizeof(storage));
	wsprintf(&MAINCSM.name, BT_PAN_SCREEN_NAME);
	CreateCSM(&MAINCSM.desc, &storage, 0);
}
