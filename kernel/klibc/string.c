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
    // try copying whole words (4 bytes) if addresses are aligned.
    uint32_t* d32 = (uint32_t*)d;
    const uint32_t* s32 = (const uint32_t*)s;
    
    while(n >= 4){
        *d32++ = *s32++;
        n -= 4;
    }

    // finish remain part (when n < 4)
    d = (uint8_t*)d32;
    s = (const uint8_t*)s32;
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
    convert int type to ascii
    int base atrributes:
        d - decimal number
        x - hexadecimal number
    int d - source int type
    char* buffer - destination product
*/
char* itoa(int value, char* str, int base)
{
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    int is_negative = 0;

    // zero handler
    if(value == 0){
        *ptr++ = '0';
        *ptr = '\0';
        return str;
    }

    // negative numbers for decimal system support
    if(value < 0 && base == 10){
        is_negative = 1;
        value = -value;
    }
    
    // digit generator
    while(value != 0){
        int rem = value % base;
        *ptr++ = (rem > 9) ? (rem - 10) + 'A' : rem + '0';
    }
    // add negative sign 
    if(is_negative){
        *ptr++ = '-';
    }

    *ptr = '\0';

    // reverse string
    ptr--;
    while(ptr1 < ptr){
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
    return str;
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
    // Return if destination and source is NULL;
    if((destination == NULL) && (source == NULL)) return NULL;
    // Take a pointer pointing to the beggining of destination string;
    char* begin = destination;
    /*
        Copy first num characters of C-string pointed by source
        into the array pointed by destination;
    */
   while(*source && num--)
   {
       *destination = *source;
       destination++;
       source++;
   }
   // null terminate destination string;
   *destination = '\0';
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

