#pragma once

#include <pmb887x.h>

void usart_init();
void usart_set_speed(uint32_t baudrate);
void usart_putc(char c);
void usart_puts(char *s);
