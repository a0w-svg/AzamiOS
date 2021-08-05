#include "./include/string.h"

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
*/
