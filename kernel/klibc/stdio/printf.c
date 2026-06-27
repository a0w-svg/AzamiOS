#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../../arch/include/spinlock.h"
#include <stddef.h>
#include <stdarg.h>
#include <limits.h>
/*
 Print function
    flags:
    %d - displaying the decimal number from the argument of the int type;
    %s - displaying the string from the argument of the  const char* type;
    %x - displaying the hexadecimal number from the argument of the int type;
    %o - displaying the octal number from the argument of the int type;
 */
 static volatile int kprintf_lock = 0;
void kprintf(const char* format, ...)
{
    spinlock_acquire(&kprintf_lock);
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
                itoa(i, buf, 10);
                puts(buf);
                break;
            case 'o': // Fetch Octal representation;
                i = va_arg(arg, int);
                itoa(i, buf, 8);
                puts(buf);
                break;
            case 's': // Fetch string;
                string = va_arg(arg, char*);
                if(string == NULL){
                    string = "(null)";
                }
                puts(string);
                break;
            case 'x': // Fetch Hexadecimal representation
                i = va_arg(arg, int);
                itoa(i, buf, 16);
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
    spinlock_release(&kprintf_lock);
}