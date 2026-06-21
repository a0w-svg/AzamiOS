#include "include/stdio.h"
#include "include/string.h"
#include "include/stdlib.h"
#include <stddef.h>
#include <stdarg.h>
#include <limits.h>

void syscall_putchar(char c){
    asm volatile(
        "int $128\n"
        :
        : "a"(1), "b"(c)
    );
}

void syscall_print_string(const char* str){
    asm volatile(
        "int $128\n"
        :
        : "a"(0), "b"(str)
    );
}
/*
int putchar(char c){
    syscall_putchar(c);
    return c;
}

int puts(const char *string){
    syscall_print_string(string);
    return 0;
}
*/
/*
 Print function
    flags:
    %d - displaying the decimal number from the argument of the int type;
    %s - displaying the string from the argument of the  const char* type;
    %x - displaying the hexadecimal number from the argument of the int type;
    %o - displaying the octal number from the argument of the int type;
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
                syscall_putchar(i);
                break;
            case 'd': // Fetch Decimal/Integer argument;
                i = va_arg(arg, int);
                itoa(i, buf, 10);
                syscall_print_string(buf);
                break;
            case 'o': // Fetch Octal representation;
                i = va_arg(arg, int);
                itoa(i, buf, 8);
                syscall_print_string(buf);
                break;
            case 's': // Fetch string;
                string = va_arg(arg, char*);
                if(string == NULL){
                    string = "(null)";
                }
                syscall_print_string(string);
                break;
            case 'x': // Fetch Hexadecimal representation
                i = va_arg(arg, int);
                itoa(i, buf, 16);
                syscall_print_string("0x");
                syscall_print_string(buf);
                break;
            }
        }
        else
        {
            syscall_putchar(*format);
            format++;
           
        }
    }
    va_end(arg);
}