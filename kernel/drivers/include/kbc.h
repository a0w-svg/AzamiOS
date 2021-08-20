#ifndef KBC_H
#define KBC_H
#include <stdint.h>
#define KB_ENDCODER_INPUT_BUFFER 0x60 // Keyboard Encoder input buffer port;
#define KB_ENDCODER_CMD_REGISTER 0x60 // Keyboard Encoder Command Register;
#define KBC_STATS_REGISTER 0x64       // Keyboard controller status register;
#define KBC_CMD_REGISTER 0x64         // Keyboard controller command register;
enum KBC_STATS_MASK{
    KBC_STATS_MASK_OUT_BUFFER = 1,    // 00000001
    KBC_STATS_MASK_IN_BUFFER = 2,     // 00000010
    KBC_STATS_MASK_SYSTEM = 4,        // 00000100
    KBC_STATS_MASK_CMD_DATA = 8,      // 00001000
    KBC_STATS_MASK_LOCKED = 0x10,     // 00010000
    KBC_STATS_MASK_AUX_BUFFER = 0x20, // 00100000
    KBC_STATS_MASK_TIMEOUT = 0x40,    // 01000000
    KBC_STATS_MASK_PARITY = 0x80      // 10000000
};

enum KB_ENC_CMD{
    KBE_SET_LED = 0xED,
    KBE_ECHO_CMD = 0xEE,
    KBE_SET_ALT_SCANCODE_SET = 0xF0,
    KBE_SEND_2_BYTE_KB_ID = 0xF2,
    KBE_SET_AUTOREPEAT_DELAY_AND_REPEAT_RATE = 0xF3,
    KBE_ENABLE_KB = 0xF4,
    KBE_RESET_POWER_ON_CONDITION_WAIT_TO_ENABLE_CMD = 0xF5,
    KBE_RESET_POWER_ON_CONDITION_AND_BEGIN_SCANNING_KB = 0xF6,
    KBE_SET_ALL_KEYS_TO_AUTOREPEAT_PS2_ONLY  = 0xF7,
    KBE_SET_ALL_KEYS_TO_SEND_MAKE_CODE_AND_BREAK_CODE_PS2_ONLY = 0xF8,
    KBE_SET_ALL_KEYS_TO_GEN_ONLY_MAKE_CODES = 0xF9,
    KBE_SET_ALL_KEYS_TO_AUTOREPAT_AND_GEN_MAKE_BREAK_CODES = 0xFA,
    KBE_SET_A_SINGLE_KEY_TO_AUTOREPEAT = 0xFB,
    KBE_SET_A_SINGLE_KEY_TO_GEN_MAKE_AND_BREAK_CODES = 0xFC,
    KBE_SET_A_SINGLE_KEY_TO_GEN_ONLY_BREAK_CODES = 0xFD,
    KBE_RESEND_LAST_RESULT = 0xFE,
    KBE_RESET_KB_TO_POWER_ON_STATE_AND_START_SELF_TEST = 0xFF
};

enum KBC_CMD{ // Onboard Keyboard Controller Commands
    // Common Commands
    KBC_READ_CMD_BYTE = 0x20, // Read command byte
    KBC_WRITE_CMD_BYTE = 0x60, // Write command byte
    KBC_SELF_TEST = 0xAA, // self test
    KBC_INTERFACE_TEST = 0xAB, // interface test
    KBC_DISABLE_KB = 0xAD, // disable keyboard
    KBC_ENABLE_KBC_KB = 0xAE, // enable keyboard 
    KBC_READ_INPUT_PORT = 0xC0, // read  input port
    KBC_READ_OUTPUT_PORT = 0xD0,
    KBC_WRITE_OUTPUT_PORT = 0xD1,
    KBC_READ_TEST_INPUTS = 0xE0,
    KBC_SYSTEM_RESET = 0xFE,
    KBC_DISABLE_MOUSE_PORT = 0xA7,
    KBC_ENABLE_MOUSE_PORT = 0xA8,
    KBC_TEST_MOUSE_PORT = 0xA9,
    KBC_WRITE_TO_MOUSE = 0xD4,
    /* 
    Non Standard Commands:
            |
    0x00-0x1F "Read controller RAM"
    0x20-0x3F "Read controller RAM"
    0x40-0x5F "Write controller RAM"
    0x60-0x7F "Write controller RAM"
    0x90-0x93 "Synaptics Multiplexer Prefix"
    0x90-0x9F "Write port  13-Port 10"
    */
    KBC_READ_COPYRIGHT = 0xA0,
    KBC_READ_FIRMWARE_VER = 0xA1,
    KBC_CHANGE_SPEED = 0xA2,
    KBC_CHANGE_SPEED2 = 0xA3,
    KBC_CHECK_IF_PASSWORD_IS_INSTALLED = 0xA4,
    KBC_LOAD_PASSWORD = 0xA5,
    KBC_CHECK_PASSWORD = 0xA6,
    KBC_DISAGNOSTIC_DUMP = 0xAC,
    KBC_READ_KB_VERSION = 0xAF,
    // 0xB0-0xB5 "Reset controller line"
    // 0xB8-0xBD "Set controller line"
    KBC_CONTINUOUS_IN_PORT_POLL_LOW = 0xC1,
    KBC_CONTINUOUS_IN_PORT_POLL_HIGH = 0xC2,
    KBC_UNBLOCK_CONTROLLER_LINES_P22_AND_P23 = 0xC8,
    KBC_BLOCK_CONTROLLER_LINES_P22_AND_P23 = 0xC9,
    KBC_READ_CONTROLLER_MODE = 0xCA,
    KBC_WRITE_CONTROLLER_MODE = 0xCB,
    KBC_WRITE_OUTPUT_BUFFER = 0xD2,
    KBC_WRITE_MOUSE_OUTPUT_BUFFER = 0xD3,
    KBC_DISABLE_A20_LINE = 0xDD,
    KBC_ENABLE_A20_LINE = 0xDF,
    // 0xF0-0xFF "Pulse output bit"
};

uint8_t kbc_read_status();
/*
    Send command byte to keyboard controller;
*/
void kbc_send_cmd(uint8_t cmd);
/*
    read keyboard encoder buffer;
*/
uint8_t kb_encoder_read_buffer();
/*
    Send command byte to keyboard encoder;
*/
void kb_encoder_send_cmd(uint8_t cmd);
/*
    Restart the system;
*/
void kb_reset_system();
/*
    run self test
*/
uint8_t kb_self_test();
/*
    run interface test;
*/
uint8_t kb_interface_test();
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
uint8_t kb_device_check_type();
#endif