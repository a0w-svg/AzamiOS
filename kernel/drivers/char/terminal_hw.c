/**
 * kernel/drivers/terminal_hw.c  –  VGA text-mode terminal (Ring-0 hardware only)
 *
 * This file is the ONLY part of the terminal stack that requires kernel
 * privileges:
 *   - Direct MMIO writes to the VGA framebuffer at 0xB8000
 *   - outb/inb to VGA cursor control ports (0x3D4 / 0x3D5)
 *   - Serial port output (debugging via COM1)
 *
 * The cursor/scroll arithmetic that does NOT touch hardware has been
 * kept here for simplicity (it only uses integer math).
 *
 * Public API (declared in kernel/drivers/include/terminal.h) is
 * unchanged so no other kernel code needs modification.
 */
#include "./include/terminal.h"
#include "./include/serial.h"
#include "../klibc/include/port.h"
#include "../klibc/include/string.h"
#include <stdint.h>

/* ── VGA constants ───────────────────────────────────────────────── */
#define VIDEO_ADDR      0xB8000
#define MAX_ROWS        25
#define MAX_COLS        80
#define REG_VGA_CTRL    0x3D4
#define REG_VGA_DATA    0x3D5
#define WHITE_ON_BLACK  0x0F

/* ── Internal helpers ────────────────────────────────────────────── */

static inline int get_offset(int col, int row) {
    return 2 * (row * MAX_COLS + col);
}

static inline int get_offset_row(int offset) {
    return offset / (2 * MAX_COLS);
}

static inline int get_offset_col(int offset) {
    return (offset - (get_offset_row(offset) * 2 * MAX_COLS)) / 2;
}

/** Fast byte-copy for scrolling — replaces the local memcpy_w */
static void vga_memcpy(uint8_t *src, uint8_t *dest, int count) {
    for (int i = 0; i < count; i++)
        dest[i] = src[i];
}

/**
 * get_cursor_offset – read the current VGA hardware cursor position.
 * Returns the byte offset into the VGA framebuffer.
 */
static int get_cursor_offset(void) {
    outb(REG_VGA_CTRL, 14);
    int offset = inb(REG_VGA_DATA) << 8;
    outb(REG_VGA_CTRL, 15);
    offset += inb(REG_VGA_DATA);
    return offset * 2;
}

/**
 * set_cursor_offset – move the hardware cursor to byte offset.
 */
static void set_cursor_offset(int offset) {
    offset /= 2;
    outb(REG_VGA_CTRL, 14);
    outb(REG_VGA_DATA, (uint8_t)(offset >> 8));
    outb(REG_VGA_CTRL, 15);
    outb(REG_VGA_DATA, (uint8_t)(offset & 0xFF));
}

/**
 * print_char – write one character to the VGA framebuffer.
 * Handles: newline, tab, backspace, scrolling.
 * Returns the new cursor byte offset.
 */
static int print_char(char ch, int col, int row, char attrib) {
    uint8_t *vga = (uint8_t*)VIDEO_ADDR;
    if (!attrib) attrib = WHITE_ON_BLACK;

    if (col >= MAX_COLS || row >= MAX_ROWS) {
        vga[2 * MAX_COLS * MAX_ROWS - 2] = 'E';
        vga[2 * MAX_COLS * MAX_ROWS - 1] = WHITE_ON_BLACK;
        return get_offset(col, row);
    }

    int offset = (col >= 0 && row >= 0) ? get_offset(col, row)
                                        : get_cursor_offset();

    if (ch == '\n') {
        row    = get_offset_row(offset);
        offset = get_offset(0, row + 1);
    } else if (ch == '\t') {
        row    = get_offset_row(offset);
        col    = get_offset_col(offset);
        offset = get_offset(col + 4, row);
        ch     = 0;
    } else if (ch == 0x08) {           /* backspace */
        vga[offset]     = ' ';
        vga[offset + 1] = attrib;
    } else {
        vga[offset]     = ch;
        vga[offset + 1] = attrib;
        offset += 2;
    }

    /* Scroll up one line when past the last row */
    if (offset >= MAX_ROWS * MAX_COLS * 2) {
        for (int i = 1; i < MAX_ROWS; i++)
            vga_memcpy((uint8_t*)(get_offset(0, i)     + VIDEO_ADDR),
                       (uint8_t*)(get_offset(0, i - 1) + VIDEO_ADDR),
                       MAX_COLS * 2);
        char *last = (char*)(get_offset(0, MAX_ROWS - 1) + (uint8_t*)VIDEO_ADDR);
        for (int i = 0; i < MAX_COLS * 2; i++) last[i] = 0;
        offset -= 2 * MAX_COLS;
    }

    set_cursor_offset(offset);
    return offset;
}

/**
 * printk_at – write a string at (col, row), or at the cursor if negative.
 */
static void printk_at(const char *txt, int col, int row, char attrib) {
    int offset;
    if (col >= 0 && row >= 0) {
        offset = get_offset(col, row);
    } else {
        offset = get_cursor_offset();
        row    = get_offset_row(offset);
        col    = get_offset_col(offset);
    }
    int i = 0;
    while (txt[i] != 0) {
        offset = print_char(txt[i++], col, row, attrib);
        row    = get_offset_row(offset);
        col    = get_offset_col(offset);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

/**
 * terminal_write – output a string to both the VGA console and COM1 serial.
 */
int terminal_write(const char *txt) {
    write_serial_bytes(txt);
    printk_at(txt, -1, -1, 0);
    return -1;
}

/**
 * terminal_clean – clear the VGA screen and reset the cursor to (0,0).
 */
void terminal_clean(void) {
    init_serial();
    int term_size = MAX_COLS * MAX_ROWS;
    uint8_t *term = (uint8_t*)VIDEO_ADDR;
    for (int i = 0; i < term_size; i++) {
        term[i * 2]     = ' ';
        term[i * 2 + 1] = WHITE_ON_BLACK;
    }
    set_cursor_offset(get_offset(0, 0));
}

/**
 * put_char – output one character to VGA and COM1.
 */
int put_char(char ch) {
    write_serial(ch);
    print_char(ch, -1, -1, 0);
    return -1;
}
