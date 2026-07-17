include ../../config.mk

CFLAGS  = $(USER_CFLAGS) -Ofast -fno-strict-aliasing -funroll-loops
LDFLAGS = -T link.ld $(COMMON_LDFLAGS)

SRCS    = main.c \
          wm_core.c \
          wm_config.c \
          desktop.c \
          services/welcome.c \
          services/terminal.c \
          services/notepad.c \
          services/sysmon.c \
          services/files.c \
          services/glcube.c \
          services/pong.c \
          services/ide.c \
          services/settings.c \
          services/snake.c \
          services/minesweeper.c \
          services/tetris.c \
          services/python_repl.c \
          services/netmon.c \
          services/browser.c \
          services/debugger.c \
          ../../../lib/gfx/game_engine.c

OBJS    = $(SRCS:.c=.o)
LIBS    = $(AZAMI_LIB)
TARGET  = wm

.PHONY: all clean

all: $(TARGET)


$(TARGET): $(OBJS) $(LIBS)

	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(USER_LDLIBS)


../../../lib/gfx/game_engine.o: ../../../lib/gfx/game_engine.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
