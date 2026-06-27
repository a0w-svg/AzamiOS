/**
 * kernel/drivers/input/kbc.c — AzamiOS PS/2 Keyboard Controller Driver
 *
 * EDUCATIONAL RELIABILITY & DEADLOCK PREVENTION EXPLANATIONS:
 * 1. Bounded Hardware Polling (Timeout Enforcements):
 *    Unbounded `while(1)` spin loops waiting on status registers can hang the entire
 *    operating system kernel if hardware fails or slows down. All polling loops now use
 *    a timeout counter (`timeout = 100000`) to guarantee progress and prevent ring 0 deadlocks.
 * 2. Correct Status Bit Polling:
 *    When waiting for the input buffer to clear before sending data, we wait while bit 1 is 1.
 *    When waiting for data from the output buffer, we wait while bit 0 is 0 (`OUT_BUFFER == 0`).
 */

#include "./include/kbc.h"
#include "../klibc/include/port.h"
#include <stdbool.h>

/*
    Read status from keyboard controller;
*/
uint8_t kbc_read_status()
{
    return inb(KBC_STATS_REGISTER); // return KBC status;
}

/*
    Send command byte to keyboard controller;
*/
void kbc_send_cmd(uint8_t cmd)
{
    int timeout = 100000;
    while (--timeout > 0) {
        if ((kbc_read_status() & KBC_STATS_MASK_IN_BUFFER) == 0)
            break;
    }
    outb(KBC_CMD_REGISTER, cmd); // send command to port 0x64;
}

/*
    read keyboard encoder buffer;
*/
uint8_t kb_encoder_read_buffer()
{
    return inb(KB_ENDCODER_INPUT_BUFFER); // return value stored in Keyboard encoder input buffer.
}

/*
    Send command byte to keyboard encoder;
*/
void kb_encoder_send_cmd(uint8_t cmd)
{
    int timeout = 100000;
    while (--timeout > 0) {
        if ((kbc_read_status() & KBC_STATS_MASK_IN_BUFFER) == 0)
            break;
    }
    outb(KB_ENDCODER_CMD_REGISTER, cmd);
}

/*
    Restart the system;
*/
void kb_reset_system()
{
    kbc_send_cmd(KBC_WRITE_OUTPUT_PORT);
    kb_encoder_send_cmd(0xFE);
}

/*
    run self test
*/
uint8_t kb_self_test()
{
    kbc_send_cmd(KBC_SELF_TEST);
    int timeout = 100000;
    while (--timeout > 0) {
        if ((kbc_read_status() & KBC_STATS_MASK_OUT_BUFFER) != 0)
            break;
    }
    if (timeout == 0) return false;
    return (kb_encoder_read_buffer() == 0x55) ? true : false;
}

/*
    run interface test;
    Returns 0x00 on success, or error code / 0xFF on timeout.
*/
uint8_t kb_interface_test()
{
    kbc_send_cmd(KBC_INTERFACE_TEST);
    int timeout = 100000;
    while (--timeout > 0) {
        if ((kbc_read_status() & KBC_STATS_MASK_OUT_BUFFER) != 0)
            break;
    }
    if (timeout == 0) return 0xFF; // timeout error
    return kb_encoder_read_buffer();
}

/*
    Check the type of PS/2 device;
*/
uint8_t kb_device_check_type()
{
    uint8_t code = 0xFF;
    kb_encoder_send_cmd(KBE_RESET_POWER_ON_CONDITION_WAIT_TO_ENABLE_CMD);
    kb_encoder_send_cmd(KBE_SEND_2_BYTE_KB_ID);
    int timeout = 100000;
    while (--timeout > 0) {
        if ((kbc_read_status() & KBC_STATS_MASK_OUT_BUFFER) != 0)
            break;
    }
    if (timeout > 0) code = kb_encoder_read_buffer();
    kb_encoder_send_cmd(0xF4);
    return code;
}