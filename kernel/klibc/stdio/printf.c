#include "../include/stdio.h"
#include "../include/string.h"
#include <stddef.h>
#include <stdarg.h>
#include <limits.h>
/*
 Print function
    flags:
    %d - displaying the decimal number from the argument of the int type;
    %s - displaying the string from the argument of the  const char* type;
    %x - displaying the hexadecimal number from the argument of the int type;
 */
void printf(char* format, ...)
{
    char *string; // Pointer to char* argument;
    char buf[50]; // Buffer used to converts numbers to strings;
    unsigned int i = 0; // Used to store temporary int variable;
    // arguments;
    va_list arg;
    va_start(arg, format); // Create arguments list;
    while(*format != '\0')
    {
        if(*format == '%')
        {
            format++;
            // Fetching and executing arguments;
            switch (*format++)
            {
            case 'c': // fetch char argument;
                i = va_arg(arg, int);
                putchar(i);
                break;
            case 'd': // Fetch Decimal/Integer argument;
                i = va_arg(arg, int);
                itoa(buf, 'd', i);
                puts(buf);
                break;
            case 'o': // Fetch Octal representation;
                i = va_arg(arg, int);
                break;
            case 's': // Fetch string;
                string = va_arg(arg, char*);
                puts(string);
                break;
            case 'x': // Fetch Hexadecimal representation
                i = va_arg(arg, int);
                itoa(buf, 'x', i);
                puts("0x");
                puts(buf);
                break;
            }
        }
        else
        {
            putchar(*format);
            format++;
           
        }
    }
    va_end(arg);
}