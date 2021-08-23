#ifndef MEM_H
#define MEM_H

#include <stddef.h>
/*
    Fill block of memory;
    Sets the first num bytes of the block of memory pointed by ptr to the specified value
    (interpreted as an unsigned char);
*/
void* memset(void* ptr, int value, size_t num);
/*
    Copy block of memory;
    Copies the values of num bytes from the location pointed to by source directly
    to the memory block pointed to by destination;
*/
void* memcpy(void* destination, const void* source, size_t num);
/*
    Move block of memory
    Copies the values of num bytes from the location pointed by source
    to the memory block pointed by destination. Copying takes place
    as if an intermediate buffer were used, allowing the destination and source to overlap;
    The underlying type of the objects pointed by both the source and destination pointers are irrelevant for this function;
     The result is a binary copy of the data;
*/
void* memmove(void* destination, const void* source, size_t num);
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
/*
    Copy string
    Copies the C string pointed by source into the array pointed by destination,
     including the terminating null character (and stopping at that point);
*/
char* strcpy(char* destination, const char* source);
/*
    Copy characters from string
    Copies the first num characters of source to destination. 
    If the end of the source C string (which is signaled by a null-character) 
    is found before num characters have been copied, destination is
    padded with zeros until a total of num characters have been written to it;
*/
char* strncpy(char* destination, const char* source, size_t num);
#endif 