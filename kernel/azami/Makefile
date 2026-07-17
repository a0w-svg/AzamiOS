#
# azamiOS Kernel Makefile
# Path: /kernel/azami/Makefile
# Description: Freestanding C11 build configuration for the azamiOS Hybrid Kernel
#

CC      ?= gcc
AR      ?= ar
CFLAGS  := -std=c11 -ffreestanding -Wall -Wextra -Werror -O2 -Iinclude
SRCS    := core/nt_kernel.c lxss/syscall_dispatcher.c drivers/win32k.c main.c
OBJS    := $(SRCS:.c=.o)
TARGET  := azami_kernel.a

.PHONY: all clean verify

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c include/azami_core.h
	$(CC) $(CFLAGS) -c $< -o $@

verify: clean all
	@echo "azamiOS kernel build successful: $(TARGET)"

clean:
	rm -f $(OBJS) $(TARGET)
