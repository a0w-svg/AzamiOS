CC      = i686-elf-gcc
LD      = i686-elf-ld

CFLAGS  = -ffreestanding -nostdlib -nostdinc -m32 -O2 -Wall -Wextra \
          -fno-stack-protector -fno-pie -fno-pic \
          -I/home/a0wsvg/opt/cross/lib/gcc/i686-elf/15.2.0/include

LDFLAGS = -T link.ld -nostdlib

SRCS    = shell.c \
          ../libc/stdio.c \
          ../libc/stdlib.c \
          ../libc/string.c \
          ../libc/time.c \
          ../libc/gui.c

OBJS    = $(SRCS:.c=.o)
TARGET  = shell

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
