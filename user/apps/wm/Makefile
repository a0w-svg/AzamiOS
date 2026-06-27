CC      = i686-elf-gcc
LD      = i686-elf-ld

CFLAGS  = -ffreestanding -nostdlib -nostdinc -m32 -O2 -Wall -Wextra \
          -fno-stack-protector -fno-pie -fno-pic \
          -I/home/a0wsvg/opt/cross/lib/gcc/i686-elf/15.2.0/include \
          -I../../libc/include

LDFLAGS = -T link.ld -nostdlib --no-warn-rwx-segments

SRCS    = main.c \
          wm_core.c \
          desktop.c \
          services/welcome.c \
          services/terminal.c \
          services/notepad.c \
          services/sysmon.c \
          services/files.c \
          services/glcube.c

OBJS    = $(SRCS:.c=.o)
LIBS    = ../../libc/libc.a
TARGET  = wm

.PHONY: all clean

all: $(TARGET)

$(LIBS):
	$(MAKE) -C ../../libc

$(TARGET): $(OBJS) $(LIBS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
