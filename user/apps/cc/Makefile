include ../../config.mk

CFLAGS  = $(USER_CFLAGS)
LDFLAGS = -T link.ld $(COMMON_LDFLAGS)

SRCS    = cc.c

OBJS    = $(SRCS:.c=.o)
LIBS    = $(AZAMI_LIB)
TARGET  = cc

.PHONY: all clean

all: $(TARGET)


$(TARGET): $(OBJS) $(LIBS)

	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(USER_LDLIBS)


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
