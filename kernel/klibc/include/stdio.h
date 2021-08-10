#ifndef STDIO_H
#define STDIO_H
#define EOF (-1)
/*
 Print function
    flags:
    %d - displaying the decimal number from the argument of the int type;
    %s - displaying the string from the argument of the  const char* type;
 */
void printf(char* format, ...);
/*
    Function places a char;
    char ch - character to print;
*/
int putchar(char ch);
/*
    Function places a string on the screen;
    const char* string - the string you want to display;
*/
int puts(const char* string);
#endif