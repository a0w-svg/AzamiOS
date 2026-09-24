CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC = $(CROSS_PREFIX)gcc
AR = $(CROSS_PREFIX)ar
LD = $(CROSS_PREFIX)ld
NASM = nasm

# See ../Makefile for why: ccache wraps the compiler (pure win on rebuilds),
# DEBUG=1 swaps -O3 for -O0, and the stack-protector pairing matches the
# __stack_chk_guard global this same libc defines in stdlib.c.
CCACHE ?= 1
ifeq ($(CCACHE),1)
  CCACHE_BIN := $(shell command -v ccache 2>/dev/null)
  ifneq ($(CCACHE_BIN),)
    CC := ccache $(CC)
  endif
endif

DEBUG ?= 0
ifeq ($(DEBUG),1)
  OPT_FLAGS := -O0
else
  OPT_FLAGS := -O3
endif

CFLAGS = -std=c11 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global \
         -fno-pie -fno-pic -m64 -Wall -Wextra -Wformat=2 -Wformat-security \
         $(OPT_FLAGS) -pipe -ffunction-sections -fdata-sections -I. -Iinclude

# Header dependency tracking: without it, editing a header rebuilds nothing and
# libc.a keeps stale objects compiled against the old declarations.
CFLAGS += -MMD -MP

SRCS = mqueue.c semaphore.c linuxext.c syscall.c stdio.c stdlib.c string.c string_simd.c ctype.c dirent.c time.c math.c az_ipc.c signal.c mman.c pthread.c termios.c resource.c getopt.c acl.c x11.c socket.c netdb.c pwd.c grp.c regex.c fnmatch.c glob.c spawn.c syslog.c search.c utmp.c xsi.c tls.c ldso_bridge.c pty.c ftw.c wordexp.c font.c theme.c sha256.c dlfcn.c
OBJS = $(SRCS:.c=.o) setjmp.o

# ── libc.so: a second, PIC build of the exact same SRCS list ────────────────
# Reuses SRCS (never a second, independently-maintained file list — see
# userland/Makefile's dynlink section and userland/ldso/ldso.c's top comment
# for why -ftls-model=initial-exec is load-bearing here: ld-azami.so only
# implements R_X86_64_TPOFF64, not the general-dynamic
# DTPMOD64/DTPOFF64 pair). Object files land at *.pic.o, never *.o, so this
# can't clobber a single object the static libc.a build (crt0.o/setjmp.o/
# *.o above) needs — both builds coexist from the same source tree.
# -fno-stack-protector (unlike the static CFLAGS above): the
# -mstack-protector-guard=global pairing assumes a plain absolute-address
# global, which is exactly what -fpic code must NOT rely on; ld-azami.so
# also has no PT_GNU_RELRO / lazy-binding infrastructure that would make a
# stack-protector-in-a-shared-object setup worth the complexity yet.
PIC_CFLAGS = -std=c11 -ffreestanding -fno-stack-protector -fpic -fno-plt \
             -m64 -Wall -Wextra -Wformat=2 -Wformat-security \
             $(OPT_FLAGS) -pipe -ffunction-sections -fdata-sections \
             -ftls-model=initial-exec -I. -Iinclude -MMD -MP

PIC_OBJS = $(SRCS:.c=.pic.o) setjmp.pic.o

.PHONY: all clean

all: libc.a crt0.o libc.so crt0_dyn.o

libc.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

crt0.o: crt0.asm
	$(NASM) -f elf64 $< -o $@

setjmp.o: setjmp.asm
	$(NASM) -f elf64 $< -o $@

setjmp.pic.o: setjmp.asm
	$(NASM) -f elf64 $< -o $@

# The dynamic-aware crt0 (userland/libc/crt0_dyn.asm) — see that file's own
# header comment for why it can't just be crt0.asm plus a rename: every
# call it makes into libc.so has to go through `wrt ..plt`, unlike
# crt0.asm's plain `call`, which only resolves correctly for a static link.
crt0_dyn.o: crt0_dyn.asm
	$(NASM) -f elf64 $< -o $@

# libc/libc.a's own crt0.o is the one every existing static app links
# against; only a genuinely dynamic app (built with the userland/Makefile
# DYNAMIC=1 path) ever pulls in crt0_dyn.o or libc.so — see userland/Makefile.
libc.so: $(PIC_OBJS)
	$(LD) -shared --soname=libc.so -T ../../scripts/user-pic.ld -nostdlib \
	    -z max-page-size=0x1000 $(PIC_OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.pic.o: %.c
	$(CC) $(PIC_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(PIC_OBJS) libc.a libc.so crt0.o crt0_dyn.o setjmp.o setjmp.pic.o
	rm -f *.d

-include $(OBJS:.o=.d)
-include $(PIC_OBJS:.pic.o=.pic.d)
