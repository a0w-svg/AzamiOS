#include "../include/stdio.h"
#include "../../drivers/include/terminal.h"
/*
    Function places a string on the screen;
    const char* string - the string you want to display;
*/
int puts(const char* string)
{
    return terminal_write(string);
}