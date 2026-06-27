/**
 * setjmp.h — AzamiOS libc: non-local jumps (x86)
 *
 * jmp_buf layout (6 × uint32_t):
 *   [0] EBX  [1] ESI  [2] EDI  [3] EBP  [4] ESP  [5] EIP
 */
#ifndef _SETJMP_H
#define _SETJMP_H

#include <stdint.h>

typedef uint32_t jmp_buf[6];

/**
 * setjmp — save the calling environment; returns 0 on first call,
 *          or the val passed to longjmp on a non-local jump.
 */
int  setjmp (jmp_buf env);

/**
 * longjmp — restore the environment saved by setjmp, passing val
 *           as the apparent return value of setjmp (never 0).
 */
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* _SETJMP_H */
