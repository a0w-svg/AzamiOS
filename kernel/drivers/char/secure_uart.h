/**
 * kernel/drivers/char/secure_uart.h — Hardened Concurrent UART Driver
 *
 * Exposes:
 *   - secure_uart_driver   : driver_t descriptor (register with drv_register)
 *   - secure_uart_device   : device_t instance   (register with dev_add)
 *   - secure_uart_init()   : call once at boot to wire everything up
 *   - SUART_IOCTL_* codes  : ioctl command set
 */
#ifndef SECURE_UART_H
#define SECURE_UART_H

#include "../include/device.h"
#include "../include/wait_queue.h"
#include <stdint.h>
#include <stddef.h>

/* ── UART IOCTL commands ─────────────────────────────────────────────── */
#define SUART_IOCTL_SET_BAUD    0x5401u  /* arg: uint32_t baud divisor  */
#define SUART_IOCTL_FLUSH_RX    0x5402u  /* flush RX ring buffer        */
#define SUART_IOCTL_FLUSH_TX    0x5403u  /* flush TX ring buffer        */
#define SUART_IOCTL_GET_ERRORS  0x5404u  /* arg: uint32_t* error bitmap */

/* ── Error bitmap (returned by SUART_IOCTL_GET_ERRORS) ─────────────── */
#define SUART_ERR_OVERRUN  (1u << 0)     /* RX ring buffer overrun      */
#define SUART_ERR_FRAMING  (1u << 1)     /* hardware framing error      */
#define SUART_ERR_PARITY   (1u << 2)     /* hardware parity error       */

/* ── Ring buffer capacity (power of 2 for cheap modulo) ─────────────── */
#define SUART_RX_BUF_SIZE 256u
#define SUART_TX_BUF_SIZE 256u

/* ── Private driver state embedded in device->private_data ──────────── */
typedef struct suart_state {
    uint16_t         base_port;          /* COM1=0x3F8, COM2=0x2F8 ... */

    /* RX ring buffer — written by ISR, read by read() */
    uint8_t          rx_buf[SUART_RX_BUF_SIZE];
    volatile uint32_t rx_head;           /* ISR increments              */
    volatile uint32_t rx_tail;           /* reader increments           */
    volatile int     rx_lock;            /* spinlock for rx_buf         */

    /* TX ring buffer — written by write(), drained by ISR/poll */
    uint8_t          tx_buf[SUART_TX_BUF_SIZE];
    volatile uint32_t tx_head;           /* writer increments           */
    volatile uint32_t tx_tail;           /* drainer increments          */
    volatile int     tx_lock;            /* spinlock for tx_buf         */

    /* I/O wait queues */
    wait_queue_t     rx_wq;             /* blocked readers wait here   */
    wait_queue_t     tx_wq;             /* blocked writers wait here   */

    /* Error tracking (ISR writes atomically with spinlock)              */
    volatile uint32_t error_flags;
} suart_state_t;

/* ── Exported driver and device descriptors ──────────────────────────── */
extern driver_t secure_uart_driver;
extern device_t secure_uart_device;

/**
 * secure_uart_init — register driver + device, configure hardware.
 * Returns 0 on success, -1 on failure.
 */
int secure_uart_init(void);

/**
 * secure_uart_irq_handler — call from the COM1 IRQ handler (IRQ4).
 * Drains the hardware RX FIFO into the ring buffer and wakes readers.
 */
void secure_uart_irq_handler(void);

#endif /* SECURE_UART_H */
