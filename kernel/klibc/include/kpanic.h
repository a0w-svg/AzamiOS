#ifndef KPANIC_H
#define KPANIC_H
#include <stdint.h>

typedef int KRESULT;

/* Displays kernel panic screen with message, file location, registers, and backtrace */
#define PANIC(msg) kpanic(msg, __FILE__, __LINE__)

/* Assertion macro triggering panic if condition is false */
#define ASSERT(b) ((b) ? (void)0 : kpanic_assert(__FILE__, __LINE__, #b))

/* Debugging and Logging macros (v2.0 Diagnostics) */
#define KDEBUG(msg) kprintf("[DEBUG] %s:%d: %s\n", __FILE__, __LINE__, msg)
#define KTRACE() kprintf("[TRACE] function %s at %s:%d\n", __func__, __FILE__, __LINE__)

#define KLOG_INFO(fmt, ...)  kprintf("[INFO]  " fmt "\n", ##__VA_ARGS__)
#define KLOG_WARN(fmt, ...)  kprintf("[WARN]  " fmt "\n", ##__VA_ARGS__)
#define KLOG_ERR(fmt, ...)   kprintf("[ERR]   %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define KLOG_DEBUG(fmt, ...) kprintf("[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

void kpanic(const char* msg, const char* file, uint32_t line);
void kpanic_assert(const char* file, uint32_t line, const char* descript);

void dump_registers(void);
void print_backtrace(uint32_t max_frames);

#endif