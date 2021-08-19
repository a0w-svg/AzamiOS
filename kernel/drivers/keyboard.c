#include "./include/keyboard.h"
#include "../klibc/include/port.h"
#include "../arch/include/isr.h"
#include <stdbool.h>
#include <stdint.h>
#include "./include/terminal.h"
// KBC constans
#define KB_ENDCODER_INPUT_BUFFER 0x60 // Keyboard Encoder input buffer port;
#define KB_ENDCODER_CMD_REGISTER 0x60 // Keyboard Encoder Command Register;
#define KBC_STATS_REGISTER 0x64       // Keyboard controller status register;
#define KBC_CMD_REGISTER 0x64         // Keyboard controller command register;
#define ESCAPE 1
#define SHIFT 42
#define SHIFT_RELEASE -86
#define CAPSLOCK 58
#define BACKSPACE 14
#define PAGEUP 73
#define PAGEDOWN 81
// current scancode set
static char scancode_set;
// current scancode;
static char scancode;
// lock keys;
static bool numlock, scroll_lock;
uint8_t capslock = 0;
// shift, alt, and ctrl keys current state;
static bool shift, alt, ctrl;
// set if keyboard error;
static int kb_error = 0;
// set if  Basic Assurance Test(BAT) failed;
static bool kb_bat_res = false;
// set if  diagnostics failed;
static bool kb_diag_res = false;
// set if system should resend last command;
static bool kb_resend_res = false;
// set if keyboard is disabled
static bool kb_disable = false;

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

static uint8_t small_ascii[128] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 
    0, // backspace
    '\t', // tab
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', // Enter
    0, // control
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, // left shift
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, // alt
    ' ', // space bar
    0, // Caps lock
    0, // 59 - F1 key ... > 
    0, 0, 0, 0, 0, 0, 0, 0,
    0, // < ... F10
    0, // 69 - Num lock
    0, // Scroll lock
    0, // Home key
    0, // Up Arrow
    0, // Page up
    '-',
    0, // left arrow
    0,
    0, // right arrow
    '+',
    0, // 79 - End key
    0, // Down Arrow
    0, // Page down
    0, // Insert Key
    0, // Delete key
    0, 0, 0,
    0, // F11 key
    0, // F12 key
    0, // All other keys are undefined
};

static uint8_t upper_ascii[128] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    0, // backspace
    '\t', // Tab
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', // Enter
    0, // Control
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '|', '~', 0, // left shift
    '\\', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, // alt
    ' ', // space bar
    0, // Caps lock
    0, // 59 - F1 key ... >
    0, 0, 0, 0, 0, 0, 0, 0,
    0, // < ... F10
    0, // 69 - Num lock
    0, // Scroll lock
    0, // Home key
    0, // Page up
    '-',
    0, // Left arrow
    0, 
    0, // Right arrow
    '+',
    0, // 79 - End key
    0, // Down arrow
    0, // Page down
    0, // Insert key
    0, // Delete key
    0, 0, 0,
    0, // F11 key
    0, // F12 key
    0 // All other keys are undefined
};
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
    Sets leds
*/
void kb_set_leds(bool num_led, bool caps_led, bool scroll_led)
{
    uint8_t leds_data = 0;
    // set or clear the bit;
    leds_data = (scroll_led) ? (leds_data | 1) : (leds_data & 1);
    leds_data = (num_led) ? (num_led | 2) : (num_led & 2);
    leds_data = (caps_led) ? (num_led | 4) : (num_led & 4);
    // send the command - update keyboard light emetting leds;
    kb_encoder_send_cmd(KBE_SET_LED);
    kb_encoder_send_cmd(leds_data);
}

/*
    run self test
*/
bool kb_self_test()
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

}

/*
    Set alternatate scan code set (PS/2 only);
*/
void kb_set_alt_scancode_set(uint8_t scancode_set)
{

}

/*
    disables the keyboard;
*/
void kb_disable_keyboard()
{
    kbc_send_cmd(KBC_DISABLE_KB);
    kb_disable = true;
}

/*
    enables the keyboard;
*/
void kb_enable_keyboard()
{
    kbc_send_cmd(KBC_ENABLE_KBC_KB);
    kb_disable = false;
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
    Key Handler
*/
void key_handler(int32_t keycode)
{
    switch (keycode)
    {
    case SHIFT:
        shift = true;
        break;
    case SHIFT_RELEASE:
        shift = false;
        break;
    case CAPSLOCK:
        if(capslock > 0)
            capslock = 0;
        else
        {
            capslock = 1;
        }
        break;
    default:
        if(keycode < 0) return;
        char ch;
        if(shift || capslock)
            ch = upper_ascii[keycode];
        else 
        {
            ch = small_ascii[keycode];
        }
        char s[2] = {ch, '\0'};
        terminal_write(s);
        break;
    }
}
/*
    Keyboard handler;
*/
static void keyboard_handler()
{
    uint8_t status;
    int8_t scancode;
    outb(0x20, 0x20); // send the EoI;
    status = kbc_read_status();
    if(status & KBC_STATS_MASK_OUT_BUFFER)
        scancode = inb(0x60);
    key_handler(scancode);
}

/*  
    Initialize the keyboard;
*/
void init_keyboard()
{
    // install keyboard interrupt handler 
    register_interrupt_handler(IRQ1, keyboard_handler);
    kb_bat_res = true;
    scancode = 0;
    numlock = false;
    capslock = 0;
    scroll_lock = false;
    kb_set_leds(false, false, false);
    shift = false;
    alt = false;
    ctrl = false;
}