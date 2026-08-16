#pragma once

#ifndef DEBUG
#define DEBUG 0
#endif

void kprintf(const char *fmt, ...);

#define pr_debug(fmt, ...) do { \
    if (DEBUG) { \
        kprintf(fmt, ##__VA_ARGS__); \
    } \
} while(0)
