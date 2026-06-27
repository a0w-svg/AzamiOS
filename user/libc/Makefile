CC      = i686-elf-gcc
AR      = i686-elf-ar
NASM    = nasm

CFLAGS  = -ffreestanding -nostdlib -nostdinc -m32 -O2 -Wall -Wextra \
          -fno-stack-protector -fno-pie -fno-pic \
          -I/home/a0wsvg/opt/cross/lib/gcc/i686-elf/15.2.0/include \
          -Iinclude

SRCS    = string.c stdlib.c stdio.c ctype.c math.c time.c signal.c gui.c syscalls.c dirent.c gl.c
OBJS    = $(SRCS:.c=.o) setjmp.o

TARGET  = libc.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

setjmp.o: setjmp.asm
	$(NASM) -f elf32 $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
