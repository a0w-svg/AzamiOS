#ifndef TERMINAL_H
#define TERMINAL_H
#endif
/*
    Writes the text on terminal;
*/
int terminal_write(const char* txt);
/*
    Cleans the terminal;
*/
void terminal_clean();
/*
    Puts character on the screen and return -1 if function finished successfully.
*/
int put_char(char ch);