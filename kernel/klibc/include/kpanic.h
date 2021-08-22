#ifndef KPANIC_H
#define KPANIC_H
#include <stdint.h>

typedef int KRESULT;
/*
    Displays the location of the exception and the type of exception;
*/
#define PANIC(msg) kpanic(msg, __FILE__, __LINE__);
/*
    This is a macro that performs kernel panic if the value entered into it is other than 0;
*/
#define ASSERT(b) ((b) ? (void)0 : kpanic_assert(__FILE__, __LINE__, #b))
/*
    Do not use these functions instead use the PANIC or ASSERT macros;
*/
void kpanic(const char* msg, const char* file, uint32_t line);
void kpanic_assert(const char* file, uint32_t line, const char* descript);

#endif