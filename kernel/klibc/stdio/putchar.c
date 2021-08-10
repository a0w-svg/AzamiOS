#include "../include/stdio.h"
#include "../../drivers/include/terminal.h"

/*
    Function places a char;
    char ch - character to print;
*/
int putchar(char ch)
{
    return put_char(ch);
}
