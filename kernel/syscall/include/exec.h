#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>

extern char g_return_program[64];
extern char g_current_program[64];

// main fuction to run a program(it recognizes format)
void execute_program(char *filename);
#endif