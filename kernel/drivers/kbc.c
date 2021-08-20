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
    // wait for keyboard controller input buffer to be clear;
    while(1)
        if((kbc_read_status() & KBC_STATS_MASK_IN_BUFFER) == 0)
            break;
    outb(KBC_CMD_REGISTER, cmd); //  send commmand to port 0x64;
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
    // wait for Keyboard Controller input buffer to be clear;
    while(1)
        if((kbc_read_status() & KBC_STATS_MASK_IN_BUFFER) == 0)
            break;
    // send command byte to keyboard encoder;
    outb(KB_ENDCODER_CMD_REGISTER, cmd);
}

/*
    Restart the system;
*/
void kb_reset_system()
{
    // writes 11111110 to the output port (sets reset system line low);
    kbc_send_cmd(KBC_WRITE_OUTPUT_PORT);
    kb_encoder_send_cmd(0xFE);
}

/*
    run self test
*/
uint8_t kb_self_test()
{
    // send the self test command;
    kbc_send_cmd(KBC_SELF_TEST);
    // wait for output buffer to be full
    while(1)
        if((kbc_read_status() & KBC_STATS_MASK_OUT_BUFFER) == 0)
            break;
    // if output buffer == 0x55, test passed;
    return (kb_encoder_read_buffer() == 0x55) ? true : false;
}

/*
    run interface test;
*/
uint8_t kb_interface_test()
{
    // send the interface test command;
    kbc_send_cmd(KBC_INTERFACE_TEST);
    // wait for output buffer to be full
    while(1)
        if((kbc_read_status() & KBC_STATS_MASK_OUT_BUFFER) == 0)
            break;
    // if output buffer == 
    // TODO LATER
    return 0;
}

/*
    Check the type of PS/2 device;
    returns:
        None - Ancient AT keyboard with translation enabled in the PS/Controller
         (not possible for the second PS/2 port)
        0x00 - Standard PS/2 mouse
        0x03 - Mouse with scroll wheel
        0x04 - 5-button mouse
        0xAB, 0x41 or 0xAB, 0xC1 -MF2 keyboard with translation enabled in the PS/Controller 
        (not possible for the second PS/2 port)
        0xAB, 0x83 - MF2 keyboard
*/
uint8_t kb_device_check_type()
{
    // send the disable scannig command;
    kb_encoder_send_cmd(KBE_RESET_POWER_ON_CONDITION_WAIT_TO_ENABLE_CMD);
    kb_encoder_send_cmd(KBE_SEND_2_BYTE_KB_ID);
    while(1)
        if((kbc_read_status() & KBC_STATS_MASK_OUT_BUFFER) == 0)
            break;
    return kb_encoder_read_buffer();
}