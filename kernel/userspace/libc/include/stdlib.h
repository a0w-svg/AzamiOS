#ifndef STDLIB_H
#define STDLIB_H

/* Integer-to-string conversion */
char* itoa(int value, char* str, int base);

/* String-to-integer conversion */
int atoi(char *str);

/* Terminate the current process */
void exit(int code);

/* Execute another program */
void exec(const char *filename);

#endif /* STDLIB_H */