include ../../config.mk

CFLAGS  = $(USER_CFLAGS)
LDFLAGS = -T ../glcube/link.ld $(COMMON_LDFLAGS)

SRCS    = uniq.c
OBJS    = $(SRCS:.c=.o)
LIBS    = $(AZAMI_LIB)
TARGET  = uniq

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(LIBS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(USER_LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
