#include "./include/terminal.h"
#include "../klibc/include/port.h"
#include "../klibc/include/string.h"
#include <stdint.h>
// defines constans values;
#define VIDEO_ADDR 0xB8000
#define MAX_ROWS 25
#define MAX_COLS 80
// VGA device I/O ports
#define REG_VGA_CTRL 0x3D4
#define REG_VGA_DATA 0x3D5
#define WHITE_ON_BLACK 0x0F

void memcpy_w(uint8_t *src, uint8_t *dest, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        *(dest + i) = *(src + i);
    }
}
/*
    Get offset;
*/
int get_offset(int col, int row);
/*
    Get row offset;
*/
int get_offset_row(int offset);
/*
    Get col offset;
*/
int get_offset_col(int offset);
/*
    Get current cursor position;
*/
int get_cursor_offset()
{
    outb(REG_VGA_CTRL, 14); // Ask for high byte of cursor offset (data 0xE);
    int offset = inb(REG_VGA_DATA) << 8; // Get the high byte of the current offset and store in the offset variable;
    outb(REG_VGA_CTRL, 15); // Ask for low byte of cursor offset (data 0xF);
    offset += inb(REG_VGA_DATA); // Get the low byte of the current offset and add to offset variable;
    return offset * 2; // return offset;
}

/*
    This function is similar to get_cursor_offset(), but this function sets a new cursor offset;
*/
void set_cursor_offset(int offset)
{
    offset /= 2;
    outb(REG_VGA_CTRL, 14); 
    outb(REG_VGA_DATA, (uint8_t)(offset >> 8));
    outb(REG_VGA_CTRL, 15);
    outb(REG_VGA_DATA, (uint8_t)(offset & 0xFF));
}

/*
    prints a character at the indicated location on the screen;
*/
int print_char(char ch, int col, int row, char attrib)
{
    uint8_t* vga_addr =  (uint8_t*)VIDEO_ADDR; // create pointer to the vga vram;
    if(!attrib) 
        attrib = WHITE_ON_BLACK;
    
    if(col >= MAX_COLS || row >= MAX_ROWS)
    {
        vga_addr[2*(MAX_COLS)*(MAX_ROWS)-2] = 'E';
        vga_addr[2*(MAX_COLS)*(MAX_ROWS)-1] = WHITE_ON_BLACK;
        return get_offset(col, row);
    }
    int offset;

    if(col >= 0 && row >= 0)
        offset = get_offset(col, row);

    else  offset = get_cursor_offset();
    
    if(ch == '\n')
    {
        row = get_offset_row(offset);
        offset = get_offset(0, row+1);
    }
    else if(ch == '\t')
    {
        row = get_offset_row(offset);
        col = get_offset_col(offset);
        offset = get_offset(col+4, row);
        ch = 0;
    }
    else if(ch == 0x08)
    {
        vga_addr[offset] = ' ';
        vga_addr[offset+1] = attrib;
    }
    else 
    {
        vga_addr[offset] = ch;
        vga_addr[offset+1] = attrib;
        offset += 2;
    }
    if(offset >= MAX_ROWS * MAX_COLS * 2)
    {
        for(int i = 1; i < MAX_ROWS; i++)
            memcpy_w((uint8_t*)(get_offset(0, i) + VIDEO_ADDR), (uint8_t*)(get_offset(0, i-1) + VIDEO_ADDR), MAX_COLS * 2);
        char* last_line = (char*)(get_offset(0, MAX_ROWS-1) + (uint8_t*) VIDEO_ADDR);
        for(int i = 0; i < MAX_COLS * 2; i++)
            last_line[i] = 0;
        offset -= 2 * MAX_COLS;
    }
    set_cursor_offset(offset);
    return offset;
}

/*
    prints a string at the indicated location on the screen.
*/
void printk_at(const char* txt, int col, int row, char attrib)
{
    int offset;
    if(col >= 0 && row >= 0) // if col and row are greater or equal 0, get the offset;
        offset = get_offset(col, row);
    else
    {
        offset = get_cursor_offset(); // get current cursor offset;
        row = get_offset_row(offset); // get row offset;
        col = get_offset_col(offset); // get col offset;
    }
    int i = 0;
    while(txt[i] != 0) // repeat until txt[i] isn't zero;
    {
        offset = print_char(txt[i++], col, row, attrib); // print_char returns offset and prints a character on the screen;
        row = get_offset_row(offset);
        col = get_offset_col(offset);
    } 
}

/*
    Writes the string on the screen;
*/
int terminal_write(const char* txt)
{
    printk_at(txt, -1, -1, 0);
    return -1;
}

/*
    Cleans the terminal;
*/
void terminal_clean()
{
    int term_size = MAX_COLS * MAX_ROWS;
    uint8_t* term = (uint8_t*)VIDEO_ADDR;
    for(int i = 0; i < term_size; i++) 
    {
        term[i*2] = ' '; // blank character
        term[i*2+1] = WHITE_ON_BLACK; // font color
    }
    set_cursor_offset(get_offset(0, 0)); // set cursor offset to 0, 0
}

/*
    Puts character on the screen and return -1 if function finished successfully.
*/
int put_char(char ch)
{
    print_char(ch, -1, -1, 0);
    return -1;
}

int get_offset(int col, int row)
{
    return 2 * (row * MAX_COLS + col);
}

int get_offset_row(int offset)
{
    return offset / (2 * MAX_COLS);
}

int get_offset_col(int offset)
{
    return (offset - ((get_offset_row(offset)*2)*MAX_COLS)) / 2;
}