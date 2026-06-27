#ifndef MEM_H
#define MEM_H

#include <stddef.h>
#ifndef NULL
#define NULL ((void*)0)
#endif
/*
    Fill block of memory;
    Sets the first num bytes of the block of memory pointed by ptr to the specified value
    (interpreted as an unsigned char);
*/
void* memset(void* s, int c, size_t n);
/*
    Copy block of memory;
    Copies the values of num bytes from the location pointed to by source directly
    to the memory block pointed to by destination;
*/
void* memcpy(void* dest, const void* src, size_t n);

int memcmp(const void* s1, const void* s2, size_t n);
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
size_t strlen(const char* s);
/*
    Compare two string and return 1 if them are identical or 0 if them are not identical;
*/
int strcmp(const char* s1, const char* s2);
/*
    Copy string
    Copies the C string pointed by source into the array pointed by destination,
     including the terminating null character (and stopping at that point);
*/
char* strcpy(char* dest, const char* src);
/*
    Copy characters from string
    Copies the first num characters of source to destination. 
    If the end of the source C string (which is signaled by a null-character) 
    is found before num characters have been copied, destination is
    padded with zeros until a total of num characters have been written to it;
*/
char* strncpy(char* destination, const char* source, size_t num);

int strncmp(const char* s1, const char* s2, size_t n);

#endif 