CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC = $(CROSS_PREFIX)gcc
AR = $(CROSS_PREFIX)ar
NASM = nasm

CFLAGS = -std=c11 -ffreestanding -fno-stack-protector -fno-pie -fno-pic -m64 -Wall -Wextra -O3 -pipe -ffunction-sections -fdata-sections -I. -Iinclude

# Header dependency tracking: without it, editing a header rebuilds nothing and
# libc.a keeps stale objects compiled against the old declarations.
CFLAGS += -MMD -MP

SRCS = mqueue.c semaphore.c linuxext.c syscall.c stdio.c stdlib.c string.c string_simd.c ctype.c dirent.c time.c math.c az_ipc.c signal.c mman.c pthread.c termios.c resource.c getopt.c acl.c x11.c socket.c netdb.c pwd.c grp.c regex.c fnmatch.c glob.c spawn.c syslog.c search.c utmp.c xsi.c tls.c
OBJS = $(SRCS:.c=.o) setjmp.o

.PHONY: all clean

all: libc.a crt0.o

libc.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

crt0.o: crt0.asm
	$(NASM) -f elf64 $< -o $@

setjmp.o: setjmp.asm
	$(NASM) -f elf64 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) libc.a crt0.o setjmp.o

-include $(OBJS:.o=.d)
