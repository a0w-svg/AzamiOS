include ../config.mk

# When USE_NEWLIB=1 (default): build only AzamiOS-specific sources; newlib
#   provides all standard C functions. Include newlib headers for compilation.
# When USE_NEWLIB=0 (legacy): build the full custom libc.
#   All headers and sources live here.

CFLAGS = $(COMMON_CFLAGS) -Iinclude

# ── Source lists ───────────────────────────────────────────────────────────────
# azami_syscalls.c: AzamiOS non-POSIX extras (exec, mount, unmount).
#   Always compiled — newlib has no equivalent.
# syscalls.c: POSIX underscore stubs (_read/_write/_sbrk/…).
#   Legacy mode only: in newlib mode these come from libnosys.a.
# gui/gl/port/…: hardware-facing AzamiOS drivers.
#   Always compiled.

AZAMI_EXTRAS = syscalls.c azami_syscalls.c gui.c dirent.c gl.c port.c speaker.c \
               gameport.c gpu.c intel_gpu.c amd_gpu.c gpu_accel.c lpc_client.c

LIBC_SRCS    = string.c stdlib.c stdio.c ctype.c math.c time.c signal.c

ifeq ($(USE_NEWLIB),1)
$(info [libc] newlib mode: standard C from $(NEWLIB_SYSROOT))
SRCS    = $(AZAMI_EXTRAS)
CFLAGS += -isystem $(NEWLIB_INC)
else
$(info [libc] legacy mode: using custom libc)
SRCS    = $(LIBC_SRCS) $(AZAMI_EXTRAS)
endif

OBJS   = $(SRCS:.c=.o) setjmp.o
TARGET = libc.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

setjmp.o: $(SETJMP_ASM)
	$(NASM) -f $(NASM_FMT) $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
