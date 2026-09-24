CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC = $(CROSS_PREFIX)gcc
AR = $(CROSS_PREFIX)ar

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
         -Wno-unused-parameter $(OPT_FLAGS) -pipe \
         -ffunction-sections -fdata-sections \
         -Iinclude -I../libc/include -I../apps/shared -I../apps/azwm -I../../include \
         -MMD -MP

SRCS = game_ecs.c game_physics.c game_audio.c game_asset.c game_scene.c game_core.c game_hw3d.c game_render3d.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: libgame.a

libgame.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) libgame.a

-include $(OBJS:.o=.d)
