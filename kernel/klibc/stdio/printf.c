#include "../include/stdio.h"
#include "../include/string.h"
#include <stdarg.h>
/*
 Print function
    flags:
    %d - displaying the decimal number from the argument of the int type;
    %s - displaying the string from the argument of the  const char* type;
 */
void printf(char* format, ...)
{
    char* tmp, *string;
    char buf[256];
    memset(buf, 0, sizeof(buf));
    unsigned int i;
    // arguments;
    va_list arg;
    va_start(arg, format);
    for(tmp = format; *tmp != '\0'; tmp++)
    {
        while(*tmp != '%')
        {
            putchar(*tmp);
            tmp++;
        }
        tmp++;
        // Fetching and executing arguments;
        switch (*tmp)
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
            puts(buf);
            break;
        }
    }
    va_end(arg);
}