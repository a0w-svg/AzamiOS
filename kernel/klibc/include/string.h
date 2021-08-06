#ifndef MEM_H
#define MEM_H

#include <stddef.h>
/*
    Fill block of memory;
    Sets the first num bytes of the block of memory pointed by ptr to the specified value
    (interpreted as an unsigned char);
*/
void memset(void* ptr, int value, size_t num);
/*
    Copy block of memory;
    Copies the values of num bytes from the location pointed to by source directly
    to the memory block pointed to by destination;
*/
void memcpy(void* destination, const void* source, size_t num);
/*
    convert int type to ascii
    int base atrributes:
        d - decimal number
        x - hexadecimal number
    int d - source int type
    char* buffer - destination product
*/
void itoa(char* buf, int base, int d);
/*
    converts ascii to int type;
*/
int atoi(char *str);
/*
    Return the string length
*/
int strlen(const char* str);
/*
    Compare two string and return 1 if them are identical or 0 if them are not identical;
*/
int strcmp(char* str1, char* str2);
#endif 