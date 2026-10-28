#pragma once

/*
 * Bluetooth address of the host adapter in the usual printed byte order.
 * Replace this value before running the PoC. All-zero disables the connect
 * attempt deliberately, so a typo cannot target an unrelated nearby device.
 */
/* PC Bluetooth adapter (printed order); the real one comes from BTPAN.bcfg. */

#ifndef BT_PANU_HOST_BDADDR
#define BT_PANU_HOST_BDADDR {0, 0, 0, 0, 0, 0}
#endif

/*
 * Address acquisition. With DHCP enabled the phone sends DISCOVER/REQUEST on
 * connect and registers the ENIP driver with the leased IP; if no server answers
 * (BT_PAN_DHCP_MAX_TRIES retransmits) it falls back to the static BT_PAN_LOCAL_IPV4
 * below - the original, device-verified path. nap.sh must run a DHCP server on pan0.
 */
#ifndef BT_PAN_USE_DHCP
#ifdef SGOLD
/* S65 with stock MTU (IP 320): a DHCP answer is >= 328 bytes of IP, the PC fragments it,
 * dhcp.c does not reassemble -> static address right away. With bt-pan-mtu_S65v58.vkp: DHCP. */
extern unsigned bt_pan_tx_max;
#define BT_PAN_USE_DHCP (bt_pan_tx_max > BT_PAN_SG_IP_MTU)
#else
#define BT_PAN_USE_DHCP 1
#endif
#endif

/* 1 = on/off screen (CSM); 0 = headless auto-start (the proven fallback if the
 * UI misbehaves on device). */
#ifndef BT_PAN_UI
#define BT_PAN_UI 1
#endif
#define BT_PAN_DHCP_RETX_TICKS (216u * 2u) /* ~2 s between retransmits */
#define BT_PAN_DHCP_MAX_TRIES 4u

#define BT_PAN_LOCAL_IPV4 {192, 168, 7, 2}
#define BT_PAN_PEER_IPV4 {192, 168, 7, 1}
/* DNS without DHCP (S65 stock MTU): the same servers nap.sh hands out; DHCP replaces them.
 * Without these the resolver has no server and apps never send a packet (D73 trace). */
#define BT_PAN_DNS1_IPV4 {8, 8, 8, 8}
#define BT_PAN_DNS2_IPV4 {1, 1, 1, 1}
/* LMAN bearer of our own context (LMAN_AllocContext). S65's takes only 0..3 and asserts on
 * anything else (8 contexts of 8 bytes; the firmware itself uses 1 = GPRS). */
#ifdef SGOLD
#define BT_PAN_BEARER_ID 3
#else
#define BT_PAN_BEARER_ID 4
#endif
/* ENIP driver bearer. 1 = GPRS: LMAN only reuses bearer-1 contexts for apps, and a socket picks
 * the driver whose (bearer, iface) equals its context's. No SIM, so no real GPRS driver to clash
 * with; with a SIM a real PDP on iface 1 could. 4 = ARMenian Developer's PoC (apps never routed). */
#define BT_PAN_DRIVER_BEARER 1
/* 1 = debug log on the cable port (USART0, 115200). It takes the port from the firmware:
 * without a cable the phone then beeps, so release builds keep it off. */
/* GBS message id as the receiver sees it: S65 (SGOLD) keeps only the low 10 bits (0x6B51 arrives
 * as 0x351, seen on the phone), so every compare goes through this. */
#ifdef SGOLD
#define GBS_MSG_ID(m) ((unsigned)(m) & 0x3ffu)
#else
#define GBS_MSG_ID(m) ((unsigned)(m) & 0xffffu)
#endif

#ifndef BT_PAN_USART_LOG
#define BT_PAN_USART_LOG 0
#endif
/* 1 = log every IPv4 packet (slow: each line holds the USART ~5 ms) */
#define BT_PAN_LOG_PACKETS 0
#define BT_PAN_IFACE_INDEX 1
/* MTU told to the phone's stack. ENIP seems to size a TCP socket's receive window as 2*MTU
 * (window 3000 seen with 1500), which caps Java/Opera that read slowly; D24+ try 3000. Real
 * packets stay <= 1500: the PC's MSS limits TCP, and send_ipv4 drops anything bigger. */
#ifdef SGOLD
#define BT_PAN_MTU BT_PAN_SG_IP_MTU
#else
#define BT_PAN_MTU 3000u
#endif

/* Keep the connection alive long enough to test from the host. 1 hour. */
#define BT_PANU_RUN_SECONDS 3600u

/* Bluetooth PAN requires an L2CAP MTU of at least 1691 bytes. */
#ifdef SGOLD
/* S65 without the MTU patch: its L2CAP sends at most 0x14F (335) bytes per SDU (BTLP_uiSendData
 * OverBT). Linux accepts the smaller MTU; the PC's bnep/pan0 MTU must then be <= IP MTU
 * below (nap.sh --mtu 320): BNEP adds up to 15 bytes per Ethernet frame. */
#define BT_PANU_RX_MTU 335u
#define BT_PANU_TX_MTU 335u
#define BT_PAN_SG_IP_MTU 320u
/* bt-pan-mtu_S65v58.vkp installed (bt_sg_mtu_patched): main.c switches to 1691 / IP 1500 */
#define BT_PAN_SG_BIG_IP_MTU 1500u
/* told to ENIP with the patch, like EL71's BT_PAN_MTU: a bigger TCP receive window for Java */
#define BT_PAN_SG_BIG_STACK_MTU 3000u
#else
#define BT_PANU_RX_MTU BT_BNEP_MIN_MTU

#define BT_PANU_TX_MTU BT_BNEP_MIN_MTU
#endif
