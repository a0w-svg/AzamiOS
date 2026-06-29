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

# ── Cross-compiler toolchain ──────────────────────────────────────────────────
ARCH ?= i686

ifeq ($(ARCH),x86_64)
CROSS_PREFIX = $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC    = $(CROSS_PREFIX)gcc
LD    = $(CROSS_PREFIX)ld
GDB   = $(CROSS_PREFIX)gdb
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -m64 -mno-red-zone -mcmodel=large -fno-stack-protector \
         -I. -Ikernel -Ilib -Ikernel/drivers -Ikernel/drivers/include
LDFLAGS = -T Link64.ld --no-warn-rwx-segments
NASM_FMT = elf64
BOOT_OBJ = $(BUILD_DIR)/obj/kernel/boot/boot64.o
OBJ = $(C_OBJS) \
      $(BUILD_DIR)/obj/kernel/arch/cpu64.o \
      $(BUILD_DIR)/obj/kernel/arch/interrupts64.o \
      $(BUILD_DIR)/obj/kernel/arch/smp_boot64.o
KERNEL_TARGET = $(BUILD_DIR)/kernel.elf
QEMU_CMD = qemu-system-x86_64 -m 1G -M q35 -vga std -accel tcg,thread=multi -kernel $(BUILD_DIR)/kernel.elf
else
CC    = i686-elf-gcc
LD    = i686-elf-ld
GDB   = i686-elf-gdb
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -m32 -fno-stack-protector \
         -I. -Ikernel -Ilib -Ikernel/drivers -Ikernel/drivers/include
LDFLAGS = -T Link.ld --no-warn-rwx-segments
NASM_FMT = elf
BOOT_OBJ = $(BUILD_DIR)/obj/kernel/boot/boot.o
OBJ = $(C_OBJS) \
      $(BUILD_DIR)/obj/kernel/arch/cpu.o \
      $(BUILD_DIR)/obj/kernel/arch/interrupts.o \
      $(BUILD_DIR)/obj/kernel/mem/paging_ext.o \
      $(BUILD_DIR)/obj/kernel/arch/smp_boot.o
KERNEL_TARGET = $(BUILD_DIR)/kernel.bin
QEMU_CMD = qemu-system-i386 -vga std -accel tcg,thread=multi -kernel $(BUILD_DIR)/kernel.bin
endif



# ── Host compiler (for lib/ unit tests) ──────────────────────────────────────
HOST_CC     = gcc
HOST_CFLAGS = -Wall -Wextra -O2 -I. -Ilib

# ──────────────────────────────────────────────────────────────────────────────
# Primary aliases / phony targets
# ──────────────────────────────────────────────────────────────────────────────
UTIL_LIST = ls help cat write time clear ifconfig ping arp lsmod reload cpu whoami fps acpi reboot shutdown about notepad files
UTIL_TARGETS = $(foreach u,$(UTIL_LIST),user/apps/$(u)/$(u))

.PHONY: all run run-iso run-uefi run-debug build-iso kernel.bin kernel.elf initrd.tar fat32.img clean test-lib
.PHONY: user/apps/wm/wm user/apps/shell/shell user/apps/cc/cc user/apps/glcube/glcube $(UTIL_TARGETS)

all: $(KERNEL_TARGET) $(BUILD_DIR)/initrd.tar

kernel.bin: $(BUILD_DIR)/kernel.bin
kernel.elf: $(BUILD_DIR)/kernel.elf
initrd.tar: $(BUILD_DIR)/initrd.tar
fat32.img:  $(BUILD_DIR)/fat32.img
virtio.img: $(BUILD_DIR)/virtio.img
build-iso:  $(BUILD_DIR)/AzamiOS.iso

# ──────────────────────────────────────────────────────────────────────────────
# Build rules inside $(BUILD_DIR)
# ──────────────────────────────────────────────────────────────────────────────

$(BUILD_DIR)/kernel.bin: $(BOOT_OBJ) ${OBJ}
	@mkdir -p $(dir $@)
	$(LD) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/kernel.elf: $(BOOT_OBJ) ${OBJ}
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/fat32.img:
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=32 status=none
	mkfs.fat -F 32 -n "AZAMIOS" $@
	echo "Hello from FAT32 inside AzamiOS!" > readme.txt
	mcopy -i $@ readme.txt ::README.TXT
	rm -f readme.txt

$(BUILD_DIR)/virtio.img:
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=16 status=none

$(BUILD_DIR)/initrd.tar: user/apps/wm/wm user/apps/shell/shell user/apps/cc/cc user/apps/glcube/glcube user/apps/cc/fib.c $(UTIL_TARGETS)
	@rm -rf $(BUILD_DIR)/user_bin
	@mkdir -p $(BUILD_DIR)/user_bin/bin $(BUILD_DIR)/user_bin/sbin $(BUILD_DIR)/user_bin/usr/bin $(BUILD_DIR)/user_bin/usr/sbin $(BUILD_DIR)/user_bin/usr/lib $(BUILD_DIR)/user_bin/usr/include $(BUILD_DIR)/user_bin/usr/share $(BUILD_DIR)/user_bin/usr/local/bin $(BUILD_DIR)/user_bin/home/root $(BUILD_DIR)/user_bin/root $(BUILD_DIR)/user_bin/etc $(BUILD_DIR)/user_bin/tmp $(BUILD_DIR)/user_bin/dev $(BUILD_DIR)/user_bin/proc $(BUILD_DIR)/user_bin/sys $(BUILD_DIR)/user_bin/var/log $(BUILD_DIR)/user_bin/var/run $(BUILD_DIR)/user_bin/var/tmp $(BUILD_DIR)/user_bin/boot $(BUILD_DIR)/user_bin/mnt $(BUILD_DIR)/user_bin/media
	cp user/apps/shell/shell $(BUILD_DIR)/user_bin/bin/shell
	for u in ls help cat write time clear whoami fps; do cp user/apps/$$u/$$u $(BUILD_DIR)/user_bin/bin/$$u; done
	for u in ifconfig ping arp lsmod reload cpu acpi reboot shutdown; do cp user/apps/$$u/$$u $(BUILD_DIR)/user_bin/sbin/$$u; done
	cp user/apps/wm/wm $(BUILD_DIR)/user_bin/usr/bin/wm
	cp user/apps/cc/cc $(BUILD_DIR)/user_bin/usr/bin/cc
	cp user/apps/glcube/glcube $(BUILD_DIR)/user_bin/usr/bin/glcube
	for u in about notepad files; do cp user/apps/$$u/$$u $(BUILD_DIR)/user_bin/usr/bin/$$u; done
	cp user/apps/cc/fib.c $(BUILD_DIR)/user_bin/home/root/fib.c
	echo "Welcome to AzamiOS v1.0 (microkernel)" > $(BUILD_DIR)/user_bin/etc/motd
	echo "azami-pc" > $(BUILD_DIR)/user_bin/etc/hostname
	printf "NAME=AzamiOS\nVERSION=1.0\nID=azamios\n" > $(BUILD_DIR)/user_bin/etc/os-release
	printf "root:x:0:0:root:/root:/bin/shell\nuser:x:1000:1000:user:/home/user:/bin/shell\n" > $(BUILD_DIR)/user_bin/etc/passwd
	printf "root:x:0:\nusers:x:1000:\n" > $(BUILD_DIR)/user_bin/etc/group
	printf "/dev/hda /fat32 vfat defaults 0 0\n/dev/sda /sata ext2 defaults 0 0\n" > $(BUILD_DIR)/user_bin/etc/fstab
	printf "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\n" > $(BUILD_DIR)/user_bin/etc/profile
	printf "AzamiOS 1.0 \n \l\n" > $(BUILD_DIR)/user_bin/etc/issue
	echo "System initialized." > $(BUILD_DIR)/user_bin/var/log/messages
	echo "Kernel boot complete." > $(BUILD_DIR)/user_bin/var/log/dmesg
	cd $(BUILD_DIR)/user_bin && tar --format=ustar -cf ../initrd.tar bin sbin usr home root etc tmp dev proc sys var boot mnt media


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
run: $(KERNEL_TARGET) $(BUILD_DIR)/initrd.tar $(BUILD_DIR)/fat32.img $(BUILD_DIR)/virtio.img
	$(QEMU_CMD) -initrd $(BUILD_DIR)/initrd.tar -hda $(BUILD_DIR)/fat32.img \
	  -drive if=none,id=vdisk,file=$(BUILD_DIR)/virtio.img,format=raw -device virtio-blk-pci,drive=vdisk \
	  -smp 4 -serial stdio -device ac97 -device rtl8139 -device e1000 -device pcnet -device ne2k_pci -device ES1370 -device ahci -device virtio-net-pci -device virtio-rng-pci

run-debug: $(KERNEL_TARGET)
	$(QEMU_CMD) -smp 4 -serial stdio

run-iso:
	$(MAKE) clean
	$(MAKE) ARCH=i686 $(BUILD_DIR)/AzamiOS.iso $(BUILD_DIR)/fat32.img $(BUILD_DIR)/virtio.img
	qemu-system-i386 -vga std -accel tcg,thread=multi -cdrom $(BUILD_DIR)/AzamiOS.iso -hda $(BUILD_DIR)/fat32.img -boot d \
	  -drive if=none,id=vdisk,file=$(BUILD_DIR)/virtio.img,format=raw -device virtio-blk-pci,drive=vdisk \
	  -smp 4 -serial stdio -device ac97 -device rtl8139 -device e1000 -device pcnet -device ne2k_pci -device ES1370 -device ahci -device virtio-net-pci -device virtio-rng-pci

run-uefi:
	$(MAKE) clean
	$(MAKE) ARCH=x86_64 run




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
	nasm $< -f $(NASM_FMT) -o $@

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