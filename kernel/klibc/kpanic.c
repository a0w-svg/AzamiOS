#include "./include/kpanic.h"
#include "./include/stdio.h"

/*
    Do not use these functions instead use the PANIC or ASSERT macros;
*/
void kpanic(const char* msg, const char* file, uint32_t line)
{
    printf("%s : %s line: %d\n", msg, file, line);
    while(1);
}
void kpanic_assert(const char* file, uint32_t line, const char* descript)
{
    printf("%s : line: %d : %s\n", file, line, descript);
    while(1);
}