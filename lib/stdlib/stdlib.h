/**
 * lib/stdlib/stdlib.h  –  AzamiOS portable stdlib (itoa / atoi)
 *
 * Kernel-independent. No hardware, no ring-0 primitives.
 */
#ifndef LIB_STDLIB_H
#define LIB_STDLIB_H

char* itoa(int value, char* str, int base);
int   atoi(char *str);

#endif /* LIB_STDLIB_H */
