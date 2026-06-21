#include "./include/string.h"
#include <limits.h>
#include <stdint.h>
/*
    Fill block of memory
    Sets the first num bytes of the block of memory pointed by ptr to the specified value
    (interpreted as an unsigned char)
*/
void* memset(void* s, int c, size_t n)
{
    uint8_t* p  = (uint8_t*)s;
    while(n--){
        *p++ = (uint8_t)c;
    }
    return s;
}

/*
    Copy block of memory
    Copies the values of num bytes from the location pointed to by source directly
    to the memory block pointed to by destination.
*/
void* memcpy(void* dest, const void* src, size_t n) {
	uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while(n--){
        *d++ = *s++;
    }
    return dest;
}
/*
    Move block of memory
    Copies the values of num bytes from the location pointed by source
    to the memory block pointed by destination. Copying takes place
    as if an intermediate buffer were used, allowing the destination and source to overlap;
    The underlying type of the objects pointed by both the source and destination pointers are irrelevant for this function;
     The result is a binary copy of the data;
*/
void* memmove(void* dest, const void* src, size_t n){
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    // if regions doesn't collide with each other then use fast memcpy
    if(d <=  s || d >= (s + n)){
        return memcpy(dest,src,n);
    }
    // if regions collide with each other (d > s), we have to copy from end,
    // in order to not override data that we haven't copied yet.
    d += n;
    s += n;
    while(n--){
        *--d = *--s;
    }
    return dest;
}

/*
    Return the string length
*/
int strlen(const char* s)
{
   size_t len = 0;
   while(s[len]){
    len++;
   }
   return len;
}

/*
    Compare two string and return 1 if them are identical or 0 if them are not identical;
*/
int strcmp(const char* s1, const char* s2)
{
    while(*s1 && (*s1 == *s2)){
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/*
    Copy string
    Copies the C string pointed by source into the array pointed by destination,
     including the terminating null character (and stopping at that point);
*/
char* strcpy(char* dest, const char* src){
    char* d = dest;
    while((*d++ = *src++));
    return dest;
}
/*
    Copy characters from string
    Copies the first num characters of source to destination. 
    If the end of the source C string (which is signaled by a null-character) 
    is found before num characters have been copied, destination is
    padded with zeros until a total of num characters have been written to it;
*/
char* strncpy(char* destination, const char* source, size_t num)
{
    char* begin = destination;
    while (num > 0 && *source) {
        *destination++ = *source++;
        num--;
    }
    while (num > 0) { // Wypełnienie reszty zerami (wymóg standardu)
        *destination++ = '\0';
        num--;
    }
    return begin;
}

int strncmp(const char* s1, const char* s2, size_t n){
    while(n && *s1 && (*s1 == *s2)){
        s1++;
        s2++;
        n--;
    }
    return (n == 0) ? 0 : (*(const unsigned char*)s1 - *(const unsigned char*)s2);
}

