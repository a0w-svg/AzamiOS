#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void init_serial();
uint8_t read_serial();
void write_serial(char ch);
void write_serial_bytes(const char* txt);
#endif