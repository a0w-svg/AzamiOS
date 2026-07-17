# ──────────────────────────────────────────────────────────────────────────────
# AzamiOS Makefile — 64-Bit Modern Microkernel OS
#
# Layout:
#   lib/          — kernel-independent pure-C code (string, stdlib, net, fs, gfx)
#   kernel/       — Ring-0 64-bit kernel (links against lib/ objects)
#   user/         — userspace programs (link against user/libc)
#   build/        — output directory for all build artifacts & objects
# ──────────────────────────────────────────────────────────────────────────────

BUILD_DIR = build
ARCH = x86_64
export ARCH
OBJ_DIR = $(BUILD_DIR)/obj-$(ARCH)

UTIL_LIST = ls help cat write time clear ifconfig ping arp lsmod reload cpu whoami fps acpi reboot shutdown about notepad files mount lpc testarch grep find sed awk du df head tail wc sort uniq python
UTIL_TARGETS = $(foreach u,$(UTIL_LIST),user/apps/$(u)/$(u))

# ── Kernel C sources ──────────────────────────────────────────────────────────
C_SOURCES = $(wildcard kernel/arch/*.c \
                        kernel/hal/*.c kernel/hal/*/*.c \
                        kernel/boot/*.c \
                        kernel/klibc/*.c \
                        kernel/klibc/stdio/*.c \
                        kernel/mem/*.c \
                        kernel/syscall/*.c \
                        kernel/filesystem/*.c \
                        kernel/proc/*.c \
                        kernel/module/*.c \
                        kernel/drivers/*.c \
                        kernel/azami/*.c kernel/azami/core/*.c kernel/azami/lxss/*.c kernel/azami/drivers/*.c) \
            $(filter-out kernel/drivers/display/gfx.c kernel/drivers/char/terminal.c, \
              $(wildcard kernel/drivers/*/*.c))

# ── Headers (for dependency tracking) ────────────────────────────────────────
HEADERS = $(wildcard kernel/arch/include/*.h \
                      kernel/boot/*.h \
                      kernel/klibc/include/*.h \
                      kernel/drivers/include/*.h \
                      kernel/mem/include/*.h \
                      kernel/syscall/include/*.h \
                      kernel/filesystem/include/*.h \
                      kernel/proc/include/*.h \
                      kernel/module/include/*.h \
                      kernel/azami/include/*.h \
                      kernel/hal/*.h kernel/hal/include/*.h \
                      lib/string/string.h \
                      lib/stdlib/stdlib.h \
                      lib/net/net_stack.h \
                      lib/net/net_hal.h \
                      lib/net/packet_buffer.h \
                      lib/fs/vfs.h \
                      lib/fs/tarfs.h \
                      lib/gfx/gfx_blit.h \
                      lib/gfx/game_engine.h)

# ── Object files (mapped into build/obj-$(ARCH)/) ────────────────────────────
C_OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(C_SOURCES))

# ── Cross-compiler toolchain (x86_64) ─────────────────────────────────────────
CROSS_PREFIX = $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC    = $(CROSS_PREFIX)gcc
LD    = $(CROSS_PREFIX)ld
GDB   = $(CROSS_PREFIX)gdb
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -m64 -mno-red-zone -mcmodel=large -fno-stack-protector \
         -O2 -fno-omit-frame-pointer -fno-strict-aliasing -funroll-loops \
         -I. -Ikernel -Ilib -Ikernel/drivers -Ikernel/drivers/include -Ikernel/azami -Ikernel/azami/include -Ikernel/hal -Ikernel/hal/include
LDFLAGS = -T kernel.ld --no-warn-rwx-segments
NASM_FMT = elf64
BOOT_OBJ = $(OBJ_DIR)/kernel/boot/boot.o
OBJ = $(C_OBJS) \
      $(OBJ_DIR)/kernel/arch/cpu.o \
      $(OBJ_DIR)/kernel/arch/interrupts.o \
      $(OBJ_DIR)/kernel/arch/smp_boot.o \
      $(OBJ_DIR)/kernel/hal/cpu/cpu_asm.o \
      $(OBJ_DIR)/kernel/hal/gdt/gdt_asm.o
KERNEL_TARGET = $(BUILD_DIR)/kernel.elf
QEMU_CMD = qemu-system-x86_64 -m 1G -M q35 -vga std -accel tcg,thread=multi -kernel $(BUILD_DIR)/kernel.elf

# ── Host compiler (for lib/ unit tests) ──────────────────────────────────────
HOST_CC     = gcc
HOST_CFLAGS = -Wall -Wextra -O2 -I. -Ilib

# ──────────────────────────────────────────────────────────────────────────────
# Primary aliases / phony targets
# ──────────────────────────────────────────────────────────────────────────────
.PHONY: all run run-iso run-uefi run-debug build-iso kernel.elf initrd.tar fat32.img clean test-lib test-automated kernel/azami/azami_kernel.a
.PHONY: user/apps/wm/wm user/apps/shell/shell user/apps/cc/cc user/apps/glcube/glcube $(UTIL_TARGETS)

all: $(KERNEL_TARGET) $(BUILD_DIR)/initrd.tar kernel/azami/azami_kernel.a

kernel/azami/azami_kernel.a:
	$(MAKE) -C kernel/azami CC="$(CC)" AR="$(CROSS_PREFIX)ar" CFLAGS="-std=c11 -ffreestanding -Wall -Wextra -Werror -O2 -Iinclude -I../../include -I.."

test-automated:
	@bash scripts/test_automated.sh

kernel.elf: $(BUILD_DIR)/kernel.elf
initrd.tar: $(BUILD_DIR)/initrd.tar
fat32.img:  $(BUILD_DIR)/fat32.img
virtio.img: $(BUILD_DIR)/virtio.img
build-iso:  $(BUILD_DIR)/AzamiOS.iso

# ──────────────────────────────────────────────────────────────────────────────
# Build rules inside $(BUILD_DIR)
# ──────────────────────────────────────────────────────────────────────────────
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
	for u in ls help cat write time clear whoami fps mount lpc testarch grep find sed awk du df head tail wc sort uniq python; do cp user/apps/$$u/$$u $(BUILD_DIR)/user_bin/bin/$$u; done
	for u in ifconfig ping arp lsmod reload cpu acpi reboot shutdown; do cp user/apps/$$u/$$u $(BUILD_DIR)/user_bin/sbin/$$u; done
	cp user/apps/wm/wm $(BUILD_DIR)/user_bin/usr/bin/wm
	cp user/apps/cc/cc $(BUILD_DIR)/user_bin/usr/bin/cc
	cp user/apps/glcube/glcube $(BUILD_DIR)/user_bin/usr/bin/glcube
	for u in about notepad files; do cp user/apps/$$u/$$u $(BUILD_DIR)/user_bin/usr/bin/$$u; done
	cp user/apps/cc/fib.c $(BUILD_DIR)/user_bin/home/root/fib.c
	echo "Welcome to AzamiOS v6.0 Modern Microkernel OS (Win10 DE / GNU / Python / Games)" > $(BUILD_DIR)/user_bin/etc/motd
	echo "azami-pc" > $(BUILD_DIR)/user_bin/etc/hostname
	printf "NAME=AzamiOS\nVERSION=6.0\nID=azamios\n" > $(BUILD_DIR)/user_bin/etc/os-release
	printf "root:x:0:0:root:/root:/bin/shell\nuser:x:1000:1000:user:/home/user:/bin/shell\n" > $(BUILD_DIR)/user_bin/etc/passwd
	printf "root:x:0:\nusers:x:1000:\n" > $(BUILD_DIR)/user_bin/etc/group
	printf "/dev/hda /fat32 vfat defaults 0 0\n/dev/sda /sata ext2 defaults 0 0\n" > $(BUILD_DIR)/user_bin/etc/fstab
	printf "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\n" > $(BUILD_DIR)/user_bin/etc/profile
	printf "AzamiOS v6.0 \n \l\n" > $(BUILD_DIR)/user_bin/etc/issue
	echo "System initialized." > $(BUILD_DIR)/user_bin/var/log/messages
	echo "Kernel boot complete." > $(BUILD_DIR)/user_bin/var/log/dmesg
	cd $(BUILD_DIR)/user_bin && tar --format=ustar -cf ../initrd.tar bin sbin usr home root etc tmp dev proc sys var boot mnt media

$(BUILD_DIR)/AzamiOS.iso: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/initrd.tar boot/grub/menu.lst
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp boot/grub/stage2_eltorito $(BUILD_DIR)/iso/boot/grub/
	cp $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/iso/boot/
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

# ── Userspace apps & library ──────────────────────────────────────────────────
user/libc/libc.a:
	$(MAKE) -C user/libc ARCH=$(ARCH)

user/apps/wm/wm: user/libc/libc.a
	$(MAKE) -C user/apps/wm ARCH=$(ARCH)
user/apps/shell/shell: user/libc/libc.a
	$(MAKE) -C user/apps/shell ARCH=$(ARCH)
user/apps/cc/cc: user/libc/libc.a
	$(MAKE) -C user/apps/cc ARCH=$(ARCH)
user/apps/glcube/glcube: user/libc/libc.a
	$(MAKE) -C user/apps/glcube ARCH=$(ARCH)
$(UTIL_TARGETS): user/libc/libc.a
	$(MAKE) -C $(dir $@) ARCH=$(ARCH)

# ── Execution targets ─────────────────────────────────────────────────────────
run: $(KERNEL_TARGET) $(BUILD_DIR)/initrd.tar $(BUILD_DIR)/fat32.img $(BUILD_DIR)/virtio.img
	$(QEMU_CMD) -initrd $(BUILD_DIR)/initrd.tar -hda $(BUILD_DIR)/fat32.img \
	  -drive if=none,id=vdisk,file=$(BUILD_DIR)/virtio.img,format=raw -device virtio-blk-pci,drive=vdisk \
	  -smp 4 -serial file:kernel.log -device ac97 -device rtl8139 -device e1000 -device pcnet -device ne2k_pci -device ES1370 -device ahci -device virtio-net-pci -device virtio-rng-pci

run-debug: $(KERNEL_TARGET) $(BUILD_DIR)/initrd.tar $(BUILD_DIR)/fat32.img $(BUILD_DIR)/virtio.img
	$(QEMU_CMD) -initrd $(BUILD_DIR)/initrd.tar -hda $(BUILD_DIR)/fat32.img \
	  -drive if=none,id=vdisk,file=$(BUILD_DIR)/virtio.img,format=raw -device virtio-blk-pci,drive=vdisk \
	  -smp 4 -serial file:kernel.log -device ac97 -device rtl8139 -device e1000 -device pcnet -device ne2k_pci -device ES1370 -device ahci -device virtio-net-pci -device virtio-rng-pci -s -S

gdb:
	$(GDB) -ex "target remote localhost:1234" -ex "symbol-file $(KERNEL_TARGET)"

run-iso: run
run-uefi: run

# ── lib/ host-compiler unit test target ──────────────────────────────────────
LIB_SOURCES = lib/string/string.c \
              lib/stdlib/stdlib.c \
              lib/fs/vfs.c \
              lib/fs/tarfs.c \
              lib/gfx/gfx_blit.c \
              lib/gfx/gfx_spooler.c \
              lib/gfx/game_engine.c \
              lib/net/net_stack.c \
              lib/net/packet_buffer.c

test-lib:
	@echo "==> Compiling lib/ with host gcc to check kernel-independence..."
	@for src in $(LIB_SOURCES); do \
	  echo "  CC $$src"; \
	  $(HOST_CC) $(HOST_CFLAGS) -c $$src -o /dev/null 2>&1 || exit 1; \
	  done
	@echo "==> lib/ test-lib PASSED: no kernel dependencies found."

# ── Compilation rules ─────────────────────────────────────────────────────────
$(OBJ_DIR)/%.o: %.c ${HEADERS}
	@mkdir -p $(dir $@)
	${CC} ${CFLAGS} -c $< -o $@

$(OBJ_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	nasm $< -f $(NASM_FMT) -o $@

$(BUILD_DIR)/%.bin: %.asm
	@mkdir -p $(dir $@)
	nasm $< -f bin -o $@

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) kernel.elf AzamiOS.iso *.bin *.dis *.log initrd.tar fat32.img iso/ user/bin/ lib/*/*.o
	$(MAKE) -C user/libc clean
	$(MAKE) -C user/apps/wm clean
	$(MAKE) -C user/apps/shell clean
	$(MAKE) -C user/apps/cc clean
	$(MAKE) -C user/apps/glcube clean
	$(MAKE) -C kernel/azami clean
	for u in $(UTIL_LIST); do $(MAKE) -C user/apps/$$u clean; done