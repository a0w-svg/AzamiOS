#include "./include/string.h"
#include <limits.h>
/*
    Fill block of memory
    Sets the first num bytes of the block of memory pointed by ptr to the specified value
    (interpreted as an unsigned char)
*/
void memset(void* ptr, int value, size_t num)
{
    char* pptr  = (char*)ptr;
    for(size_t i = 0; i < num; i++)
        pptr[i] = (char)value;
}

/*
    Copy block of memory
    Copies the values of num bytes from the location pointed to by source directly
    to the memory block pointed to by destination.
*/
void memcpy(void* destination, const void* source, size_t num)
{
    char* src_char = (char*)source;
    char* dest_char = (char*)destination;
    for(size_t i = 0; i < num; i++)
        dest_char[i] = src_char[i]; // copy contents byte by byte.
}

/*
    convert int type to ascii
    int base atrributes:
        d - decimal number
        x - hexadecimal number
    int d - source int type
    char* buffer - destination product
*/
void itoa(char* buf, int base, int d)
{
    char* p = buf;
    char* p1, *p2;
    unsigned long src = d;
    int divisor = 10; // decimal
    if(base == 'd' && d < 0)
    {
        *p++ = '-';
        buf++;
        src = -d;
    }
    else if(base == 'x')
        divisor = 16;
    do{
        int remainer = src % divisor;
        *p++ = (remainer < 10) ? remainer + '0' : remainer + 'A' - 10;
    }while(src /= divisor);
    *p = 0;
    p1 = buf;
    p2 = p - 1;
    while(p1 < p2)
    {
        char tmp = *p1;
        *p1 = *p2;
        *p2 = tmp;
        p1++;
        p2--;
    }
}

/*
    converts ascii to int type;
*/
int atoi(char *str)
{
    int sign = 1, base = 0, i = 0;
    // if a string contains whitespace, ignore it
    while(str[i] == ' ')
        i++;
    // check the sign of number
    if(str[i] == '-' || str[i] == '-')
        sign = 1 - 2 * (str[i++] == '-');
    // checking for valid input
    while(str[i] >= '0' && str[i] <= '9')
    {
        // handling overflow test case
        if((base > INT_MAX / 10) || (base == INT_MAX / 10 && str[i] - '0' > 7))
        {
            if(sign == 1)
                return INT_MAX;
            else INT_MIN;
        }
        base = 10 * base + (str[i++] - '0');
    }
    return base * sign;
} 

/*
    Return the string length
*/
int strlen(const char* str)
{
    int i = 0;
    while(str[i] != '\0')
        ++i;
    return i;
}

/*
    Compare two string and return 1 if them are identical or 0 if them are not identical;
*/
int strcmp(char* str1, char* str2)
{
    int i = 0, fault = 0;
    while(str1[i] != '\0' && str2[i] != '\0')
    {
        if(str1[i] != str2[i])
        {
            fault = 1;
            break;
        }
        i++;
    }
    if((str1[i] == '\0' && str2[i] != '\0') || (str1[i] != '\0' && str2[i] == '\0'))
        fault = 1;
    return fault;
}