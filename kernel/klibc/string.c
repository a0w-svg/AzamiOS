#include "./include/string.h"
#include <limits.h>
/*
    Fill block of memory
    Sets the first num bytes of the block of memory pointed by ptr to the specified value
    (interpreted as an unsigned char)
*/
void* memset(void* ptr, int value, size_t num)
{
    unsigned char* p = ptr;
    while(num--)
    {
        *p++ = (unsigned char)value;
    }
    return ptr;
}

/*
    Copy block of memory
    Copies the values of num bytes from the location pointed to by source directly
    to the memory block pointed to by destination.
*/
void* memcpy(void* restrict dstptr, const void* restrict srcptr, size_t size) {
	unsigned char* dst = (unsigned char*) dstptr;
	const unsigned char* src = (const unsigned char*) srcptr;
	for (size_t i = 0; i < size; i++)
		dst[i] = src[i];
	return dstptr;
}

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

