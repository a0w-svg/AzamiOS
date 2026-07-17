# ==============================================================================
# AzamiOS Userspace Toolchain Configuration
# Included by user/libc/Makefile and user/apps/*/Makefile
# ==============================================================================

ARCH = x86_64
NASM ?= nasm

# ── Cross-compiler selection ───────────────────────────────────────────────────
CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC      = $(CROSS_PREFIX)gcc
AR      = $(CROSS_PREFIX)ar
LD      = $(CROSS_PREFIX)ld
CFLAGS_ARCH  = -m64 -mno-red-zone -mcmodel=large
LDFLAGS_ARCH = --no-warn-rwx-segments
NASM_FMT     = elf64
SETJMP_ASM   = setjmp.asm

GCC_INC      = $(shell $(CC) -print-file-name=include)
COMMON_CFLAGS  = -ffreestanding -nostdlib -nostdinc $(CFLAGS_ARCH) \
                 -O2 -Wall -Wextra -fno-stack-protector -fno-pie -fno-pic \
                 -I$(GCC_INC)
COMMON_LDFLAGS = -nostdlib $(LDFLAGS_ARCH)

# ── Newlib sysroot ─────────────────────────────────────────────────────────────
# newlib is installed by scripts/port_newlib.sh into the cross-compiler sysroot.
NEWLIB_SYSROOT ?= $(HOME)/opt/cross-x86_64/x86_64-elf
NEWLIB_INC = $(NEWLIB_SYSROOT)/include
NEWLIB_LIB = $(NEWLIB_SYSROOT)/lib

# ── AzamiOS-specific libc directory ───────────────────────────────────────────
# (always relative to the config.mk location, i.e. user/)
AZAMI_LIBC_DIR = $(dir $(lastword $(MAKEFILE_LIST)))libc
AZAMI_INC      = $(AZAMI_LIBC_DIR)/include
AZAMI_LIB      = $(AZAMI_LIBC_DIR)/libc.a

# ── USE_NEWLIB: 1 = use newlib (default), 0 = custom libc only ────────────────
USE_NEWLIB ?= 1

ifeq ($(USE_NEWLIB),1)
# Compile: newlib headers first (system), then AzamiOS-specific headers on top
USER_CFLAGS  = $(COMMON_CFLAGS) \
               -isystem $(NEWLIB_INC) \
               -I$(AZAMI_INC)
USER_LDLIBS  = --start-group $(AZAMI_LIB) -L$(NEWLIB_LIB) -lc -lm --end-group -lnosys

else
# Legacy mode: custom libc only
USER_CFLAGS  = $(COMMON_CFLAGS) -I$(AZAMI_INC)
USER_LDLIBS  = $(AZAMI_LIB)
endif
