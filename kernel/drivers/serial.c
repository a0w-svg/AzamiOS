#include "./include/serial.h"
#include "../klibc/include/port.h"
#define  COM1 0x3F8 // COM1
void init_serial()
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

int serial_received()
{
    return inb(COM1 + 5) & 1;
}

uint8_t read_serial()
{
    while(serial_received() == 0);
    return inb(COM1);
}

int is_transmit_empty()
{
    return inb(COM1 + 5) & 0x20;
}

void write_serial(char ch)
{
    while(is_transmit_empty() == 0);
    outb(COM1, ch);
}

void write_serial_bytes(const char* txt)
{
    int i = 0;
    while(txt[i] != 0)
    {
        write_serial(txt[i]);
        i++;
    }
}