#ifndef MEM_H
#define MEM_H

#include <stddef.h>
/*
    Fill block of memory
    Sets the first num bytes of the block of memory pointed by ptr to the specified value
    (interpreted as an unsigned char)
*/
void memset(void* ptr, int value, size_t num);
/*
    Copy block of memory
    Copies the values of num bytes from the location pointed to by source directly
    to the memory block pointed to by destination.
*/
void memcpy(void* destination, const void* source, size_t num);
/*
*/

#endif