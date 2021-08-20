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
    return diagnostics test result
*/
bool kb_get_diagnostic_res();
/*
    return BAT test result
*/
bool kb_get_bat_res();
/*
    returs true if keyboard is disabled;
*/
bool kb_is_disabled();
/*
    return last char;
*/
uint8_t kb_getchar();
/*
    Read status from keyboard controller;
*/
void kb_set_leds(bool num_led, bool caps_led, bool scroll_led);

/*
    disable keyboard
*/
void kb_disable_keyboard();
/*
    enables the keyboard;
*/
void kb_enable_keyboard();
/*  
    Initialize the keyboard;
*/
void init_keyboard();
#endif