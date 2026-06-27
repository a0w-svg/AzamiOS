/**
 * ctype.h — AzamiOS libc: character classification & conversion
 */
#ifndef _CTYPE_H
#define _CTYPE_H

/* Classification — all return non-zero (true) or 0 (false) */
int isalpha (int c);
int isdigit (int c);
int isalnum (int c);
int isspace (int c);
int isprint (int c);
int isgraph (int c);
int ispunct (int c);
int isupper (int c);
int islower (int c);
int iscntrl (int c);
int isxdigit(int c);
int isblank (int c);

/* Conversion */
int toupper(int c);
int tolower(int c);

#endif /* _CTYPE_H */
