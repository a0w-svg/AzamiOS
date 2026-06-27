# ──────────────────────────────────────────────────────────────────────────────
# AzamiOS  Makefile
#
# Layout after lib/ refactor:
#   lib/          — kernel-independent pure-C code (string, stdlib, net, fs, gfx)
#   kernel/       — Ring-0 kernel (links against lib/ objects)
#   user/         — userspace programs (link against user/libc)
#   build/        — separate output directory for all build artifacts & objects
# ──────────────────────────────────────────────────────────────────────────────

BUILD_DIR = build

# ── Kernel C sources ──────────────────────────────────────────────────────────
C_SOURCES = $(wildcard kernel/arch/*.c \
                        kernel/klibc/*.c \
                        kernel/klibc/stdio/*.c \
                        kernel/mem/*.c \
                        kernel/syscall/*.c \
                        kernel/filesystem/*.c \
                        kernel/proc/*.c \
                        kernel/module/*.c) \
            $(filter-out kernel/drivers/display/gfx.c kernel/drivers/char/terminal.c, \
              $(wildcard kernel/drivers/*/*.c))


# ── Headers (for dependency tracking) ────────────────────────────────────────
HEADERS = $(wildcard kernel/arch/include/*.h \
                      kernel/klibc/include/*.h \
                      kernel/drivers/include/*.h \
                      kernel/mem/include/*.h \
                      kernel/syscall/include/*.h \
                      kernel/filesystem/include/*.h \
                      kernel/proc/include/*.h \
                      kernel/module/include/*.h \
                      kernel/hal/*.h \
                      lib/string/string.h \
                      lib/stdlib/stdlib.h \
                      lib/net/net_stack.h \
                      lib/net/net_hal.h \
                      lib/fs/vfs.h \
                      lib/fs/tarfs.h \
                      lib/gfx/gfx_blit.h)

# ── Object files (mapped into build/obj/) ────────────────────────────────────
C_OBJS = $(patsubst %.c, $(BUILD_DIR)/obj/%.o, $(C_SOURCES))

OBJ = $(C_OBJS) \
      $(BUILD_DIR)/obj/kernel/arch/cpu.o \
      $(BUILD_DIR)/obj/kernel/arch/interrupts.o \
      $(BUILD_DIR)/obj/kernel/mem/paging_ext.o \
      $(BUILD_DIR)/obj/kernel/arch/smp_boot.o

# ── Cross-compiler toolchain ──────────────────────────────────────────────────
CC    = i686-elf-gcc
GDB   = i686-elf-gdb
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -m32 -fno-stack-protector \
         -I. -Ikernel -Ilib -Ikernel/drivers -Ikernel/drivers/include



# ── Host compiler (for lib/ unit tests) ──────────────────────────────────────
HOST_CC     = gcc
HOST_CFLAGS = -Wall -Wextra -O2 -I. -Ilib

# ──────────────────────────────────────────────────────────────────────────────
# Primary aliases / phony targets
# ──────────────────────────────────────────────────────────────────────────────
UTIL_LIST = ls help cat write time clear ifconfig ping arp lsmod reload cpu whoami fps acpi reboot shutdown about notepad files
UTIL_TARGETS = $(foreach u,$(UTIL_LIST),user/apps/$(u)/$(u))

.PHONY: all run run-iso run-debug build-iso kernel.bin kernel.elf initrd.tar fat32.img clean test-lib
.PHONY: user/apps/wm/wm user/apps/shell/shell user/apps/cc/cc user/apps/glcube/glcube $(UTIL_TARGETS)

all: $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/initrd.tar

kernel.bin: $(BUILD_DIR)/kernel.bin
kernel.elf: $(BUILD_DIR)/kernel.elf
initrd.tar: $(BUILD_DIR)/initrd.tar
fat32.img:  $(BUILD_DIR)/fat32.img
build-iso:  $(BUILD_DIR)/AzamiOS.iso

# ──────────────────────────────────────────────────────────────────────────────
# Build rules inside $(BUILD_DIR)
# ──────────────────────────────────────────────────────────────────────────────

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/obj/kernel/boot/boot.o ${OBJ}
	@mkdir -p $(dir $@)
	i686-elf-ld $^ -T Link.ld --no-warn-rwx-segments -o $@

$(BUILD_DIR)/kernel.elf: $(BUILD_DIR)/obj/kernel/boot/boot.o ${OBJ}
	@mkdir -p $(dir $@)
	i686-elf-ld -T Link.ld --no-warn-rwx-segments $^ -o $@

$(BUILD_DIR)/fat32.img:
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=32 status=none
	mkfs.fat -F 32 -n "AZAMIOS" $@
	echo "Hello from FAT32 inside AzamiOS!" > readme.txt
	mcopy -i $@ readme.txt ::README.TXT
	rm -f readme.txt

$(BUILD_DIR)/initrd.tar: user/apps/wm/wm user/apps/shell/shell user/apps/cc/cc user/apps/glcube/glcube user/apps/cc/fib.c $(UTIL_TARGETS)
	@mkdir -p $(BUILD_DIR)/user_bin
	cp user/apps/wm/wm    $(BUILD_DIR)/user_bin/wm
	cp user/apps/shell/shell $(BUILD_DIR)/user_bin/shell
	cp user/apps/cc/cc    $(BUILD_DIR)/user_bin/cc
	cp user/apps/glcube/glcube $(BUILD_DIR)/user_bin/glcube
	cp user/apps/cc/fib.c $(BUILD_DIR)/user_bin/fib.c
	for u in $(UTIL_LIST); do cp user/apps/$$u/$$u $(BUILD_DIR)/user_bin/$$u; done
	cd $(BUILD_DIR)/user_bin && tar --format=ustar -cf ../initrd.tar wm shell cc glcube fib.c $(UTIL_LIST)

$(BUILD_DIR)/AzamiOS.iso: $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/initrd.tar boot/grub/menu.lst
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp boot/grub/stage2_eltorito $(BUILD_DIR)/iso/boot/grub/
	cp $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/iso/boot/
	cp $(BUILD_DIR)/initrd.tar $(BUILD_DIR)/iso/boot/
	cp boot/grub/menu.lst $(BUILD_DIR)/iso/boot/grub/
	mkisofs -R \
	  -b boot/grub/stage2_eltorito \
	  -no-emul-boot \
	  -boot-load-size 4 \
	  -A os \
	  -input-charset utf8 \
	  -quiet \
	  -boot-info-table \
	  -o $@ \
	  $(BUILD_DIR)/iso

# ── Userspace apps ────────────────────────────────────────────────────────────
user/apps/wm/wm:
	$(MAKE) -C user/apps/wm
user/apps/shell/shell:
	$(MAKE) -C user/apps/shell
user/apps/cc/cc:
	$(MAKE) -C user/apps/cc
user/apps/glcube/glcube:
	$(MAKE) -C user/apps/glcube
$(UTIL_TARGETS):
	$(MAKE) -C $(dir $@)

# ── Execution targets ─────────────────────────────────────────────────────────
run: $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/initrd.tar $(BUILD_DIR)/fat32.img
	qemu-system-i386 -vga std -accel tcg,thread=multi -kernel $(BUILD_DIR)/kernel.bin -initrd $(BUILD_DIR)/initrd.tar -hda $(BUILD_DIR)/fat32.img \
	  -smp 4 -serial stdio -device ac97 -device rtl8139 -device e1000 -device pcnet -device ne2k_pci -device ES1370 -device ahci

run-debug: $(BUILD_DIR)/kernel.bin
	qemu-system-i386 -vga std -accel tcg,thread=multi -kernel $(BUILD_DIR)/kernel.bin -smp 4 -serial stdio

run-iso: $(BUILD_DIR)/AzamiOS.iso $(BUILD_DIR)/fat32.img
	qemu-system-i386 -vga std -accel tcg,thread=multi -cdrom $(BUILD_DIR)/AzamiOS.iso -hda $(BUILD_DIR)/fat32.img -boot d \
	  -smp 4 -serial stdio -device ac97 -device rtl8139 -device e1000 -device pcnet -device ne2k_pci -device ES1370 -device ahci




# ──────────────────────────────────────────────────────────────────────────────
# lib/ host-compiler unit test target
# ──────────────────────────────────────────────────────────────────────────────
LIB_SOURCES = lib/string/string.c \
              lib/stdlib/stdlib.c \
              lib/fs/vfs.c \
              lib/fs/tarfs.c \
              lib/gfx/gfx_blit.c \
              lib/net/net_stack.c

test-lib:
	@echo "==> Compiling lib/ with host gcc to check kernel-independence..."
	@for src in $(LIB_SOURCES); do \
	  echo "  CC $$src"; \
	  $(HOST_CC) $(HOST_CFLAGS) -c $$src -o /dev/null 2>&1 || exit 1; \
	  done
	@echo "==> lib/ test-lib PASSED: no kernel dependencies found."

# ──────────────────────────────────────────────────────────────────────────────
# Compilation rules
# ──────────────────────────────────────────────────────────────────────────────

$(BUILD_DIR)/obj/%.o: %.c ${HEADERS}
	@mkdir -p $(dir $@)
	${CC} ${CFLAGS} -c $< -o $@

$(BUILD_DIR)/obj/%.o: %.asm
	@mkdir -p $(dir $@)
	nasm $< -f elf -o $@

$(BUILD_DIR)/%.bin: %.asm
	@mkdir -p $(dir $@)
	nasm $< -f bin -o $@

# ──────────────────────────────────────────────────────────────────────────────
# Clean
# ──────────────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) kernel/boot/boot.o kernel.bin kernel.elf \
	       AzamiOS.iso *.bin *.dis initrd.tar fat32.img iso/ user/bin/
	$(MAKE) -C user/libc       clean
	$(MAKE) -C user/apps/wm    clean
	$(MAKE) -C user/apps/shell clean
	$(MAKE) -C user/apps/cc    clean