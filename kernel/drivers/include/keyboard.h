#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdint.h>
#include <stdbool.h>

/*
    returns scroll lock state
*/
bool kb_get_scroll_lock();
/*
    returns numlock state
*/
bool kb_get_numlock();
/*
    returs caps lock state
*/
bool kb_get_capslock();
/*
    returns status of control key
*/
bool kb_get_ctrl();
/*
    returns status of alt key
*/
bool kb_get_alt();
/*
    returns status of shift key
*/
bool kb_get_shift();
/*
    tells driver to ignore last resend request
*/
void kb_ignore_resend();
/*
    return if system should redo last commands
*/
bool kb_check_resend();
/*
    return BAT test result
*/
bool kb_get_bat_res();
/*
    Read status from keyboard controller;
*/
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
    Sets leds
*/
void kb_set_leds(bool num_led, bool caps_led, bool scroll_led);
/*
    run self test
*/
bool kb_self_test();
/*
    Set alternatate scan code set (PS/2 only);
*/
void kb_set_alt_scancode_set(uint8_t scancode_set);
/*
    disable keyboard
*/
void kb_disable_keyboard();
/*
    enables the keyboard;
*/
void kb_enable_keyboard();
/*
    Restart the system;
*/
void kb_reset_system();
/*  
    Initialize the keyboard;
*/
void init_keyboard();
#endif