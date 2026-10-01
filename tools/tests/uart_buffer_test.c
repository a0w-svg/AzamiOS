/* Exercise the real ring-buffer fops with simulated locks, I/O and scheduler.
 * Run with: sh tools/tests/uart-buffer-test.sh
 */
#include "../../drivers/char/uart.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/uaccess.h"

static unsigned locks, blocks;
#define spinlock_lock_irqsave(lock) ((void)(lock), locks++, (irqflags_t)0)
#define spinlock_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define inb(port) ((void)(port), (u8)0)
#define outb(port, value) ((void)(port), (void)(value))
#define sched_current_thread() ((thread_t *)0)
#define sched_block(state) ((void)(state), blocks++)
#define sched_unblock(thread) ((void)(thread))
#include "../../drivers/char/uart.c"

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

int main(void)
{
    uart_port_t port = { .tx_running = true };
    file_t file = { .private_data = &port, .f_flags = O_NONBLOCK };
    char data[RING_BUFFER_SIZE];
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (char)i;
    CHECK(uart_fops_read(&file, data, 1, NULL) == -EAGAIN);
    CHECK(uart_fops_poll(&file) == POLLOUT);

    port.rx_tail = RING_BUFFER_SIZE - 2;
    port.rx_head = 2;
    port.rx_buf[RING_BUFFER_SIZE - 2] = 'a';
    port.rx_buf[RING_BUFFER_SIZE - 1] = 'b';
    port.rx_buf[0] = 'c';
    port.rx_buf[1] = 'd';
    CHECK(uart_fops_poll(&file) == (POLLIN | POLLOUT));
    char readback[8];
    locks = 0;
    CHECK(uart_fops_read(&file, readback, sizeof(readback), NULL) == 4);
    CHECK(readback[0] == 'a' && readback[1] == 'b' &&
          readback[2] == 'c' && readback[3] == 'd');
    CHECK(locks == 3 && port.rx_tail == port.rx_head);

    port.tx_head = port.tx_tail = RING_BUFFER_SIZE - 2;
    u64 offset = 0;
    locks = 0;
    CHECK(uart_fops_write(&file, data, sizeof(data), &offset) ==
          RING_BUFFER_SIZE - 1);
    CHECK(locks == 3 && offset == RING_BUFFER_SIZE - 1);
    CHECK(uart_fops_poll(&file) == 0);
    CHECK(uart_fops_write(&file, data, 1, &offset) == -EAGAIN);
    CHECK(offset == RING_BUFFER_SIZE - 1 && blocks == 0);
    for (unsigned i = 0; i < RING_BUFFER_SIZE - 1; i++)
        CHECK(port.tx_buf[(RING_BUFFER_SIZE - 2 + i) % RING_BUFFER_SIZE]
              == data[i]);
    port.tx_tail = (port.tx_tail + 1) % RING_BUFFER_SIZE;
    CHECK(uart_fops_poll(&file) == POLLOUT);
    CHECK(uart_fops_write(&file, data, 1, &offset) == 1);
    CHECK(offset == RING_BUFFER_SIZE && blocks == 0);
    return 0;
}
