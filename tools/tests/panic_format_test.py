#!/usr/bin/env python3
"""Exercise the production panic formatter with hosted console/CPU stubs."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
DEFS = r"""
#pragma once
#include <stdint.h>
#define __noreturn __attribute__((noreturn))
typedef uint8_t u8;
void cpu_cli(void);
__noreturn void cpu_halt_loop(void);
__noreturn void kernel_panic(const char *, ...);
"""
CONSOLE = r"""
#pragma once
void kprintf(const char *, ...);
void kputc(char);
void console_force_verbose(void);
"""
HARNESS = r"""
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "panic.h"
static jmp_buf halt;
static char output[8192], expected[2048];
static size_t used;
static unsigned cli, stopped, verbose, notified, debugged, checks;
void cpu_cli(void) { cli++; }
void cpu_halt_loop(void) { longjmp(halt, 1); }
void smp_stop_other_cpus(void) { stopped++; }
void console_force_verbose(void) { verbose++; }
void pvpanic_notify(u8 event) { notified += event == 1; }
void debugcon_puts(const char *s) { debugged += strstr(s, "system halted") != NULL; }
void kputc(char c) {
    if (used + 1 >= sizeof(output)) abort();
    output[used++] = c; output[used] = '\0';
}
void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(output + used, sizeof(output) - used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(output) - used) abort();
    used += (size_t)n;
}
static void verify(const char *name) {
    char *start = strstr(output, "[test]");
    char *end = start ? strstr(start + 6, "[/test]") : NULL;
    if (end) *end = '\0';
    if (!start || !end || strcmp(start + 6, expected) ||
        cli != 1 || stopped != 1 || verbose != 1 || notified != 1 || debugged != 1) {
        fprintf(stderr, "%s: expected <%s>, captured <%s>\n", name, expected, output);
        exit(1);
    }
    checks++;
}
#define CHECK(name, fmt, ...) do { \
    int n = snprintf(expected, sizeof(expected), fmt, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= sizeof(expected)) abort(); \
    used = 0; output[0] = '\0'; cli = stopped = verbose = notified = debugged = 0; \
    if (!setjmp(halt)) kernel_panic("[test]" fmt "[/test]", __VA_ARGS__); \
    verify(name); \
} while (0)
int main(void) {
    CHECK("negative int", "%d", -17);
    CHECK("INT_MIN", "%d", INT_MIN);
    CHECK("negative long", "%ld", -17L);
    CHECK("LONG_MIN", "%ld", LONG_MIN);
    CHECK("negative long long", "%lld", -17LL);
    CHECK("LLONG_MIN", "%lld", LLONG_MIN);
    CHECK("signed zero", "%d %ld %lld", 0, 0L, 0LL);
    CHECK("signed positive", "%d %ld %lld", INT_MAX, LONG_MAX, LLONG_MAX);
    CHECK("unsigned zero", "%u %lu %llu", 0U, 0UL, 0ULL);
    CHECK("unsigned maxima", "%u %lu %llu", UINT_MAX, ULONG_MAX, ULLONG_MAX);
    CHECK("mixed argument advancement", "%d|%u|%ld|%lu|%lld|%llu|%s|%c|%%|%x",
          INT_MIN, UINT_MAX, LONG_MIN, ULONG_MAX, LLONG_MIN, ULLONG_MAX,
          "literal %d", 'Z', 0xabcU);
    printf("panic formatter: %u production boundary checks passed\n", checks);
    return 0;
}
"""


def main():
    compiler = shlex.split(os.environ.get("CC", "cc"))
    source = (REPO / "kernel/panic.c").read_text()
    with tempfile.TemporaryDirectory(prefix="azami-panic-format-") as temporary:
        work = Path(temporary)
        for relative, text in {
            "kernel/panic.h": '#include "../include/azami/defs.h"\n',
            "include/azami/defs.h": DEFS,
            "drivers/char/console.h": CONSOLE,
            "harness.c": HARNESS,
        }.items():
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error"]
        # stdin and a temporary cwd ensure quoted includes use only our stubs.
        subprocess.run(compiler + flags + ["-iquote", str(work / "kernel"),
                       "-x", "c", "-", "-c", "-o", "panic.o"],
                       input=source, text=True, cwd=work, check=True)
        subprocess.run(compiler + flags + ["-iquote", str(work / "kernel"),
                       "harness.c", "panic.o", "-o", "test"], cwd=work, check=True)
        subprocess.run([str(work / "test")], cwd=work, check=True)


if __name__ == "__main__":
    main()
