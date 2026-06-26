#include "./include/mouse.h"
#include "./include/kbc.h"
#include "./include/gfx.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../arch/include/isr.h"

static volatile uint8_t mouse_cycle = 0;
static volatile int8_t mouse_bytes[3];
static mouse_state_t g_mouse_state = {160, 100, false, false, false};

mouse_state_t* mouse_get_state(void) {
    return &g_mouse_state;
}

static void mouse_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBC_STATS_REGISTER) & 2) == 0) return;
    }
}

static void mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBC_STATS_REGISTER) & 1) == 1) return;
    }
}

static void mouse_write(uint8_t val) {
    mouse_wait_write();
    outb(KBC_CMD_REGISTER, 0xD4);
    mouse_wait_write();
    outb(KB_ENDCODER_INPUT_BUFFER, val);
}

static uint8_t mouse_read(void) {
    mouse_wait_read();
    return inb(KB_ENDCODER_INPUT_BUFFER);
}

static void mouse_handler(registers_t *r) {
    UNUSED(r);
    int8_t b = inb(KB_ENDCODER_INPUT_BUFFER);

    switch(mouse_cycle) {
        case 0:
            if ((b & 0x08) == 0) return; // packet desync
            mouse_bytes[0] = b;
            mouse_cycle++;
            break;
        case 1:
            mouse_bytes[1] = b;
            mouse_cycle++;
            break;
        case 2:
            mouse_bytes[2] = b;
            mouse_cycle = 0;

            g_mouse_state.left_btn   = mouse_bytes[0] & 0x01;
            g_mouse_state.right_btn  = mouse_bytes[0] & 0x02;
            g_mouse_state.middle_btn = mouse_bytes[0] & 0x04;

            g_mouse_state.x += mouse_bytes[1];
            g_mouse_state.y -= mouse_bytes[2]; // PS/2 Y axis inverted

            if (g_mouse_state.x < 0) g_mouse_state.x = 0;
            if (g_mouse_state.x > 319) g_mouse_state.x = 319;
            if (g_mouse_state.y < 0) g_mouse_state.y = 0;
            if (g_mouse_state.y > 199) g_mouse_state.y = 199;

            if (gfx_is_enabled()) {
                gfx_on_mouse_move(g_mouse_state.x, g_mouse_state.y);
            }
            break;
    }
}

void init_mouse(void) {
    uint8_t status;

    // Enable auxiliary mouse device
    mouse_wait_write();
    outb(KBC_CMD_REGISTER, 0xA8);

    // Read KBC command byte
    mouse_wait_write();
    outb(KBC_CMD_REGISTER, 0x20);
    mouse_wait_read();
    status = inb(KB_ENDCODER_INPUT_BUFFER);

    // Set bit 1 (enable IRQ12) and clear bit 5 (disable mouse clock disable)
    status |= 2;
    status &= ~0x20;

    mouse_wait_write();
    outb(KBC_CMD_REGISTER, 0x60);
    mouse_wait_write();
    outb(KB_ENDCODER_INPUT_BUFFER, status);

    // Set defaults
    mouse_write(0xF6);
    mouse_read(); // ACK

    // Enable streaming
    mouse_write(0xF4);
    mouse_read(); // ACK

    register_interrupt_handler(IRQ12, mouse_handler);
    kprintf("mouse: PS/2 auxiliary mouse driver initialized on IRQ12\n");
}
