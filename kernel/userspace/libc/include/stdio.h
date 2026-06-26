#ifndef STDIO_H
#define STDIO_H

#define EOF (-1)
#define NULL ((void*)0)

/*
 * Print function — format flags:
 *   %c  character
 *   %d  decimal integer
 *   %s  string
 *   %x  hexadecimal integer
 *   %o  octal integer
 */
void printf(char* format, ...);

/* Write a single character to the terminal via syscall */
int putchar(char ch);

/* Write a string followed by newline via syscall */
int puts(const char* string);

/* Read a character via syscall */
int getchar(void);

#endif /* STDIO_H */