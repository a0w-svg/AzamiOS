include ../../config.mk

CFLAGS  = $(USER_CFLAGS)
LDFLAGS = -T ../glcube/link.ld $(COMMON_LDFLAGS)

SRCS    = testarch.c
OBJS    = $(SRCS:.c=.o)
LIBS    = $(AZAMI_LIB)
TARGET  = testarch

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(LIBS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(USER_LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
