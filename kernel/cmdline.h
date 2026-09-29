/* ============================================================================
 * AzamiOS — Kernel command line
 * File: kernel/cmdline.h
 *
 * The string the bootloader passes as the kernel's command line
 * (`kernel_cmdline:` in limine.conf), parsed with Linux's rules: parameters
 * are separated by spaces, a value follows the first '=', and double quotes
 * group a value containing spaces (`init="/bin/sh -l"`). When a key appears
 * more than once, the last occurrence wins — which is what lets a bootloader
 * menu append an override to a default line.
 *
 * Parameters AzamiOS acts on (full descriptions: docs/KERNEL-PARAMETERS.md):
 *
 *   root=<dev>            root block device, with or without /dev/
 *   rootfstype=<fs>       root filesystem type (default: ext2)
 *   init=<path>           first user process (default: /sbin/init.elf)
 *   nosmp, maxcpus=<n>    limit the CPUs brought online
 *   quiet                 keep boot messages in the log (dmesg) only
 *   console=ttyS0|tty0    where kernel messages are echoed (repeatable)
 *   clocksource=<name>    tsc, hpet or jiffies
 *   azami.disk_selftest=1 run the write/read-back block driver self-tests
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"

#define CMDLINE_MAX 1024

/** cmdline_init() — capture the bootloader-provided command line.
 *
 * Copies it into kernel memory; must run before the PMM starts handing out
 * bootloader-reclaimable pages, which is where the original string lives. */
void cmdline_init(void);

/** cmdline_get() — the full command line exactly as passed ("" if none). */
const char *cmdline_get(void);

/** cmdline_has(key) — true if `key` or `key=...` appears. */
bool cmdline_has(const char *key);

/** cmdline_get_str(key, out, len) — copy the value of the last `key=value`.
 * Returns true and NUL-terminates @out (truncating) if the key has a value. */
bool cmdline_get_str(const char *key, char *out, size_t len);

/** cmdline_get_bool(key, def) — a bare `key` is true; `key=` accepts
 * 1/0, y/n, yes/no, on/off, true/false. Absent or unparsable gives @def. */
bool cmdline_get_bool(const char *key, bool def);

/** cmdline_get_long(key, def) — decimal, or 0x-prefixed hex, value of @key. */
long cmdline_get_long(const char *key, long def);

/** cmdline_for_each(key, fn, ctx) — call @fn for every value of a key that
 * may repeat (console=). Returns the number of occurrences. */
int cmdline_for_each(const char *key, void (*fn)(const char *val, void *ctx), void *ctx);
