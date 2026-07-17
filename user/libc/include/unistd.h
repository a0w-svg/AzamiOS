/**
 * unistd.h — AzamiOS unistd wrapper
 *
 * Includes newlib's standard <unistd.h> and declares AzamiOS-specific
 * syscall functions like exec().
 */

#ifndef _AZAMI_UNISTD_H
#define _AZAMI_UNISTD_H

#include_next <unistd.h>

void exec(const char *filename);
int fork(void);
void yield(void);
int thread_create(void (*entry)(void*), void *arg, void *user_stack);

#endif /* _AZAMI_UNISTD_H */
