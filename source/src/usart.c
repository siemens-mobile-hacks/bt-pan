#include "usart.h"

#include <swilib.h>
#include "config.h"

#define USART_CLOCK_DIV 16
#define USART_MAX_FDV 511
#define USART_MAX_BG 8191

static uint32_t usart_calc_bg_fdv(uint32_t baudrate, uint32_t *bg, uint32_t *fdv);

void usart_init() {
	if (!BT_PAN_USART_LOG)
		return;
	// Disable usart IRQ
	NVIC_CON(NVIC_USART0_ABDET_IRQ) = 0;
	NVIC_CON(NVIC_USART0_TX_IRQ) = 0;
	NVIC_CON(NVIC_USART0_RX_IRQ) = 0;
	NVIC_CON(NVIC_USART0_ERR_IRQ) = 0;
	NVIC_CON(NVIC_USART0_CTS_IRQ) = 0;
	NVIC_CON(NVIC_USART0_ABDET_IRQ) = 0;
	NVIC_CON(NVIC_USART0_ABSTART_IRQ) = 0;
	NVIC_CON(NVIC_USART0_TMO_IRQ) = 0;
}

void usart_set_speed(uint32_t baudrate) {
	uint32_t bg, fdv;
	if (!BT_PAN_USART_LOG)
		return;
	usart_calc_bg_fdv(baudrate, &bg, &fdv);

	LockSched();
	USART_CLC(USART0) = 1 << MOD_CLC_RMC_SHIFT;
	USART_CON(USART0) &= ~USART_CON_CON_R;
	USART_BG(USART0) = bg;
	USART_FDV(USART0) = fdv;
	USART_CON(USART0) = USART_CON_CON_R | USART_CON_REN | USART_CON_FDE | USART_CON_M_ASYNC_8BIT;
	UnlockSched();
}

void usart_putc(char c) {
	USART_TXB(USART0) = c;
	while (!(USART_RIS(USART0) & USART_RIS_TX))
		;
	USART_ICR(USART0) |= USART_ICR_TX;
}

/* Whole line under LockSched: logs come from several processes (BTPAN, ENIP, MMI). A process
 * preempted inside usart_putc after its TXB write would otherwise find the TX flag already
 * cleared by the other writer and spin forever (b13: BTPAN hung, RX stopped, BT wedged). */
void usart_puts(char *s) {
	if (!BT_PAN_USART_LOG)
		return;
	LockSched();
	while (*s) {
		usart_putc(*s);
		s++;
	}
	UnlockSched();
}

static uint32_t usart_calc_bg_fdv(uint32_t baudrate, uint32_t *bg, uint32_t *fdv) {
	int div;
	uint32_t max_baudrate = 52000000 / USART_CLOCK_DIV;
	if (baudrate >= max_baudrate) { // Maximum baudrate
		*bg = 0;
		*fdv = 0;
		return max_baudrate;
	}

	if ((max_baudrate % baudrate) == 0) {
		// Exact baudrate
		*bg = (max_baudrate / baudrate) - 1;
		*fdv = 0;

		// Reducing BG by FDV if exceeded limit
		div = 256;
		while (*bg > USART_MAX_BG) {
			*fdv = div;
			*bg >>= 1;
			div >>= 1;
		}
	} else {
		// Baudrate with approximation
		uint32_t good_baud = max_baudrate / (max_baudrate / baudrate);
		*bg = (max_baudrate / good_baud) - 1;
		*fdv = baudrate * 512 / good_baud;

		// Reducing BG by FDV if exceeded limit
		while (*bg > USART_MAX_BG) {
			*bg >>= 1;
			*fdv >>= 1;
		}
	}

	// Real baudrate
	return *fdv ? (max_baudrate / (*bg + 1)) * *fdv / 512 : (max_baudrate / (*bg + 1));
}
