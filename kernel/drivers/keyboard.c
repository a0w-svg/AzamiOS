#include "./include/keyboard.h"
#include "../klibc/include/port.h"
#include "../arch/include/isr.h"
#include "./include/kbc.h"
#include <stdbool.h>
#include <stdint.h>
#include "./include/terminal.h"
// KBC constans
#define ESCAPE 1
#define SHIFT 42
#define SHIFT_RELEASE -86
#define CAPSLOCK 58
#define BACKSPACE 14
#define PAGEUP 73
#define PAGEDOWN 81
// current scancode;
static uint8_t scancode;
// lock keys;
static bool numlock, scroll_lock;
uint8_t capslock = 0;
// shift, alt, and ctrl keys current state;
static bool shift, alt, ctrl;
// set if  Basic Assurance Test(BAT) failed;
static bool kb_bat_res = false;
// set if  diagnostics failed;
static bool kb_diag_res = false;
// set if system should resend last command;
static bool kb_resend_res = false;
// set if keyboard is disabled
static bool kb_disable = false;



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
    returns scroll lock state
*/
bool kb_get_scroll_lock()
{
    return scroll_lock;
}

/*
    returns numlock state
*/
bool kb_get_numlock()
{
    return numlock;
}

/*
    returs caps lock state
*/
bool kb_get_capslock()
{
    return (capslock > 0) ? true : false;
}

/*
    returns status of control key
*/
bool kb_get_ctrl()
{
    return ctrl;
}

/*
    returns status of alt key
*/
bool kb_get_alt()
{
    return alt;
}

/*
    returns status of shift key
*/
bool kb_get_shift()
{
    return (shift > 0) ? true : false;
}

/*
    tells driver to ignore last resend request
*/
void kb_ignore_resend()
{
    kb_resend_res = false;
}

/*
    return if system should redo last commands
*/
bool kb_check_resend()
{
    return kb_resend_res;
}

/*
    return diagnostics test result
*/
bool kb_get_diagnostic_res()
{
    return kb_diag_res;
}

/*
    return BAT test result
*/
bool kb_get_bat_res()
{
    return kb_bat_res;
}

/*
    returs true if keyboard is disabled;
*/
bool kb_is_disabled()
{
    return kb_disable;
}

/*
    return last scancode;
*/
uint8_t kb_get_last_scan()
{
    return scancode;
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
        if(keycode < 0) return; // error
        char ch;
        if(shift || capslock)
            ch = upper_ascii[keycode];
        else 
        {
            ch = small_ascii[keycode];
        }
        scancode = ch;
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
    int8_t scan_code;
    outb(0x20, 0x20); // send the EoI;
    status = kbc_read_status(); // get kbc status;
    if(status & KBC_STATS_MASK_OUT_BUFFER) // if status is KBC_STATS_MASK_OUT_BUFFER, get data from 0x60 port;
        scan_code = inb(0x60);
    key_handler(scan_code);
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