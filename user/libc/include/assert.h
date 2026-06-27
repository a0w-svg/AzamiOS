/**
 * assert.h — AzamiOS libc: assertion macro
 */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
void _assert_fail(const char *expr, const char *file, int line, const char *func);
#  define assert(expr) \
    ((expr) ? (void)0 : _assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#endif /* _ASSERT_H */
