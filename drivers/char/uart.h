/* ============================================================================
 * AzamiOS — UART (16550) Early Console Driver
 * File: drivers/char/uart.h / uart.c
 * ============================================================================ */
#pragma once
#include "../../include/azami/types.h"

#define UART_COM1  0x3F8
#define UART_COM2  0x2F8

/* IOCTL Commands */
#define UART_SET_BAUD 0x5450

/** uart_init(port) — Initialise 16550 UART at the given I/O base port. */
void uart_init(u16 port);

/** uart_putc(port, c) — Transmit one character (busy-wait). */
void uart_putc(u16 port, char c);

/** uart_puts(port, s) — Transmit a null-terminated string. */
void uart_puts(u16 port, const char *s);

/** uart_write(port, buf, len) — Transmit len bytes. */
void uart_write(u16 port, const char *buf, size_t len);

/** uart_getc(port) — Read one character non-blocking (-1 if empty). */
int uart_getc(u16 port);

/** uart_register_devfs() — Register UART devices to devfs. */
void uart_register_devfs(void);
